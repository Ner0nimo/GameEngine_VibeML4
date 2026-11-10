#include <gtest/gtest.h>

#include "ignis/assets/asset_manager.h"
#include "ignis/ecs/components.h"
#include "ignis/scene/scene.h"
#include "ignis/scripting/script_engine.h"

#include <filesystem>
#include <fstream>

using namespace ignis;

namespace {

std::string write_script(const std::string& name, const std::string& code) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream f(path);
    f << code;
    return path.string();
}

const char* kMoverScript = R"LUA(
created_id = 0
function on_create(self)
    ignis.entity.add_component(self.entity, "Transform", {position = {1, 2, 3}})
end
function on_update(self, dt)
    local t = ignis.entity.get_component(self.entity, "Transform")
    if t then
        t.position = {t.position[1] + dt, t.position[2], t.position[3]}
        ignis.entity.add_component(self.entity, "Transform", t)
    end
    if ignis.frame() == 5 then
        created_id = ignis.entity.create("FromLua")
        ignis.entity.add_component(created_id, "Transform", {position = {9, 9, 9}})
    end
    if ignis.frame() == 8 then
        ignis.entity.destroy(created_id)
        created_id = 0
    end
end
function on_destroy(self)
    ignis.log(2, "destroyed " .. self.entity)
end
)LUA";

} // namespace

TEST(Scripting, EntityLifecycleAndComponents) {
    Scene scene("script_test");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());

    const std::string script_path = write_script("ignis_mover_test.lua", kMoverScript);
    EntityId e = scene.world().create_entity("scripted");
    scene.world().emplace<ScriptComponent>(e).path = script_path;

    const f32 dt = 1.f / 60.f;
    for (int i = 0; i < 10; ++i) scripts.update(dt);

    // Transform was created by on_create and moved by on_update.
    const auto* t = scene.world().get<TransformComponent>(e);
    ASSERT_NE(t, nullptr);
    EXPECT_NEAR(t->position.x, 1.f + 10.f * dt, 1e-4f);
    EXPECT_NEAR(t->position.y, 2.f, 1e-6f);

    // Entity created from Lua at frame 5 and destroyed at frame 8.
    u32 named = 0;
    for (EntityId id : scene.world().all_entities()) {
        if (scene.world().name_of(id) == "FromLua") ++named;
    }
    EXPECT_EQ(named, 0u);

    // on_destroy fired when we destroy the scripted entity.
    scripts.on_entity_destroyed(e);
    scene.world().destroy_entity(e);
    EXPECT_FALSE(scene.world().alive(e));

    std::filesystem::remove(script_path);
}

TEST(Scripting, PrefabSpawnFromLua) {
    Scene scene("prefab_test");
    register_builtin_components(scene.world());
    scene.add_prefab("P", {{"name", "P"}, {"components", {{"Transform", {{"position", {4, 4, 4}}}}}}});
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());

    const std::string path = write_script("ignis_prefab_test.lua", R"LUA(
function on_update(self, dt)
    if ignis.frame() == 2 then
        local e = ignis.scene.spawn_prefab("P")
        spawned = e
    end
end
)LUA");
    EntityId e = scene.world().create_entity("s");
    scene.world().emplace<ScriptComponent>(e).path = path;
    for (int i = 0; i < 3; ++i) scripts.update(0.016f);

    u32 count = 0;
    for (EntityId id : scene.world().all_entities()) {
        if (scene.world().name_of(id) == "P") {
            ++count;
            EXPECT_EQ(scene.world().get<TransformComponent>(id)->position.x, 4.f);
        }
    }
    EXPECT_EQ(count, 1u);
    std::filesystem::remove(path);
}

TEST(Scripting, BadScriptReportsError) {
    Scene scene("bad");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());
    const std::string path = write_script("ignis_bad_test.lua", "this is not lua {{{");
    EntityId e = scene.world().create_entity("s");
    scene.world().emplace<ScriptComponent>(e).path = path;
    scripts.update(0.016f); // must not crash; error is logged
    EXPECT_TRUE(scene.world().alive(e));
    std::filesystem::remove(path);
}
