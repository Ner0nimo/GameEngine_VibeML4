// ignis-runtime: runs a scene (game) without editing capabilities.
//
// Usage:
//   ignis-runtime <scene.json> [--frames N] [--dt 1/60] [--commands cmds.jsonl]
//                 [--dump out.json] [--pak game.ignispak]
//   ignis-runtime --headless <scene.json> --frames 120 --dump state.json
//
// --headless runs the simulation (scripting + physics) without a window or
// GPU and is what the automated feature-test scene uses.

#include "ignis/agent/commands.h"
#include "ignis/assets/asset_manager.h"
#include "ignis/audio/audio_engine.h"
#include "ignis/audio/audio_system.h"
#include "ignis/core/log.h"
#include "ignis/core/pak.h"
#include "ignis/ecs/components.h"
#include "ignis/physics/physics_world.h"
#include "ignis/scene/scene.h"
#include "ignis/scripting/script_engine.h"

#ifdef IGNIS_WITH_GRAPHICS
#include "ignis/renderer/renderer.h"
#include "ignis/renderer/vulkan_renderer.h"
#include "ignis/window/window.h"
#endif

#include "ignis/core/json_parse.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef IGNIS_WITH_GRAPHICS
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ignis;

namespace {

struct Args {
    std::string scene_path;
    std::string pak_path;
    std::string commands_path;
    std::string dump_path;
    bool headless = false;
    int frames = 1;
    f32 dt = 1.f / 60.f;
};

bool parse_args(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--headless") out.headless = true;
        else if (a == "--frames") {
            try { out.frames = std::stoi(next("--frames")); }
            catch (const std::exception&) {
                std::fprintf(stderr, "invalid --frames value (not an integer)\n");
                return false;
            }
        }
        else if (a == "--dt") {
            try { out.dt = std::stof(next("--dt")); }
            catch (const std::exception&) {
                std::fprintf(stderr, "invalid --dt value (not a number)\n");
                return false;
            }
        }
        else if (a == "--pak") out.pak_path = next("--pak");
        else if (a == "--commands") out.commands_path = next("--commands");
        else if (a == "--dump") out.dump_path = next("--dump");
        else if (!a.empty() && a[0] != '-') out.scene_path = a;
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return false;
        }
    }
    // Never trust externally supplied simulation parameters.
    constexpr int kMaxFrames = 10000000;
    constexpr f32 kMaxDt = 10.f;
    if (out.frames < 1 || out.frames > kMaxFrames) {
        std::fprintf(stderr, "--frames must be in [1, %d]\n", kMaxFrames);
        return false;
    }
    if (!std::isfinite(out.dt) || out.dt <= 0.f || out.dt > kMaxDt) {
        std::fprintf(stderr, "--dt must be finite and in (0, %f]\n", static_cast<double>(kMaxDt));
        return false;
    }
    // Scene path may be omitted when loading from a pak (scene.json inside).
    return !out.scene_path.empty() || !out.pak_path.empty();
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        std::fprintf(stderr,
            "usage: ignis-runtime [--headless] [--pak p.ignispak] [--frames N] [--dt s]\n"
            "                     [--commands c.jsonl] [--dump out.json] <scene.json>\n");
        return 2;
    }

    Logger::instance().set_level(LogLevel::Info);

    // --- assets (+ optional pak) -------------------------------------------
    AssetManager assets;
    PakReader pak;
    if (!args.pak_path.empty()) {
        auto opened = pak.open(args.pak_path);
        if (!opened.ok()) {
            IGNIS_ERROR("{}", opened.error());
            return 1;
        }
        assets.set_pak(&pak);
        // Exported game: the package is the only asset source, never the host FS.
        assets.set_access_mode(AssetAccessMode::PackageOnly);
    } else {
        // Development runtime: assets resolve inside the project (CWD) only.
        assets.set_access_mode(AssetAccessMode::ProjectFilesystem);
        assets.set_project_root(std::filesystem::current_path().string());
    }

    // --- scene --------------------------------------------------------------
    // Register components first (registry is global), then load the scene.
    Scene scene;
    register_builtin_components(scene.world());
    PhysicsSystem::register_components(scene.world());
    AudioSystem::register_components(scene.world());

    if (!args.pak_path.empty()) {
        auto bytes = pak.read("scene.json");
        if (!bytes.ok()) {
            IGNIS_ERROR("pak '{}' has no scene.json ({})", args.pak_path, bytes.error());
            return 1;
        }
        std::string text(bytes.value().begin(), bytes.value().end());
        auto parsed = parse_json_bounded(text);
        if (!parsed.ok()) {
            IGNIS_ERROR("cannot parse scene.json in pak: {}", parsed.error());
            return 1;
        }
        try {
            scene = Scene::deserialize(parsed.value());
        } catch (const std::exception& e) {
            IGNIS_ERROR("malformed scene.json in pak: {}", e.what());
            return 1;
        }
    } else {
        auto loaded = scene.load_file(args.scene_path);
        if (!loaded.ok()) {
            IGNIS_ERROR("{}", loaded.error());
            return 1;
        }
    }

    // --- systems --------------------------------------------------------------
    ScriptEngine scripts;
    if (auto r = scripts.init(&scene, &assets); !r.ok()) {
        IGNIS_ERROR("script engine init failed: {}", r.error());
        return 1;
    }
    // Lua path bindings (ignis.scene.save) follow the same policy as assets:
    // packaged runtime -> no host paths; dev runtime -> project root only.
    if (!args.pak_path.empty()) {
        scripts.set_host_paths_allowed(false);
    } else {
        scripts.set_project_root(std::filesystem::current_path().string());
    }
    AudioEngine audio;
    if (auto r = audio.init(args.headless); !r.ok()) { // null device when headless
        IGNIS_ERROR("audio init failed: {}", r.error());
        return 1;
    }
    audio.set_asset_manager(&assets); // pak-aware playback
    AudioEngine::register_lua_bindings(scripts.state());
    AudioSystem audio_sys(audio, &scene);
    AudioSystem::register_lua_bindings(scripts.state());
    PhysicsSystem physics(scene);
    if (auto r = physics.init(scene.settings().gravity); !r.ok()) {
        IGNIS_ERROR("physics init failed: {}", r.error());
        return 1;
    }
    PhysicsSystem::register_lua_bindings(scripts.state());

    scene.add_system([&](World&, f32 dt) { scripts.update(dt); });
    scene.add_system([&](World& w, f32 dt) { physics.update(w, dt); });
    scene.add_system([&](World& w, f32 dt) { audio_sys.update(w, dt); });

    // Keep the 3D audio listener on the active camera.
    auto sync_listener = [&]() {
        for (EntityId e : scene.world().view<CameraComponent, TransformComponent>()) {
            const auto* cc = scene.world().get<CameraComponent>(e);
            const auto* tc = scene.world().get<TransformComponent>(e);
            if (cc && cc->active && tc) {
                const math::Vec3 fwd = tc->rotation * math::Vec3(0.f, 0.f, -1.f);
                audio_sys.set_listener(tc->position, fwd);
            }
        }
    };

    // --- agent commands (AI-controllable) ------------------------------------
    if (!args.commands_path.empty()) {
        CommandProcessor proc(scene, assets);
        if (!args.pak_path.empty()) {
            proc.set_host_paths_allowed(false); // exported game: no host paths
        } else {
            proc.set_project_root(std::filesystem::current_path().string());
        }
        auto results = proc.execute_file(args.commands_path);
        for (const auto& r : results) {
            IGNIS_INFO("cmd: {} ({})", r.message, r.ok ? "ok" : "FAIL");
        }
    }

    // --- run ------------------------------------------------------------------
    if (args.headless) {
        for (int i = 0; i < args.frames; ++i) {
            scene.update(args.dt * scene.settings().time_scale); // sim time only
            sync_listener();
        }
        if (!args.dump_path.empty()) {
            std::ofstream f(args.dump_path);
            f << scene.serialize().dump(2);
            IGNIS_INFO("dumped state to {}", args.dump_path);
        }
        std::printf("%s", scene.serialize().dump(2).c_str());
        return 0;
    }

#ifdef IGNIS_WITH_GRAPHICS
    // Graphics path: window + Vulkan renderer.
    Window window({args.scene_path, 1280, 720, true, true});
    if (auto r = window.init(); !r.ok()) {
        IGNIS_ERROR("window init failed: {}", r.error());
        return 1;
    }

    {
        // Probe the Vulkan stack in a forked child first: on some driver stacks
        // the driver itself crashes when NVRHI calls into it, and the runtime
        // must fail with a clear message instead of a segfault.
        auto probe = []() -> bool {
            const pid_t pid = fork();
            if (pid == 0) {
                VulkanRenderer probeRenderer;
                if (!probeRenderer.init_offscreen(64, 64).ok()) _exit(2);
                probeRenderer.shutdown();
                _exit(0);
            }
            int status = 0;
            waitpid(pid, &status, 0);
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        };
        if (!probe()) {
            IGNIS_ERROR("Vulkan/NVRHI device not usable on this system (driver crash or no GPU). "
                        "Run with --headless for simulation-only mode.");
            return 1;
        }
    }
    VulkanRenderer renderer;
    if (auto r = renderer.init(&window); !r.ok()) {
        IGNIS_ERROR("renderer init failed: {}", r.error());
        return 1;
    }
    // Framebuffer resize -> renderer records a pending resize; the next
    // render() recreates the swapchain after waitForIdle (issue #53).
    window.on_resize = [&renderer](u32 w, u32 h) { renderer.request_resize(w, h); };
    // Find active camera entity.
    Camera camera;
    while (!window.should_close()) {
        window.poll_events();
        const f32 dt = window.delta_time();
        scene.update(dt * scene.settings().time_scale); // sim time only, not frame rate
        sync_listener();
        for (EntityId e : scene.world().view<CameraComponent, TransformComponent>()) {
            const auto* cc = scene.world().get<CameraComponent>(e);
            const auto* tc = scene.world().get<TransformComponent>(e);
            if (cc && cc->active && tc) {
                camera.position = tc->position;
                camera.rotation = tc->rotation;
                camera.fov_y_deg = cc->fov_y_deg;
                camera.z_near = cc->z_near;
                camera.z_far = cc->z_far;
            }
        }
        const FramebufferSize fb = window.framebuffer_size();
        const u32 w = fb.width, h = fb.height;
        if (w > 0 && h > 0) {
            renderer.render(scene, camera, dt);
            renderer.present();
        }
    }
    renderer.shutdown();
    window.shutdown();
    return 0;
#else
    IGNIS_ERROR("this build has no graphics support; use --headless");
    return 1;
#endif
}
