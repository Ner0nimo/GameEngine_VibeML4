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
