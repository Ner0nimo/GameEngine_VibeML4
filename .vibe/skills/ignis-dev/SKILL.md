---
name: ignis-dev
description: Load this skill when working on the Ignis 3D game engine in this repository (C++20/CMake, ECS, Lua scripting, reactphysics3d, miniaudio, NVRHI/Vulkan renderer, GTest tests, CI). It contains the build/test commands, module map, code style and commit rules for the engine.
---

# Ignis engine development

Spec: `GameEngineDoc.md`. Guidelines: `AGENTS.md` (read it first — it is the
authoritative dev guide and this skill summarizes it).

## Build & test (always before committing)

```bash
cmake --preset headless && cmake --build --preset headless && ctest --preset headless
# full (editor/Vulkan) needs: sudo zypper install libX11-devel libXrandr-devel \
#   libXinerama-devel libXcursor-devel libXi-devel libxkbcommon-devel \
#   wayland-devel wayland-protocols-devel vulkan-devel glslang alsa-devel libpulse-devel
cmake --preset full && cmake --build --preset full && ctest --preset full
```

Headless preset works without GPU/X11. Tests run with CWD = repo root.
`-Werror` is on for all Ignis targets; third-party headers are SYSTEM includes.

## Module map (dependencies point downward only)

`engine/` ignis_core (ECS World/View, scene JSON + prefabs, gltf/stb assets,
IGPK pak, JSON agent commands) ← `scripting/` (Lua 5.4, `ignis.*` API),
`physics/` (reactphysics3d, `ignis.physics.*`), `audio/` (miniaudio,
`ignis.audio.*`) ← `window/` (GLFW), `renderer/` (NVRHI/Vulkan, picking, IBL)
← `runtime/` (`ignis_runtime`, `--headless`/`--pak`), `editor/`
(`ignis_editor`, graphics builds only), `tools/exporter/` (`ignis_export`).

## Conventions

- namespace `ignis`; scalar types from `core/types.h` (`u32`, `f32`); explicit
  `static_cast` (sign-conversion is an error).
- New component: struct + `ComponentIO<T>` specialization + registration via
  `world.register_component_type<T>("Name")` (examples: `ecs/components.h`,
  `physics/physics_world.h`).
- New feature: failing test first in `tests/src/test_*.cpp`; if user-visible,
  extend `scenes/test_scene.scene.json` / `scripts/mover.lua` so the
  integration test (`Integration.TestSceneAllFeatures`) covers it.
- Editor is AI-controllable via JSON commands (`ignis/agent/commands.h`):
  `ignis_runtime --commands file.jsonl scene.json`, one JSON command per line.

## Commit rules

Review code before committing (style, warnings, tests green), short imperative
commit message, push to `main` of `github.com/Ner0nimo/GameEngine_VibeML4` only
after `ctest` passes. Never commit `build/`.
