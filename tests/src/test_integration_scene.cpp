// Integration test: loads scenes/test_scene.scene.json and runs it headless
// for 120 frames with scripting + physics + audio + agent commands active.
// This is the "test scene that exercises every feature" from GameEngineDoc.md.

#include <gtest/gtest.h>

#include "ignis/agent/commands.h"
#include "ignis/assets/asset_manager.h"
#include "ignis/audio/audio_engine.h"
#include "ignis/audio/audio_system.h"
#include "ignis/ecs/components.h"
#include "ignis/physics/physics_world.h"
#include "ignis/scene/scene.h"
#include "ignis/scripting/script_engine.h"

#include "lua_all.h"

using namespace ignis;

TEST(Integration, TestSceneAllFeatures) {
    Scene scene;
    register_builtin_components(scene.world());
    PhysicsSystem::register_components(scene.world());
    AudioSystem::register_components(scene.world());
    ASSERT_TRUE(scene.load_file(std::string(IGNIS_SCENES_DIR) + "/test_scene.scene.json").ok());

    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());

    AudioEngine audio;
    ASSERT_TRUE(audio.init(true).ok()); // null device, headless
    audio.set_asset_manager(&assets);
    AudioEngine::register_lua_bindings(scripts.state());
    AudioSystem audio_sys(audio, &scene);
    AudioSystem::register_lua_bindings(scripts.state());

    PhysicsSystem physics(scene);
    ASSERT_TRUE(physics.init({0.f, -9.81f, 0.f}).ok());
    PhysicsSystem::register_lua_bindings(scripts.state());

    scene.add_system([&](World&, f32 dt) { scripts.update(dt); });
    scene.add_system([&](World& w, f32 dt) { physics.update(w, dt); });
    scene.add_system([&](World& w, f32 dt) { audio_sys.update(w, dt); });

    // Agent commands: create an entity via JSON command (AI-controllable path).
    CommandProcessor proc(scene, assets);
    auto results = proc.execute_file(std::string(IGNIS_SCRIPTS_DIR) + "/commands.jsonl");
    ASSERT_FALSE(results.empty());
    ASSERT_TRUE(results[0].ok) << results[0].message;
    const EntityId agent_made = results[0].data["entity"].get<EntityId>();
    auto r = proc.execute({{"cmd", "add_component"}, {"entity", agent_made},
                           {"component", "Transform"}, {"props", {{"position", {7, 7, 7}}}}});
    ASSERT_TRUE(r.ok) << r.message;

    // Run 300 frames at 60 Hz (5 s) so the falling box fully settles.
    const f32 dt = 1.f / 60.f;
    for (int i = 0; i < 300; ++i) scene.update(dt);

    // --- Feature assertions -------------------------------------------------

    // 1. Scripting moved the Mover entity along +X (2 units/s * 2 s = ~4).
    EntityId mover = NULL_ENTITY, box = NULL_ENTITY, target = NULL_ENTITY;
    u32 prefab_count = 0;
    for (EntityId e : scene.world().all_entities()) {
        const std::string n = scene.world().name_of(e);
        if (n == "Mover") mover = e;
        if (n == "FallingBox") box = e;
        if (n == "Target") target = e;
        if (n == "SpawnedPrefab") ++prefab_count;
    }
    ASSERT_NE(mover, NULL_ENTITY);
    ASSERT_NE(box, NULL_ENTITY);
    const auto* mt = scene.world().get<TransformComponent>(mover);
    ASSERT_NE(mt, nullptr);
    EXPECT_GT(mt->position.x, 3.f); // moved from 0 by ~4

    // 2. Script spawned a prefab at frame 30.
    EXPECT_EQ(prefab_count, 1u);

    // 3. Script destroyed the Target entity at frame 60.
    EXPECT_EQ(target, NULL_ENTITY);

    // 4. Physics: FallingBox fell from y=5 and rests on the ground (~y=1.5).
    const auto* bt = scene.world().get<TransformComponent>(box);
    ASSERT_NE(bt, nullptr);
    EXPECT_NEAR(bt->position.y, 1.5f, 0.1f);
    EXPECT_FALSE(physics.take_collision_events().empty());

    // 5. Agent command created entity with Transform.
    ASSERT_TRUE(scene.world().alive(agent_made));
    EXPECT_EQ(scene.world().get<TransformComponent>(agent_made)->position.x, 7.f);

    // 6. Raycast from C++ side hits the ground.
    const RaycastHit hit = physics.raycast({0.f, 10.f, 0.f}, {0.f, -1.f, 0.f}, 100.f);
    EXPECT_TRUE(hit.hit);

    // 7. AudioSource: SoundEmitter auto-started (play_on_start), was stopped
    //    and restarted from Lua (mover.lua frames 120/150).
    EntityId emitter = NULL_ENTITY;
    for (EntityId e : scene.world().all_entities()) {
        if (scene.world().name_of(e) == "SoundEmitter") emitter = e;
    }
    ASSERT_NE(emitter, NULL_ENTITY);
    const auto* src = scene.world().get<AudioSourceComponent>(emitter);
    ASSERT_NE(src, nullptr);
    EXPECT_EQ(src->path, "assets/test.wav");
    EXPECT_TRUE(src->play_on_start);
    EXPECT_TRUE(src->spatial);
    EXPECT_TRUE(audio_sys.source_playing(emitter));
    EXPECT_GE(audio.active_sound_count(), 1u);
    // mover.lua's globals live in the mover's private script env (sandboxed).
    auto lua_bool = [&](const char* name) { return scripts.script_env_bool(mover, name); };
    EXPECT_TRUE(lua_bool("mover_src_playing"));
    EXPECT_TRUE(lua_bool("mover_src_stopped"));
    EXPECT_TRUE(lua_bool("mover_src_restarted"));

    // 8. Scene serializes cleanly after all of this, AudioSource included.
    const nlohmann::json j = scene.serialize();
    EXPECT_EQ(j["name"], "test_scene");
    EXPECT_FALSE(j["entities"].empty());
    Scene reloaded = Scene::deserialize(j);
    EXPECT_EQ(reloaded.world().entity_count(), scene.world().entity_count());
    EntityId reloaded_emitter = NULL_ENTITY;
    for (EntityId e : reloaded.world().all_entities()) {
        if (reloaded.world().name_of(e) == "SoundEmitter") reloaded_emitter = e;
    }
    ASSERT_NE(reloaded_emitter, NULL_ENTITY);
    const auto* rsrc = reloaded.world().get<AudioSourceComponent>(reloaded_emitter);
    ASSERT_NE(rsrc, nullptr);
    EXPECT_EQ(rsrc->path, "assets/test.wav");
    EXPECT_TRUE(rsrc->loop);
    EXPECT_TRUE(rsrc->spatial);
    EXPECT_NEAR(rsrc->volume, 0.8f, 1e-6f);
}
