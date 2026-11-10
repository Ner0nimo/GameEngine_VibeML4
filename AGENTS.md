# AGENTS.md — Ignis Game Engine

Concrete development guidelines for AI agents (and humans) working on Ignis.
Read this before changing anything. The product spec is `GameEngineDoc.md`.

## Project identity

- Engine name: **Ignis**, namespace `ignis`, CMake targets `ignis_core`,
  `ignis_scripting`, `ignis_physics`, `ignis_audio`, `ignis_window`,
  `ignis_renderer`, exes `ignis_runtime`, `ignis_editor`, tool `ignis_export`.
- Remote: `https://github.com/Ner0nimo/GameEngine_VibeML4` (private).
- C++20, CMake ≥ 3.27, Ninja. Deps are pinned via FetchContent in
  `cmake/Dependencies.cmake` — never add unpinned deps.

## Build & test (do this before every commit)

```bash
# Headless (works everywhere, no GPU/X11 needed):
cmake --preset headless && cmake --build --preset headless && ctest --preset headless

# Full (editor + graphics; needs X11/Wayland dev + vulkan-devel + glslang):
#   sudo zypper install libX11-devel libXrandr-devel libXinerama-devel \
#     libXcursor-devel libXi-devel libxkbcommon-devel wayland-devel \
#     wayland-protocols-devel vulkan-devel glslang alsa-devel libpulse-devel
cmake --preset full && cmake --build --preset full && ctest --preset full

# Release distribution build (runtime only, editor stripped):
cmake --preset release && cmake --build --preset release
```

- All tests must pass (`ctest`). Tests run with CWD = repo root.
- Warnings are errors (`-Werror`) on all Ignis targets. Third-party headers
  are SYSTEM includes; never add warnings to third-party targets.

## Architecture

```
engine/     ignis_core: log, math (glm), ECS (World/View/components),
            scene (JSON serialize, prefabs), assets (gltf/cgltf, textures/stb),
            pak (IGPK packages + sha256), agent commands (JSON command API)
scripting/  ignis_scripting: Lua 5.4 engine, full ECS/scene bindings (ignis.*)
physics/    ignis_physics: reactphysics3d wrapper, RigidBody/Collider components,
            raycast + collision events, ignis.physics.* Lua bindings
audio/      ignis_audio: miniaudio engine, decode helpers, null-device mode,
            AudioSource component + AudioSystem (spatial 3D audio, pak-aware
            playback), ignis.audio.* Lua bindings
window/     ignis_window: GLFW wrapper (Vulkan surface in graphics builds)
renderer/   ignis_renderer: IRenderer iface, VulkanRenderer (NVRHI), picking,
            IBL (CPU equirect->irradiance), debug lines
runtime/    ignis_runtime exe: --headless for CI/tests, --pak for exported games
editor/     ignis_editor exe (graphics builds only): window + renderer + UI
tools/exporter/  ignis_export: packs scene+assets+scripts into game.ignispak
tests/      GTest suites + integration test (scenes/test_scene.scene.json)
scenes/     scene files (JSON)
scripts/    Lua scripts referenced by scenes
assets/     test assets (wav, ...)
```

Module dependency rule: lower modules never depend on higher ones
(core ← scripting/physics/audio ← window/renderer ← runtime/editor).

## Code style

- C++20, `namespace ignis`, types from `core/types.h` (`u32`, `f32`, ...).
- Explicit `static_cast` everywhere (sign-conversion is an error).
- Headers in `include/ignis/...`, sources in `src/...`, one class per file.
- No exceptions across module boundaries; log via `IGNIS_ERROR/WARN/INFO`.
- New components: define struct + `ComponentIO<T>` specialization, register
  with `world.register_component_type<T>("Name")` (see `components.h`,
  `physics_world.h`).

## Function design rules (the three rules)

Every function in Ignis follows these three rules. When auditing or writing
code, check each function against all three.

### 1. Build your system out of honest functions; inject dishonesty at the topmost possible level

A function does exactly what its name and contract say — no silent coercions,
no hidden fallbacks, no magic sentinel behavior buried inside. When the real
world forces dishonesty (a driver may crash, a file may be corrupt, an input
may be invalid), handle it as high up the call stack as possible (usually
`main()` / `EditorApp::run()` / a top-level orchestrator) and let lower levels
report failure honestly instead of papering over it.

- `World::set_parent` returns `bool` and refuses dead parents and cycles at
  write time; `world_matrix` then trusts the acyclic invariant instead of
  carrying a magic "guard < 1024" loop.
- `Scene::load_file` really loads INTO the scene (keeps registered systems)
  instead of silently doing `*this = deserialize(...)`.
- Low-level functions return `Result<T>` and never swallow errors; the
  top-level caller decides what to do with `r.error()`. The fork-based Vulkan
  probe lives in `EditorApp::run`/`main` — that is the topmost level where
  "the driver might segfault" belongs.

### 2. Signatures should communicate and show empathy for callers

The signature alone should tell the caller what the function does, what it
needs, and how it can fail. If a caller has to declare a temp string just to
receive an error, or guess whether `0`/`-1`/`nullptr` means failure, the
signature is not empathetic.

- Prefer `Result<T>` (see `core/result.h`) over `bool f(..., std::string& err)`:
  `Result<void> AudioEngine::init(bool null_device)` — one object carries
  success and message: `if (auto r = audio.init(true); !r.ok()) log(r.error());`
- Prefer returning values over out-params:
  `Result<std::vector<u8>> PakReader::read(path)` instead of
  `bool read(path, std::vector<u8>& out)`;
  `picking::Ray screen_ray(cam, aspect, x, y)` instead of two out-params;
  `std::optional<f32> ray_aabb(...)` instead of a `-1` sentinel;
  `FramebufferSize framebuffer_size()` instead of `void f(u32&, u32&)`.

### 3. Stay at one level of abstraction

A function body should read like a summary of what happens; every step that
belongs to a lower level of detail goes into its own named function one level
down. Don't mix "create window" with "fill in VkInstanceCreateInfo".

- `ScriptEngine::update` iterates script entities and delegates the actual
  call to `run_callback(env_ref, "on_update", true, dt)`.
- `AudioSystem::update` = `stop_orphaned_sources(world)` + `update_source(e, world)` per entity.
- `EditorApp::run` is split into `init_*` helpers and the main loop instead of
  one 400-line body mixing window, renderer, ImGui and UI code.

## Adding a feature (workflow)

1. Write failing unit tests first (`tests/src/test_*.cpp`).
2. Implement in the lowest module that fits.
3. If it is user-visible in a scene, extend `scenes/test_scene.scene.json`
   and/or `scripts/mover.lua` so the integration test covers it.
4. `ctest` green, then commit.

## Commit rules (from GameEngineDoc.md)

- Code review before every commit: check style, warnings, tests.
- Commit message: short imperative summary; body explains why.
- Never commit build/ directories (see .gitignore).
- Push to `origin main` of Ner0nimo/GameEngine_VibeML4 only after tests pass.

## AI-agent control of the editor

`ignis/agent/commands.h` exposes every editor action as a JSON command
(create_entity, add_component, spawn_prefab, save_scene, run_commands, ...).
Editor/runtime accept `--commands file.jsonl` (one JSON per line). This is the
intended interface for "build me a game" requests.
