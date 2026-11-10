# AI Game Engine

We're building a production-grade, simple and straight-forward 3D Game Engine for Windows, macOS and Linux (Ubuntu 24+). This is professional-grade software that needs to be stable, fully-tested, and real-world deployable. No hacks, no shortcuts - solid as a rock.

## Decisions / status (maintained as we go)

- Engine name: **Ignis** (namespace `ignis`).
- Repo: https://github.com/Ner0nimo/GameEngine_VibeML4 (private).
- Implemented: C++20/CMake core static lib (`ignis_core`: ECS, scene JSON +
  prefabs, glTF assets, pak export format, JSON agent-command API), Lua 5.4
  scripting with full ECS bindings, reactphysics3d physics (rigid bodies,
  colliders, raycast, collision events), miniaudio audio (null-device headless
  mode, AudioSource component + AudioSystem with spatial 3D audio and
  pak-aware playback), GLFW window + NVRHI/Vulkan renderer (compile-verified; PBR/IBL/
  shadows/SSAO/HDR-tonemap passes), headless runtime (`--headless`, `--pak`),
  `ignis_export` packaging tool, editor (window + renderer + ImGui UI + agent commands).
- Tests: 43 GTest cases incl. an integration test that runs
  `scenes/test_scene.scene.json` headless and exercises scripting, physics,
  audio, prefabs, entity destruction and agent commands. CI: `.github/workflows/ci.yml`.
- Dev guide: `AGENTS.md`; skill: `.vibe/skills/ignis-dev`.

## Dev Workflow
- Ask questions only when absolutely necessary - work autonomously
- IMRORTANT! DO NOT look outside of working directory or use other code on my computer as reference
- Start by asking question to determine engine name. Suggest a few
- Testing is of the utmost importance - use lots of unit tests (any framework, simple is good), run automated testing
- Setup a scene used for testing, which tests every single feature in the engine - all components, and entire scripting API
- Do things properly, this is production-grade and not a hack project
- Create AGENTS.md and relevant skills to support development, and contain concrete development guidelines
- Create git repository, commit and push to [GitHub repo](https://github.com/Ner0nimo/GameEngine_VibeML4). IMPORTANT: do a code review before committing, and make sure all changes comply with code style, production-grade quality standards, and have been properly tested/have unit tests that have passed where necessary

## Tech stack
- C++ and CMake for core engine
- [GLFW](https://github.com/glfw/glfw) and [nvrhi](https://github.com/NVIDIA-RTX/NVRHI), using Vulkan primarily on all platforms
- [glm](https://github.com/g-truc/glm) for math
- [miniaudio](https://github.com/mackron/miniaudio) for audio
- Lua for scripting, though this can be up for discussion

## Basic Architecture
- Static library for core engine, executable for editor and runtime (depending on chosen design)
- Need to have editor to build games - this can be embedded into runtime executable (and stripped from distribution builds), or can be a standalone executable
- Simple ECS perhaps, to author scene with entities and components
- Ability to "export" game - executable that runs game without editing ability that we can distribute
- Editor needs to be fully controllable by AI agents - I should be able to ask you to build me a game like Tetris, and you should have all the tools available to do so without my intervention

## 3D Renderer
- Import gltf meshes with materials and textures
- Editor has gizmo to position them in the world
- PBR material workflow
- Directional + point + spot lights (PBR Cook-Torrance per light; shadows on
  the directional light only)
- IBL with HDRIs from https://polyhaven.com/hdris (diffuse irradiance +
  mipmapped prefiltered radiance cubemap + BRDF LUT for roughness-correct
  specular reflections, built on CPU at load time)
- Soft shadow maps: 4 cascaded shadow maps (CSM atlas, practical splits,
  texel snapping, cross-cascade blending) with PCSS per cascade (blocker
  search + penumbra-scaled PCF); 1-cascade fallback via
  `set_shadow_cascade_count(1)`
- Good SSAO
- HDR pipeline with bloom (multi-level mip chain, composited pre-tonemap) and
  ACES tonemapping; `VulkanRenderer::set_bloom()` tunes it
- Screen-space reflections (view-space ray march + binary refine, confidence
  blend over IBL) via `VulkanRenderer::set_ssr()`; Hi-Z coarse pass can later
  replace the linear march in `ssr.frag` without changing the blend logic

## 3D Physics
- Use simple 3D physics library of your choosing
- Controllable via scripting, authored via components in editor

## Behavior
- Scripting with Lua (or other chosen language)
- Control entities/components and run in update loop, and entity destruction/creation
- Spawn new entities/prefabs

## Security trust model
Agent commands (`ignis/agent/commands.h`, `--commands file.jsonl`) and Lua
scripts are treated as **untrusted input** even when supplied locally; the
engine enforces capability boundaries rather than trusting the caller:

- **Actor classes**: local trusted editor owner (full access), AI-generated
  commands (validated, path-scoped, size/batch-limited), commands from
  imported projects or external integrations (same validation; in exported
  runtime builds host-filesystem paths are denied entirely).
- **Filesystem access modes** (`AssetAccessMode`): `EditorFilesystem` (trusted
  editor process), `ProjectFilesystem` (paths must resolve inside the project
  root, symlink-safe), `PackageOnly` (exported runtime: pak entries only, a
  miss is an error, never a host-disk fallback). `ignis.scene.save` from Lua
  follows the same policy.
- **Resource budgets**: Lua instruction budget + memory cap per engine,
  bounded Lua→JSON conversion, scene parse limits (depth/size/counts),
  package index limits, command size/batch/recursion limits.
