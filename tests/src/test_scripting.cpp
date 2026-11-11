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
    scene.world().emplace<ScriptComponent>(e)->path = script_path;

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

// Security regression (#46): component ops through the Lua bindings must
// reject dead entity ids safely (refusal/absence), never write into storage
// and never abort the script with an uncaught error.
TEST(Scripting, DeadEntityComponentOpsAreSafe) {
    Scene scene("dead_id_test");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());

    const std::string path = write_script("ignis_dead_id_test.lua", R"LUA(
function on_update(self, dt)
    if ignis.frame() == 2 then
        local v = ignis.entity.create("victim")
        ignis.entity.destroy(v)
        -- Every op on the dead id must be refused/absent, not crash.
        ignis.entity.add_component(v, "Transform", {position = {1, 2, 3}})
        ignis.entity.get_component(v, "Transform")
        ignis.entity.has_component(v, "Transform")
        ignis.entity.remove_component(v, "Transform")
        ignis.entity.create("after") -- recycles v: must stay component-free
    end
    if ignis.frame() == 3 then
        -- Marker: proves the script survived the dead-id operations above.
        ignis.entity.add_component(self.entity, "Transform", {position = {5, 5, 5}})
    end
end
)LUA");
    EntityId e = scene.world().create_entity("scripted");
    scene.world().emplace<ScriptComponent>(e)->path = path;
    for (int i = 0; i < 5; ++i) scripts.update(1.f / 60.f);

    // The script ran to completion past the dead-id operations.
    const auto* t = scene.world().get<TransformComponent>(e);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->position.x, 5.f);

    // The victim is gone, and the recycled id started component-clean.
    EntityId after = NULL_ENTITY;
    u32 victims = 0;
    for (EntityId id : scene.world().all_entities()) {
        if (scene.world().name_of(id) == "victim") ++victims;
        if (scene.world().name_of(id) == "after") after = id;
    }
    EXPECT_EQ(victims, 0u);
    ASSERT_NE(after, NULL_ENTITY);
    EXPECT_FALSE(scene.world().has<TransformComponent>(after));
    EXPECT_EQ(scene.world().get<TransformComponent>(after), nullptr);
    std::filesystem::remove(path);
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
    scene.world().emplace<ScriptComponent>(e)->path = path;
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
    scene.world().emplace<ScriptComponent>(e)->path = path;
    scripts.update(0.016f); // must not crash; error is logged
    EXPECT_TRUE(scene.world().alive(e));
    std::filesystem::remove(path);
}

namespace {

bool entity_named_exists(Scene& scene, const std::string& name) {
    for (EntityId id : scene.world().all_entities()) {
        if (scene.world().name_of(id) == name) return true;
    }
    return false;
}

} // namespace

TEST(LuaIsolation, HostLibrariesUnavailableToScripts) {
    Scene scene("iso");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());
    const std::string path = write_script("ignis_iso_host.lua", R"LUA(
function on_create(self)
    if io == nil and os == nil and package == nil and debug == nil
       and coroutine == nil and dofile == nil and loadfile == nil and load == nil
       and _G.io == nil and _G.os == nil then
        ignis.entity.create("sandbox_ok")
    else
        ignis.entity.create("sandbox_fail")
    end
end
)LUA");
    EntityId e = scene.world().create_entity("s");
    scene.world().emplace<ScriptComponent>(e)->path = path;
    scripts.update(0.016f);
    EXPECT_TRUE(entity_named_exists(scene, "sandbox_ok"));
    EXPECT_FALSE(entity_named_exists(scene, "sandbox_fail"));
    std::filesystem::remove(path);
}

TEST(LuaIsolation, EnvironmentsArePrivatePerScript) {
    Scene scene("iso2");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());
    const std::string path_a = write_script("ignis_iso_a.lua", R"LUA(
function on_create(self)
    secret_marker = 42 -- lands in this script's own env, not _G
end
)LUA");
    const std::string path_b = write_script("ignis_iso_b.lua", R"LUA(
function on_create(self)
    if secret_marker == nil and rawget(_G, "secret_marker") == nil then
        ignis.entity.create("isolated_ok")
    else
        ignis.entity.create("isolated_fail")
    end
end
)LUA");
    EntityId a = scene.world().create_entity("a");
    scene.world().emplace<ScriptComponent>(a)->path = path_a;
    EntityId b = scene.world().create_entity("b");
    scene.world().emplace<ScriptComponent>(b)->path = path_b;
    scripts.update(0.016f);
    EXPECT_TRUE(entity_named_exists(scene, "isolated_ok"));
    EXPECT_FALSE(entity_named_exists(scene, "isolated_fail"));
    std::filesystem::remove(path_a);
    std::filesystem::remove(path_b);
}

TEST(LuaIsolation, RunawayScriptAbortedByInstructionBudget) {
    Scene scene("budget");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());
    scripts.set_instruction_budget(20000); // tiny budget: must abort quickly
    const std::string bad_path = write_script("ignis_runaway.lua", R"LUA(
function on_update(self, dt)
    while true do end
end
)LUA");
    const std::string good_path = write_script("ignis_good.lua", R"LUA(
function on_update(self, dt)
    if ignis.frame() == 2 then
        ignis.entity.create("good_ok")
    end
end
)LUA");
    EntityId bad = scene.world().create_entity("bad");
    scene.world().emplace<ScriptComponent>(bad)->path = bad_path;
    EntityId good = scene.world().create_entity("good");
    scene.world().emplace<ScriptComponent>(good)->path = good_path;
    for (int i = 0; i < 3; ++i) scripts.update(0.016f); // must return, not hang
    EXPECT_TRUE(entity_named_exists(scene, "good_ok"));   // engine still functional
    EXPECT_TRUE(scene.world().alive(bad));                 // error swallowed, entity intact
    std::filesystem::remove(bad_path);
    std::filesystem::remove(good_path);
}

TEST(LuaIsolation, CyclicTableConversionIsAControlledError) {
    Scene scene("cyclic");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());
    const std::string bad_path = write_script("ignis_cyclic.lua", R"LUA(
function on_create(self)
    local t = {}
    t.self_ref = t
    ignis.entity.add_component(self.entity, "Transform", t) -- cyclic table
end
)LUA");
    const std::string good_path = write_script("ignis_cyclic_good.lua", R"LUA(
function on_update(self, dt)
    if ignis.frame() == 2 then
        ignis.entity.create("after_cyclic_ok")
    end
end
)LUA");
    EntityId bad = scene.world().create_entity("bad");
    scene.world().emplace<ScriptComponent>(bad)->path = bad_path;
    EntityId good = scene.world().create_entity("good");
    scene.world().emplace<ScriptComponent>(good)->path = good_path;
    for (int i = 0; i < 3; ++i) scripts.update(0.016f); // must not crash
    EXPECT_TRUE(entity_named_exists(scene, "after_cyclic_ok"));
    std::filesystem::remove(bad_path);
    std::filesystem::remove(good_path);
}

TEST(LuaIsolation, BadScriptDoesNotCorruptEngineState) {
    Scene scene("bad2");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());
    const std::string bad_path = write_script("ignis_bad2.lua", "this is not lua {{{");
    const std::string good_path = write_script("ignis_good2.lua", R"LUA(
function on_update(self, dt)
    if ignis.frame() == 1 then
        ignis.entity.create("still_works")
    end
end
)LUA");
    EntityId bad = scene.world().create_entity("bad");
    scene.world().emplace<ScriptComponent>(bad)->path = bad_path;
    EntityId good = scene.world().create_entity("good");
    scene.world().emplace<ScriptComponent>(good)->path = good_path;
    for (int i = 0; i < 2; ++i) scripts.update(0.016f);
    EXPECT_TRUE(entity_named_exists(scene, "still_works"));
    EXPECT_TRUE(scene.world().alive(bad));
    std::filesystem::remove(bad_path);
    std::filesystem::remove(good_path);
}

TEST(LuaIsolation, StringAndIntegerTableKeysAccepted) {
    Scene scene("keys_ok");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());
    const std::string path = write_script("ignis_keys_ok.lua", R"LUA(
function on_create(self)
    local t = { label = "x", [7] = 1, [42] = "y" } -- string + integer keys
    ignis.entity.add_component(self.entity, "Transform", t)
    ignis.entity.create("keys_ok_marker")
end
)LUA");
    EntityId e = scene.world().create_entity("s");
    scene.world().emplace<ScriptComponent>(e)->path = path;
    scripts.update(0.016f);
    EXPECT_TRUE(entity_named_exists(scene, "keys_ok_marker")); // conversion did not error
    std::filesystem::remove(path);
}

TEST(LuaIsolation, UnsupportedTableKeyTypesAreControlledErrors) {
    for (const char* bad_key_case : {"[true] = 1", "[{}] = 1", "[1.5] = 1", "[print] = 1"}) {
        Scene scene("keys_bad");
        register_builtin_components(scene.world());
        AssetManager assets;
        ScriptEngine scripts;
        ASSERT_TRUE(scripts.init(&scene, &assets).ok());
        const std::string bad_path = write_script("ignis_keys_bad.lua",
            std::string("function on_create(self)\n    local t = { ") + bad_key_case +
            " }\n    ignis.entity.add_component(self.entity, \"Transform\", t)\n    ignis.entity.create(\"should_not_happen\")\nend\n");
        const std::string good_path = write_script("ignis_keys_good.lua", R"LUA(
function on_update(self, dt)
    if ignis.frame() == 2 then
        ignis.entity.create("engine_still_ok")
    end
end
)LUA");
        EntityId bad = scene.world().create_entity("bad");
        scene.world().emplace<ScriptComponent>(bad)->path = bad_path;
        EntityId good = scene.world().create_entity("good");
        scene.world().emplace<ScriptComponent>(good)->path = good_path;
        for (int i = 0; i < 3; ++i) scripts.update(0.016f); // must not crash
        EXPECT_FALSE(entity_named_exists(scene, "should_not_happen")) << bad_key_case;
        EXPECT_TRUE(entity_named_exists(scene, "engine_still_ok")) << bad_key_case;
        std::filesystem::remove(bad_path);
        std::filesystem::remove(good_path);
    }
}

TEST(LuaIsolation, SharedGlobalsAreReadOnly) {
    // IGN-001: _G of each script is its OWN env (writes stay isolated), and
    // the sandbox removes the mutation bypasses: rawset, setmetatable,
    // getmetatable, load/dofile/loadfile.
    Scene scene("ro");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());
    const std::string path_a = write_script("ignis_ro_a.lua", R"LUA(
function on_create(self)
    _G.pwned = 1 -- lands in THIS script's env only (not shared)
    if rawset == nil and setmetatable == nil and getmetatable == nil
       and load == nil and dofile == nil and loadfile == nil then
        ignis.entity.create("ro_ok")
    else
        ignis.entity.create("ro_fail")
    end
end
)LUA");
    const std::string path_b = write_script("ignis_ro_b.lua", R"LUA(
function on_create(self)
    -- _G is B's own env: A's writes must be invisible here.
    if rawget(_G, "pwned") == nil and pwned == nil then
        ignis.entity.create("iso_ok")
    else
        ignis.entity.create("iso_fail")
    end
end
)LUA");
    EntityId a = scene.world().create_entity("a");
    scene.world().emplace<ScriptComponent>(a)->path = path_a;
    EntityId b = scene.world().create_entity("b");
    scene.world().emplace<ScriptComponent>(b)->path = path_b;
    scripts.update(0.016f);
    EXPECT_TRUE(entity_named_exists(scene, "ro_ok"));
    EXPECT_FALSE(entity_named_exists(scene, "ro_fail"));
    EXPECT_TRUE(entity_named_exists(scene, "iso_ok"));
    EXPECT_FALSE(entity_named_exists(scene, "iso_fail"));
    std::filesystem::remove(path_a);
    std::filesystem::remove(path_b);
}

TEST(LuaIsolation, SharedLibTablesAreReadOnly) {
    // Shared stdlib tables (string, table, math, utf8, ignis) must not be
    // mutable from a script - otherwise one script breaks the others.
    Scene scene("libro");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());
    const std::string path_a = write_script("ignis_libro_a.lua", R"LUA(
function on_create(self)
    local blocked = not pcall(function() string.rep = nil end)
    if blocked and string.rep ~= nil and not pcall(function() ignis.entity = nil end) then
        ignis.entity.create("libro_ok")
    else
        ignis.entity.create("libro_fail")
    end
end
)LUA");
    const std::string path_b = write_script("ignis_libro_b.lua", R"LUA(
function on_create(self)
    if string.rep("x", 3) == "xxx" and ignis.entity ~= nil then
        ignis.entity.create("libro_b_ok")
    else
        ignis.entity.create("libro_b_fail")
    end
end
)LUA");
    EntityId a = scene.world().create_entity("a");
    scene.world().emplace<ScriptComponent>(a)->path = path_a;
    EntityId b = scene.world().create_entity("b");
    scene.world().emplace<ScriptComponent>(b)->path = path_b;
    scripts.update(0.016f);
    EXPECT_TRUE(entity_named_exists(scene, "libro_ok"));
    EXPECT_FALSE(entity_named_exists(scene, "libro_fail"));
    EXPECT_TRUE(entity_named_exists(scene, "libro_b_ok"));
    EXPECT_FALSE(entity_named_exists(scene, "libro_b_fail"));
    std::filesystem::remove(path_a);
    std::filesystem::remove(path_b);
}

TEST(LuaIsolation, MemoryBudgetAbortsAllocationBomb) {
    // Instruction budgets don't cover memory: string.rep is one instruction.
    // The custom allocator must fail the 64 MB allocation with a controlled
    // Lua error instead of an OOM kill.
    Scene scene("mem");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());
    scripts.set_memory_budget(1ull << 20); // 1 MB cap
    const std::string path = write_script("ignis_mem_bomb.lua", R"LUA(
function on_create(self)
    local s = string.rep("A", 64 * 1024 * 1024) -- 64 MB in a single instruction
    ignis.entity.create("bomb_succeeded") -- must NOT happen
end
)LUA");
    EntityId e = scene.world().create_entity("bomber");
    scene.world().emplace<ScriptComponent>(e)->path = path;
    scripts.update(0.016f); // controlled error, no crash, no OOM
    EXPECT_FALSE(entity_named_exists(scene, "bomb_succeeded"));
    EXPECT_LE(scripts.memory_used(), scripts.memory_budget());
    std::filesystem::remove(path);
}

TEST(LuaIsolation, SceneSaveRespectsPathPolicy) {
    // ignis.scene.save must obey the same path policy as agent commands:
    // host paths disallowed (exported runtime) -> denied; PackageOnly asset
    // mode -> denied; project root -> relative saves inside the root allowed,
    // traversal denied.
    Scene scene("savepol");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "ignis_savepol";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    const std::string target = (tmp / "pwned.json").string();

    std::string code = R"LUA(
function on_update(self, dt)
    if ignis.frame() == 2 then
        save_abs = ignis.scene.save("__TARGET__")
    end
    if ignis.frame() == 4 then
        save_rel = ignis.scene.save("out.json")
        save_esc = ignis.scene.save("../escape.json")
    end
    if ignis.frame() == 6 then
        save_pak = ignis.scene.save("pak.json")
    end
end
)LUA";
    const std::string marker = "__TARGET__";
    const size_t pos = code.find(marker);
    code.replace(pos, marker.size(), target);
    const std::string path = write_script("ignis_savepol.lua", code);

    EntityId e = scene.world().create_entity("s");
    scene.world().emplace<ScriptComponent>(e)->path = path;

    // Phase 1 (frames 1-2): exported-runtime policy - host paths disallowed.
    scripts.set_host_paths_allowed(false);
    scripts.update(0.016f);
    scripts.update(0.016f);
    EXPECT_FALSE(fs::exists(target)) << "absolute save must be denied";
    EXPECT_FALSE(scripts.script_env_bool(e, "save_abs"));

    // Phase 2 (frames 3-4): project root = tmp, host paths allowed.
    scripts.set_host_paths_allowed(true);
    scripts.set_project_root(tmp.string());
    scripts.update(0.016f);
    scripts.update(0.016f);
    EXPECT_TRUE(fs::exists(tmp / "out.json")) << "relative save inside root must work";
    EXPECT_FALSE(fs::exists(tmp / ".." / "escape.json")) << "traversal must be denied";
    EXPECT_TRUE(scripts.script_env_bool(e, "save_rel"));
    EXPECT_FALSE(scripts.script_env_bool(e, "save_esc"));

    // Phase 3 (frames 5-6): PackageOnly asset mode denies even with the flag on.
    assets.set_access_mode(AssetAccessMode::PackageOnly);
    scripts.update(0.016f);
    scripts.update(0.016f);
    EXPECT_FALSE(fs::exists(tmp / "pak.json")) << "PackageOnly must deny host writes";
    EXPECT_FALSE(scripts.script_env_bool(e, "save_pak"));

    std::filesystem::remove(path);
    fs::remove_all(tmp);
}

TEST(LuaIsolation, DeadEntityIdsRejectedByBindings) {
    Scene scene("dead");
    register_builtin_components(scene.world());
    AssetManager assets;
    ScriptEngine scripts;
    ASSERT_TRUE(scripts.init(&scene, &assets).ok());
    const std::string bad_path = write_script("ignis_dead.lua", R"LUA(
function on_create(self)
    local ok_add = ignis.entity.add_component(999, "Transform", {position = {1, 2, 3}})
    if ok_add == false then
        ignis.entity.set_name(999, "ghost")          -- must be a safe no-op
        ignis.entity.remove_component(999, "Transform") -- must be a safe no-op
        ignis.entity.create("dead_rejected")
    else
        ignis.entity.create("dead_accepted")
    end
end
)LUA");
    EntityId e = scene.world().create_entity("s");
    scene.world().emplace<ScriptComponent>(e)->path = bad_path;
    scripts.update(0.016f);
    EXPECT_TRUE(entity_named_exists(scene, "dead_rejected"));
    EXPECT_FALSE(entity_named_exists(scene, "dead_accepted"));
    std::filesystem::remove(bad_path);
}
