#include <gtest/gtest.h>

#include "ignis/agent/commands.h"
#include "ignis/assets/asset_manager.h"
#include "ignis/ecs/components.h"
#include "ignis/scene/scene.h"

#include <filesystem>
#include <fstream>

using namespace ignis;

TEST(AgentCommands, BuildSceneViaJson) {
    Scene scene("agent");
    register_builtin_components(scene.world());
    AssetManager assets;
    CommandProcessor proc(scene, assets);

    auto r = proc.execute({{"cmd", "create_entity"}, {"name", "Player"}});
    ASSERT_TRUE(r.ok);
    const EntityId e = r.data["entity"].get<EntityId>();

    r = proc.execute({{"cmd", "add_component"}, {"entity", e}, {"component", "Transform"},
                      {"props", {{"position", {1, 2, 3}}}}});
    ASSERT_TRUE(r.ok) << r.message;
    r = proc.execute({{"cmd", "add_component"}, {"entity", e}, {"component", "Camera"},
                      {"props", {{"fov_y_deg", 45}}}});
    ASSERT_TRUE(r.ok) << r.message;

    r = proc.execute({{"cmd", "get_entity"}, {"entity", e}});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.data["name"], "Player");
    bool has_cam = false;
    for (const auto& c : r.data["components"]) {
        if (c["type"] == "Camera") {
            has_cam = true;
            EXPECT_EQ(c["data"]["fov_y_deg"].get<f32>(), 45.f);
        }
    }
    EXPECT_TRUE(has_cam);

    r = proc.execute({{"cmd", "list_entities"}});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.data["entities"].size(), 1u);

    // set_component overwrites, remove_component removes.
    r = proc.execute({{"cmd", "set_component"}, {"entity", e}, {"component", "Transform"},
                      {"props", {{"position", {9, 9, 9}}}}});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(scene.world().get<TransformComponent>(e)->position.x, 9.f);
    r = proc.execute({{"cmd", "remove_component"}, {"entity", e}, {"component", "Camera"}});
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(scene.world().has<CameraComponent>(e));

    // Prefabs.
    r = proc.execute({{"cmd", "add_prefab"}, {"prefab", "Enemy"},
                      {"template", {{"name", "Enemy"}, {"components", nlohmann::json::object()}}}});
    ASSERT_TRUE(r.ok);
    r = proc.execute({{"cmd", "spawn_prefab"}, {"prefab", "Enemy"}});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(scene.world().entity_count(), 2u);

    // Unknown command and unknown component fail gracefully.
    EXPECT_FALSE(proc.execute({{"cmd", "fly_to_moon"}}).ok);
    EXPECT_FALSE(proc.execute({{"cmd", "add_component"}, {"entity", e}, {"component", "Nope"},
                               {"props", nlohmann::json::object()}}).ok);

    // destroy
    r = proc.execute({{"cmd", "destroy_entity"}, {"entity", e}});
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(scene.world().alive(e));
}

TEST(AgentCommands, JsonlBatch) {
    Scene scene("batch");
    register_builtin_components(scene.world());
    AssetManager assets;
    CommandProcessor proc(scene, assets);

    const auto path = std::filesystem::temp_directory_path() / "ignis_cmds.jsonl";
    {
        std::ofstream f(path);
        f << R"({"cmd": "create_entity", "name": "a"})" "\n";
        f << R"({"cmd": "create_entity", "name": "b"})" "\n";
        f << "not json at all\n";
        f << R"({"cmd": "create_entity", "name": "c"})" "\n";
    }
    auto results = proc.execute_file(path.string());
    ASSERT_EQ(results.size(), 4u);
    EXPECT_TRUE(results[0].ok);
    EXPECT_TRUE(results[1].ok);
    EXPECT_FALSE(results[2].ok);
    EXPECT_TRUE(results[3].ok);
    EXPECT_EQ(scene.world().entity_count(), 3u);
    std::filesystem::remove(path);
}

namespace {

namespace fs = std::filesystem;

struct AgentSandbox {
    fs::path tmp;
    fs::path proj;
    AgentSandbox() {
        tmp = fs::temp_directory_path() / "ignis_agent_sec";
        fs::remove_all(tmp);
        proj = tmp / "proj";
        fs::create_directories(proj);
        std::ofstream(tmp / "evil.json") << "should not be written";
    }
    ~AgentSandbox() { std::error_code ec; fs::remove_all(tmp, ec); }
};

} // namespace

TEST(AgentSecurity, UnknownAndMalformedCommandsFail) {
    Scene scene("s");
    register_builtin_components(scene.world());
    AssetManager assets;
    CommandProcessor proc(scene, assets);
    EXPECT_FALSE(proc.execute({{"cmd", "definitely_not_a_command"}}).ok);
    EXPECT_FALSE(proc.execute({{"no_cmd", true}}).ok);
    EXPECT_FALSE(proc.execute(nlohmann::json::array()).ok);
    EXPECT_FALSE(proc.execute({{"cmd", 42}}).ok); // cmd must be a string
}

TEST(AgentSecurity, DeadEntityCommandsFail) {
    Scene scene("s");
    register_builtin_components(scene.world());
    AssetManager assets;
    CommandProcessor proc(scene, assets);
    EXPECT_FALSE(proc.execute({{"cmd", "destroy_entity"}, {"entity", 999}}).ok);
    EXPECT_FALSE(proc.execute({{"cmd", "add_component"}, {"entity", 999},
                               {"component", "Transform"}, {"props", nlohmann::json::object()}}).ok);
    EXPECT_FALSE(proc.execute({{"cmd", "get_entity"}, {"entity", 999}}).ok);
}

TEST(AgentSecurity, PathCommandsScopedToProjectRoot) {
    AgentSandbox sb;
    Scene scene("s");
    register_builtin_components(scene.world());
    AssetManager assets;
    CommandProcessor proc(scene, assets);
    proc.set_project_root(sb.proj.string());

    auto r = proc.execute({{"cmd", "save_scene"}, {"path", "ok.json"}});
    EXPECT_TRUE(r.ok) << r.message;
    EXPECT_TRUE(fs::exists(sb.proj / "ok.json"));

    r = proc.execute({{"cmd", "save_scene"}, {"path", "../evil.json"}});
    EXPECT_FALSE(r.ok);
    // The pre-existing file outside the root must be untouched.
    std::ifstream f(sb.tmp / "evil.json");
    const std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "should not be written");

    r = proc.execute({{"cmd", "save_scene"}, {"path", sb.tmp.string() + "/abs.json"}});
    EXPECT_FALSE(r.ok); // absolute path rejected

    r = proc.execute({{"cmd", "load_model"}, {"path", "../evil.json"}});
    EXPECT_FALSE(r.ok);
    r = proc.execute({{"cmd", "run_commands"}, {"file", "../evil.json"}});
    EXPECT_FALSE(r.ok);
}

TEST(AgentSecurity, HostPathsCanBeDeniedEntirely) {
    Scene scene("s");
    register_builtin_components(scene.world());
    AssetManager assets;
    CommandProcessor proc(scene, assets);
    proc.set_host_paths_allowed(false);
    EXPECT_FALSE(proc.execute({{"cmd", "save_scene"}, {"path", "scene.json"}}).ok);
    EXPECT_FALSE(proc.execute({{"cmd", "load_model"}, {"path", "x.gltf"}}).ok);
    EXPECT_FALSE(proc.execute({{"cmd", "load_texture"}, {"path", "x.png"}}).ok);
    EXPECT_FALSE(proc.execute({{"cmd", "run_commands"}, {"file", "x.jsonl"}}).ok);
    // Non-path commands still work.
    EXPECT_TRUE(proc.execute({{"cmd", "create_entity"}, {"name", "ok"}}).ok);
}

TEST(AgentSecurity, BatchCommandLimitEnforced) {
    AgentSandbox sb;
    Scene scene("s");
    register_builtin_components(scene.world());
    AssetManager assets;
    CommandProcessor proc(scene, assets);
    proc.max_batch_commands = 3;
    const auto batch = sb.proj / "batch.jsonl";
    {
        std::ofstream f(batch);
        for (int i = 0; i < 5; ++i)
            f << "{\"cmd\":\"create_entity\",\"name\":\"e" << i << "\"}\n";
    }
    auto results = proc.execute_file(batch.string());
    ASSERT_EQ(results.size(), 4u); // 3 executed + 1 failure marker
    EXPECT_FALSE(results.back().ok);
    EXPECT_EQ(scene.world().entity_count(), 3u);
}

TEST(AgentSecurity, OversizedCommandsRejected) {
    Scene scene("s");
    register_builtin_components(scene.world());
    AssetManager assets;
    CommandProcessor proc(scene, assets);
    proc.max_command_bytes = 64;
    nlohmann::json big;
    big["cmd"] = "create_entity";
    big["name"] = std::string(1000, 'x');
    EXPECT_FALSE(proc.execute(big).ok);

    // Oversized line inside a batch file fails that line only.
    const auto path = std::filesystem::temp_directory_path() / "ignis_big_line.jsonl";
    {
        std::ofstream f(path);
        f << "{\"cmd\":\"create_entity\",\"name\":\"" << std::string(1000, 'x') << "\"}\n";
        f << "{\"cmd\":\"create_entity\",\"name\":\"fine\"}\n";
    }
    auto results = proc.execute_file(path.string());
    ASSERT_EQ(results.size(), 2u);
    EXPECT_FALSE(results[0].ok);
    EXPECT_TRUE(results[1].ok);
    std::filesystem::remove(path);
}

TEST(AgentSecurity, RunCommandsRecursionIsRejected) {
    Scene scene("rec");
    register_builtin_components(scene.world());
    AssetManager assets;
    CommandProcessor proc(scene, assets);
    namespace fs = std::filesystem;

    // Self-referencing batch: must fail with the guard, not recurse forever.
    const fs::path tmp = fs::temp_directory_path() / "ignis_rec_cmds.jsonl";
    {
        std::ofstream f(tmp);
        f << "{\"cmd\":\"create_entity\",\"name\":\"self_rec\"}\n";
        f << "{\"cmd\":\"run_commands\",\"file\":\"" << tmp.string() << "\"}\n";
    }
    auto results = proc.execute_file(tmp.string());
    ASSERT_FALSE(results.empty());
    // The self-reference must be stopped by the guard after exactly one pass.
    u32 selfRec = 0;
    for (EntityId id : scene.world().all_entities())
        if (scene.world().name_of(id) == "self_rec") ++selfRec;
    EXPECT_EQ(selfRec, 1u) << "guard must stop the self-reference after one pass";
    fs::remove(tmp);

    // Deep chain f0 -> f1 -> ... : each file creates one entity then recurses.
    // The depth guard (max 8) must stop the chain; exactly 8 entities exist.
    const fs::path dir = fs::temp_directory_path() / "ignis_rec_chain";
    fs::remove_all(dir);
    fs::create_directories(dir);
    for (int i = 0; i < 12; ++i) {
        std::ofstream f(dir / ("f" + std::to_string(i) + ".jsonl"));
        f << "{\"cmd\":\"create_entity\",\"name\":\"chain" << i << "\"}\n";
        f << "{\"cmd\":\"run_commands\",\"file\":\"" << (dir / ("f" + std::to_string(i + 1) + ".jsonl")).string()
          << "\"}\n";
    }
    proc.execute_file((dir / "f0.jsonl").string()); // must return, not overflow the stack
    u32 chained = 0;
    for (EntityId id : scene.world().all_entities())
        if (scene.world().name_of(id).rfind("chain", 0) == 0) ++chained;
    EXPECT_EQ(chained, 8u) << "depth guard must cap nesting at 8";
    fs::remove_all(dir);
}
