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
            ignis.audio.* Lua bindings
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
