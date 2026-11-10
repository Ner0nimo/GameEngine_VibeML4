#include <gtest/gtest.h>

#include "ignis/core/json_parse.h"
#include "ignis/ecs/components.h"
#include "ignis/scene/scene.h"

#include <filesystem>
#include <fstream>

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

TEST(Scene, LightComponentsRoundTrip) {
    Scene scene("lights");
    register_builtin_components(scene.world());
    EntityId sun = scene.world().create_entity("sun");
    auto& dl = scene.world().emplace<DirectionalLightComponent>(sun);
    dl.direction = {0.3f, -1.f, 0.2f};
    dl.color = {1.f, 0.9f, 0.8f};
    dl.intensity = 3.5f;
    dl.cast_shadow = false;
    EntityId lamp = scene.world().create_entity("lamp");
    auto& pl = scene.world().emplace<PointLightComponent>(lamp);
    pl.color = {0.2f, 0.4f, 1.f};
    pl.intensity = 42.f;
    pl.range = 7.5f;
    EntityId torch = scene.world().create_entity("torch");
    auto& sl = scene.world().emplace<SpotLightComponent>(torch);
    sl.direction = {0.f, 0.f, -1.f};
    sl.color = {1.f, 0.5f, 0.25f};
    sl.intensity = 12.f;
    sl.range = 9.f;
    sl.inner_angle_deg = 20.f;
    sl.outer_angle_deg = 35.f;

    Scene loaded = Scene::deserialize(scene.serialize());
    EntityId l_sun = NULL_ENTITY, l_lamp = NULL_ENTITY, l_torch = NULL_ENTITY;
    for (EntityId e : loaded.world().all_entities()) {
        const std::string n = loaded.world().name_of(e);
        if (n == "sun") l_sun = e;
        if (n == "lamp") l_lamp = e;
        if (n == "torch") l_torch = e;
    }
    ASSERT_NE(l_sun, NULL_ENTITY);
    ASSERT_NE(l_lamp, NULL_ENTITY);
    ASSERT_NE(l_torch, NULL_ENTITY);
    const auto* dl2 = loaded.world().get<DirectionalLightComponent>(l_sun);
    ASSERT_NE(dl2, nullptr);
    EXPECT_NEAR(dl2->direction.x, 0.3f, 1e-6f);
    EXPECT_NEAR(dl2->color.y, 0.9f, 1e-6f);
    EXPECT_NEAR(dl2->intensity, 3.5f, 1e-6f);
    EXPECT_FALSE(dl2->cast_shadow);
    const auto* pl2 = loaded.world().get<PointLightComponent>(l_lamp);
    ASSERT_NE(pl2, nullptr);
    EXPECT_NEAR(pl2->color.z, 1.f, 1e-6f);
    EXPECT_NEAR(pl2->intensity, 42.f, 1e-6f);
    EXPECT_NEAR(pl2->range, 7.5f, 1e-6f);
    const auto* sl2 = loaded.world().get<SpotLightComponent>(l_torch);
    ASSERT_NE(sl2, nullptr);
    EXPECT_NEAR(sl2->direction.z, -1.f, 1e-6f);
    EXPECT_NEAR(sl2->color.y, 0.5f, 1e-6f);
    EXPECT_NEAR(sl2->intensity, 12.f, 1e-6f);
    EXPECT_NEAR(sl2->range, 9.f, 1e-6f);
    EXPECT_NEAR(sl2->inner_angle_deg, 20.f, 1e-6f);
    EXPECT_NEAR(sl2->outer_angle_deg, 35.f, 1e-6f);
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

TEST(Scene, LoadJsonReplacesWorldAndPrefabsButKeepsSystems) {
    Scene scene = make_scene();
    int calls = 0;
    scene.add_system([&](World&, f32) { ++calls; });

    Scene other("other");
    register_builtin_components(other.world());
    EntityId e = other.world().create_entity("solo");
    other.world().emplace<TransformComponent>(e).position = {9.f, 9.f, 9.f};
    nlohmann::json prefab;
    prefab["name"] = "P";
    other.add_prefab("P", prefab);

    ASSERT_TRUE(scene.load_json(other.serialize()).ok());
    EXPECT_EQ(scene.name(), "other");
    EXPECT_EQ(scene.world().entity_count(), 1u);
    EXPECT_TRUE(scene.has_prefab("P"));
    EXPECT_FALSE(scene.has_prefab("Box")); // prefabs are replaced, not merged

    EntityId solo = NULL_ENTITY;
    for (EntityId x : scene.world().all_entities()) solo = x;
    ASSERT_NE(solo, NULL_ENTITY);
    EXPECT_EQ(scene.world().name_of(solo), "solo");
    ASSERT_TRUE(scene.world().has<TransformComponent>(solo));
    EXPECT_EQ(scene.world().get<TransformComponent>(solo)->position.x, 9.f);

    // The system pipeline survives load_json (same contract as load_file).
    scene.update(0.016f);
    EXPECT_EQ(calls, 1);
}

TEST(Scene, LoadJsonRoundtripIntoSelf) {
    Scene scene = make_scene();
    ASSERT_TRUE(scene.load_json(scene.serialize()).ok());
    EXPECT_EQ(scene.name(), "demo");
    EXPECT_EQ(scene.world().entity_count(), 2u);
    EXPECT_TRUE(scene.has_prefab("Box"));
    EntityId child = NULL_ENTITY;
    for (EntityId e : scene.world().all_entities())
        if (scene.world().name_of(e) == "child") child = e;
    ASSERT_NE(child, NULL_ENTITY);
    ASSERT_TRUE(scene.world().has<CameraComponent>(child));
}

TEST(Scene, LoadJsonRejectsMalformed) {
    Scene scene = make_scene();
    EXPECT_FALSE(scene.load_json(nlohmann::json::array()).ok()); // not an object
    EXPECT_FALSE(scene.load_json(nlohmann::json(42)).ok());
    // A failed load must not clobber the current scene.
    EXPECT_EQ(scene.world().entity_count(), 2u);
    EXPECT_EQ(scene.name(), "demo");
}

namespace {

nlohmann::json entity_json(u32 id, const std::string& name = "e") {
    nlohmann::json ej;
    ej["id"] = id;
    ej["name"] = name;
    return ej;
}

} // namespace

TEST(SceneSecurity, NonObjectJsonRejected) {
    Scene scene("s");
    register_builtin_components(scene.world());
    EXPECT_FALSE(scene.load_json(nlohmann::json::array()).ok());
    EXPECT_FALSE(scene.load_json(nlohmann::json(42)).ok());
}

TEST(SceneSecurity, DuplicateEntityIdsRejected) {
    Scene scene("s");
    register_builtin_components(scene.world());
    nlohmann::json j;
    j["name"] = "dup";
    j["entities"] = {entity_json(1, "a"), entity_json(1, "b")};
    EXPECT_FALSE(scene.load_json(j).ok());
}

TEST(SceneSecurity, MissingOrNonNumericIdRejected) {
    Scene scene("s");
    register_builtin_components(scene.world());
    nlohmann::json j;
    j["entities"] = nlohmann::json::array();
    j["entities"].push_back({{"name", "no id"}});
    EXPECT_FALSE(scene.load_json(j).ok());
    j["entities"][0] = {{"id", "not-a-number"}, {"name", "x"}};
    EXPECT_FALSE(scene.load_json(j).ok());
}

TEST(SceneSecurity, EntityCountLimitEnforced) {
    Scene scene("s");
    register_builtin_components(scene.world());
    nlohmann::json j;
    j["entities"] = nlohmann::json::array();
    for (u32 i = 1; i <= 5; ++i) j["entities"].push_back(entity_json(i));
    SceneLimits limits;
    limits.max_entities = 2;
    EXPECT_FALSE(scene.load_json(j, limits).ok());
    EXPECT_TRUE(scene.load_json(j).ok()); // default limits accept it
}

TEST(SceneSecurity, ComponentsPerEntityLimitEnforced) {
    Scene scene("s");
    register_builtin_components(scene.world());
    nlohmann::json j;
    nlohmann::json ej = entity_json(1);
    ej["components"]["Transform"] = nlohmann::json::object();
    ej["components"]["Camera"] = nlohmann::json::object();
    ej["components"]["Light"] = nlohmann::json::object();
    j["entities"] = {ej};
    SceneLimits limits;
    limits.max_components_per_entity = 2;
    EXPECT_FALSE(scene.load_json(j, limits).ok());
}

TEST(SceneSecurity, DeeplyNestedJsonRejected) {
    const std::string deep = std::string(100000, '[') + std::string(100000, ']');
    auto parsed = parse_json_bounded(deep);
    EXPECT_FALSE(parsed.ok()); // bounded depth: parse error, no stack overflow
    const auto path = std::filesystem::temp_directory_path() / "ignis_deep_scene.json";
    {
        std::ofstream f(path);
        f << "{\"name\":\"x\",\"entities\":" << deep << "}";
    }
    Scene scene("s");
    register_builtin_components(scene.world());
    EXPECT_FALSE(scene.load_file(path.string()).ok());
    std::filesystem::remove(path);
}

TEST(SceneSecurity, OversizedSceneFileRejected) {
    const auto path = std::filesystem::temp_directory_path() / "ignis_big_scene.json";
    {
        std::ofstream f(path);
        f << "{\"name\":\"" << std::string(1000, 'x') << "\",\"entities\":[]}";
    }
    Scene scene("s");
    register_builtin_components(scene.world());
    SceneLimits limits;
    limits.max_file_bytes = 16;
    EXPECT_FALSE(scene.load_file(path.string(), limits).ok());
    EXPECT_TRUE(scene.load_file(path.string()).ok()); // default limit accepts
    std::filesystem::remove(path);
}

TEST(SceneSecurity, MalformedJsonFileRejected) {
    const auto path = std::filesystem::temp_directory_path() / "ignis_bad_scene.json";
    {
        std::ofstream f(path);
        f << "{\"name\": \"truncated\"  ";
    }
    Scene scene("s");
    register_builtin_components(scene.world());
    EXPECT_FALSE(scene.load_file(path.string()).ok());
    std::filesystem::remove(path);
}
