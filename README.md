# Ignis

Production-grade 3D game engine for Windows, macOS and Linux (Ubuntu 24+).
C++20 / CMake, Vulkan via NVRHI, GLFW windowing, Lua scripting, ECS,
reactphysics3d physics, miniaudio audio. Spec: [GameEngineDoc.md](GameEngineDoc.md).
Development guidelines: [AGENTS.md](AGENTS.md).

## Quickstart

```bash
# configure + build + test (headless, no GPU needed)
cmake --preset headless
cmake --build --preset headless
ctest --preset headless --output-on-failure

# run the feature-test scene headless (scripting + physics + audio + commands)
./build/headless/runtime/ignis_runtime --headless --frames 300 \
    --commands scripts/commands.jsonl scenes/test_scene.scene.json

# export a distributable game package
./build/headless/tools/exporter/ignis_export \
    --scene scenes/test_scene.scene.json --include scripts --include assets \
    --out dist/
./build/headless/runtime/ignis_runtime --headless --pak dist/game.ignispak

# full build with editor + Vulkan renderer (needs X11/Wayland + Vulkan dev pkgs)
cmake --preset full && cmake --build --preset full
./build/full/editor/ignis_editor scenes/test_scene.scene.json
```

## Layout

| Path | What |
|---|---|
| `engine/` | `ignis_core` static lib: ECS, scene, assets, pak, agent commands |
| `scripting/` | Lua 5.4 bindings (`ignis.*` API for entities/scene/prefabs) |
| `physics/` | reactphysics3d: RigidBody/Collider, raycast, collisions |
| `audio/` | miniaudio playback + decoding, null-device headless mode, `AudioSource` component + `AudioSystem` (spatial 3D audio, pak-aware playback) |
| `window/` `renderer/` | GLFW window, NVRHI/Vulkan renderer (PBR, shadows, SSAO, IBL, HDR tonemap) |
| `runtime/` `editor/` | game runtime (headless-capable) and editor executables |
| `tools/exporter/` | `ignis_export` CLI: packs a game into `game.ignispak` |
| `tests/` | GTest unit tests + integration test over `scenes/test_scene.scene.json` |
| `scenes/` `scripts/` `assets/` | test scene, Lua scripts, test assets |

## Scene format

Scenes are JSON (see `scenes/test_scene.scene.json`): entities with `id`,
`name`, `parent`, `components` (keyed by registered component name), plus
`prefabs`. Paths inside are relative to CWD or to the pak root.

## Scripting (Lua)

```lua
function on_create(self) ... end        -- self.entity is the EntityId
function on_update(self, dt) ... end
function on_destroy(self) ... end
-- API: ignis.entity.*, ignis.scene.*, ignis.physics.raycast, ignis.audio.play,
--      ignis.audio.play_source/stop_source/source_playing/set_listener, ...
```

## Agent control

Every editor action is a JSON command (`ignis/agent/commands.h`), e.g.
`{"cmd": "create_entity", "name": "Player"}` — batch files via `--commands x.jsonl`.
