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

#include <cstdio>
#include <cstring>
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
        else if (a == "--frames") out.frames = std::stoi(next("--frames"));
        else if (a == "--dt") out.dt = std::stof(next("--dt"));
        else if (a == "--pak") out.pak_path = next("--pak");
        else if (a == "--commands") out.commands_path = next("--commands");
        else if (a == "--dump") out.dump_path = next("--dump");
        else if (!a.empty() && a[0] != '-') out.scene_path = a;
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return false;
        }
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
        if (!pak.open(args.pak_path)) {
            IGNIS_ERROR("cannot open pak '{}'", args.pak_path);
            return 1;
        }
        assets.set_pak(&pak);
    }

    // --- scene --------------------------------------------------------------
    // Register components first (registry is global), then load the scene.
    Scene scene;
    register_builtin_components(scene.world());
    PhysicsSystem::register_components(scene.world());

    bool ok = false;
    if (!args.pak_path.empty()) {
        std::vector<u8> bytes;
        if (!pak.read("scene.json", bytes)) {
            IGNIS_ERROR("pak '{}' has no scene.json", args.pak_path);
            return 1;
        }
        std::string text(bytes.begin(), bytes.end());
        scene = Scene::deserialize(nlohmann::json::parse(text));
        ok = true;
    } else {
        ok = scene.load_file(args.scene_path);
    }
    if (!ok) {
        IGNIS_ERROR("cannot load scene '{}'", args.scene_path);
        return 1;
    }

    // --- systems --------------------------------------------------------------
    ScriptEngine scripts;
    {
        std::string err;
        if (!scripts.init(&scene, &assets, err)) {
            IGNIS_ERROR("script engine init failed: {}", err);
            return 1;
        }
    }
    AudioEngine audio;
    {
        std::string err;
        audio.init(args.headless, err); // null device when headless
        AudioEngine::register_lua_bindings(scripts.state());
    }
    PhysicsSystem physics(scene);
    {
        std::string err;
        if (!physics.init({0.f, -9.81f, 0.f}, &err)) {
            IGNIS_ERROR("physics init failed: {}", err);
            return 1;
        }
        PhysicsSystem::register_lua_bindings(scripts.state());
    }

    scene.add_system([&](World&, f32 dt) { scripts.update(dt); });
    scene.add_system([&](World& w, f32 dt) { physics.update(w, dt); });

    // --- agent commands (AI-controllable) ------------------------------------
    if (!args.commands_path.empty()) {
        CommandProcessor proc(scene, assets);
        auto results = proc.execute_file(args.commands_path);
        for (const auto& r : results) {
            IGNIS_INFO("cmd: {} ({})", r.message, r.ok ? "ok" : "FAIL");
        }
    }

    // --- run ------------------------------------------------------------------
    if (args.headless) {
        for (int i = 0; i < args.frames; ++i) {
            scene.update(args.dt);
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
    std::string err;
    if (!window.init(err)) {
        IGNIS_ERROR("window init failed: {}", err);
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
                std::string probeErr;
                if (!probeRenderer.init_offscreen(64, 64, probeErr)) _exit(2);
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
    if (!renderer.init(&window, err)) {
        IGNIS_ERROR("renderer init failed: {}", err);
        return 1;
    }
    // Find active camera entity.
    Camera camera;
    while (!window.should_close()) {
        window.poll_events();
        const f32 dt = window.delta_time();
        scene.update(dt);
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
        u32 w = 0, h = 0;
        window.framebuffer_size(w, h);
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
