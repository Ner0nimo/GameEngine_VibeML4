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

## CI & security gates (all must be green before a PR merges)

- `CI` (`.github/workflows/ci.yml`): headless build + `ctest` + runtime/export
  smoke tests + a `-Werror` syntax check of the graphics-only TUs, and a full
  graphics build + `ctest` (lavapipe software Vulkan).
- `Security CI` (`.github/workflows/security.yml`): ASan+UBSan test run
  (`headless-asan` preset); libFuzzer smoke over every untrusted-input parser
  (`fuzz_pak`, `fuzz_scene`, `fuzz_json`, `fuzz_gltf`, `fuzz_texture`,
  `fuzz_audio`, `fuzz_commands` — corpus in `tests/fuzz/corpus/<name>/`);
  clang-tidy security checks; gitleaks secret scan over full history;
  actionlint workflow lint; a FetchContent pin audit (every `GIT_TAG` must be a
  40-hex commit SHA over `https://`); and an audit that every workflow
  `uses:` is pinned to a full commit SHA.
- `CodeQL` (`.github/workflows/codeql.yml`): cpp `security-and-quality` on PRs
  and weekly; SARIF lands in the Security tab.

Rules for PRs from agents:

- Add unit tests for any new parser/untrusted-input path, and a fuzz target in
  `tests/fuzz/` for any new file format.
- Never add a GitHub Action or a dependency without a pinned commit SHA; never
  commit secrets. Dependabot bumps the pinned action SHAs weekly.
- `.github/CODEOWNERS` requires owner review only for CI/dependency
  infrastructure and the security-critical parsers (core/assets/agent/audio
  sources, fuzz harnesses); general engine code is open to any reviewer.
- Nightly fuzz runs upload the grown corpora as the `fuzz-corpus-update`
  artifact; merge those back into `tests/fuzz/corpus/` manually (weekly) so
  coverage keeps growing. Crash artifacts (`fuzz-crash-artifacts`) are
  regressions — fix and add them as corpus seeds.

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
editor/     ignis_editor exe (graphics builds only): window + Vulkan renderer +
            ImGui editor UI (menu/toolbar, hierarchy, inspector, console,
            asset browser, status bar, ImGuizmo viewport transform gizmo)
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

Every function in Ignis follows these three rules. When writing, reviewing, or
refactoring code, check each function against all three.

### 1. Fulfill your contract completely; never make callers compensate — and be defensive about what you consume

A function must do exactly what its name and contract promise: return a
complete, meaningful result, or explicitly report why it could not. Callers
must not have to compensate for incomplete operations, interpret undocumented
sentinel values, repeat validation, or repair inconsistent state.

At the same time, do not blindly trust inputs or dependencies. Validate
untrusted input at system boundaries, enforce invariants at mutation
boundaries, and trust established internal invariants — but still detect
violations instead of silently accommodating them. When something can fail,
report the failure explicitly; the recovery decision (propagate, retry,
degrade, substitute a documented fallback, abort) belongs to the highest layer
that has enough semantic context for it — not necessarily `main()`. An
explicit, documented fallback is fine; a concealed one is not. Never hide a
programming error behind a fallback.

- `World::set_parent` validates entities and rejects cycles itself, so callers
  don't duplicate those checks and `world_matrix` can trust the acyclic
  invariant (no magic `guard < 1024` loop).
- `Scene::load_file` really loads INTO the scene and preserves registered
  systems instead of silently doing `*this = deserialize(...)`.
- Low-level functions return `Result<T>` and never swallow errors; the caller
  decides what to do with `r.error()`. The fork-based Vulkan probe lives in
  `EditorApp::run`/`main` because only the top level knows that a driver
  crash is unrecoverable there.
- Negative example: `Texture load_texture(path) { if (!exists) return
  default_texture; }` — the caller cannot distinguish the requested texture
  from the fallback. Return `Result<Texture>` / `std::optional<Texture>` and
  let the asset-policy layer decide on placeholders.

### 2. Signatures should communicate the contract and minimize caller burden

The signature — name, parameter types, return type, plus docs and
preconditions — should communicate inputs, outputs, ownership, and failure
modes. Callers should not have to guess what special values mean, construct
unnecessary temporaries, or read the implementation to use a function
correctly. Pick the type that matches the semantics of the operation; do not
add wrapper types when a simpler type already expresses the contract:

- `Result<T>` (see `core/result.h`) when the caller needs to understand
  failure: `Result<void> AudioEngine::init(bool null_device)` — one object
  carries success and message: `if (auto r = audio.init(true); !r.ok())
  log(r.error());` — instead of `bool init(std::string& err)`.
- `std::optional<T>` when absence is an ordinary outcome:
  `std::optional<f32> ray_aabb(...)` instead of a `-1` sentinel.
- Return values instead of out-params: `Result<std::vector<u8>>
  PakReader::read(path)` instead of `bool read(path, std::vector<u8>& out)`;
  `picking::Ray screen_ray(cam, aspect, x, y)` instead of two out-params;
  `FramebufferSize framebuffer_size()` instead of `void f(u32&, u32&)`.
- `bool` when the outcome is genuinely yes/no; `void` when the operation
  cannot meaningfully fail. The goal is to make correct usage obvious and
  misuse difficult — not to maximize type complexity.

### 3. Keep each function at a consistent level of abstraction

A function should read like one coherent operation: its major steps expressed
at roughly the same level of conceptual detail, with lower-level details
delegated to meaningful, named functions. Do not mix high-level orchestration
with low-level implementation details in one body — but also do not extract
trivial statements into helpers just to make a function shorter.
Decomposition should improve understanding, not create indirection. The goal
is readability, not maximal decomposition.

- `ScriptEngine::update` iterates script entities and delegates the actual
  call to `run_callback(env_ref, "on_update", true, dt)`.
- `AudioSystem::update` = `stop_orphaned_sources(world)` + `update_source(e, world)` per entity.
- `EditorApp::run` delegates to `init_*` helpers and `main_loop` instead of
  one 400-line body mixing window, renderer, ImGui and event handling.

### Using the three rules together

The rules complement each other — do not satisfy one at the expense of
another: don't split functions into meaningless helpers for rule 3, don't
wrap every call in `Result` for rule 2, don't push recovery into an
inappropriate layer for rule 1. When in doubt, prefer explicit contracts,
meaningful abstractions, and the simplest design that preserves correctness.
Rule 1 protects correctness, rule 2 API quality, rule 3 maintainability.


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
- Do all work on a branch (`vibe/<slug>`), open a PR, and merge to `main` of
  Ner0nimo/GameEngine_VibeML4 only when all tests and CI pass. Do not push
  directly to `main`.

## AI-agent control of the editor

`ignis/agent/commands.h` exposes every editor action as a JSON command
(create_entity, add_component, spawn_prefab, save_scene, run_commands, ...).
Editor/runtime accept `--commands file.jsonl` (one JSON per line). This is the
intended interface for "build me a game" requests.
