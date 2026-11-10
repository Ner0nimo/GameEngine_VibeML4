#include <gtest/gtest.h>

#include "ignis/ecs/components.h"
#include "ignis/scene/scene.h"

#include <filesystem>

using namespace ignis;

namespace {
Scene make_scene() {
    Scene scene("demo");
    register_builtin_components(scene.world());
    EntityId parent = scene.world().create_entity("parent");
    EntityId child = scene.world().create_entity("child");
    scene.world().emplace<TransformComponent>(parent).position = {1.f, 0.f, 0.f};
    scene.world().emplace<TransformComponent>(child).position = {0.f, 2.f, 0.f};
    scene.world().emplace<CameraComponent>(child);
    scene.world().set_parent(child, parent);
    nlohmann::json prefab;
    prefab["name"] = "Box";
    prefab["components"]["Transform"]["position"] = {5.f, 5.f, 5.f};
    scene.add_prefab("Box", std::move(prefab));
    return scene;
}
} // namespace

TEST(Scene, SerializeDeserializeRoundtrip) {
    Scene scene = make_scene();
    const nlohmann::json j = scene.serialize();

    Scene loaded = Scene::deserialize(j);
    EXPECT_EQ(loaded.name(), "demo");
    EXPECT_EQ(loaded.world().entity_count(), 2u);

    // Find child by name and check parent link + components survived.
    EntityId child = NULL_ENTITY;
    EntityId parent = NULL_ENTITY;
    for (EntityId e : loaded.world().all_entities()) {
        if (loaded.world().name_of(e) == "child") child = e;
        if (loaded.world().name_of(e) == "parent") parent = e;
    }
    ASSERT_NE(child, NULL_ENTITY);
    ASSERT_NE(parent, NULL_ENTITY);
    EXPECT_EQ(loaded.world().parent_of(child), parent);
    ASSERT_TRUE(loaded.world().has<TransformComponent>(child));
    EXPECT_EQ(loaded.world().get<TransformComponent>(child)->position.y, 2.f);
    ASSERT_TRUE(loaded.world().has<CameraComponent>(child));
    EXPECT_TRUE(loaded.has_prefab("Box"));
}

TEST(Scene, SpawnPrefab) {
    Scene scene = make_scene();
    EntityId e = scene.spawn_prefab("Box");
    ASSERT_NE(e, NULL_ENTITY);
    EXPECT_EQ(scene.world().name_of(e), "Box");
    ASSERT_TRUE(scene.world().has<TransformComponent>(e));
    EXPECT_EQ(scene.world().get<TransformComponent>(e)->position.x, 5.f);
    EXPECT_EQ(scene.spawn_prefab("nope"), NULL_ENTITY);
}

TEST(Scene, SaveLoadFile) {
    Scene scene = make_scene();
    const auto path = std::filesystem::temp_directory_path() / "ignis_test_scene.json";
    ASSERT_TRUE(scene.save_to_file(path.string()).ok());
    auto loaded_r = Scene::load_from_file(path.string());
    ASSERT_TRUE(loaded_r.ok()) << loaded_r.error();
    Scene loaded = std::move(loaded_r.value());
    EXPECT_EQ(loaded.world().entity_count(), 2u);
    std::filesystem::remove(path);
}

TEST(Scene, SystemsRunInOrder) {
    Scene scene("s");
    register_builtin_components(scene.world());
    int order = 0;
    scene.add_system([&](World&, f32) { EXPECT_EQ(order++, 0); });
    scene.add_system([&](World&, f32) { EXPECT_EQ(order++, 1); });
    scene.update(0.016f);
    EXPECT_EQ(order, 2);
}
