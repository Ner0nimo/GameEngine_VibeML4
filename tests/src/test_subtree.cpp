#include <gtest/gtest.h>

#include "ignis/ecs/components.h"
#include "ignis/ecs/subtree.h"
#include "ignis/scene/scene.h"

using namespace ignis;

namespace {

// Test-only component holding an entity reference, serialized under the
// "entity" key (the convention the agent command API also uses).
struct RefComponent {
    EntityId target = NULL_ENTITY;
};

} // namespace

template <>
struct ignis::ComponentIO<RefComponent> {
    static nlohmann::json to_json(const RefComponent& c) { return {{"entity", c.target}}; }
    static RefComponent from_json(const nlohmann::json& j) {
        return RefComponent{j.value("entity", 0u)};
    }
};

namespace {

Scene make_subtree_scene() {
    Scene scene("sub");
    register_builtin_components(scene.world());
    scene.world().register_component_type<RefComponent>("Ref");

    EntityId root = scene.world().create_entity("root");
    scene.world().emplace<TransformComponent>(root)->position = {1.f, 0.f, 0.f};
    EntityId child = scene.world().create_entity("child");
    scene.world().emplace<TransformComponent>(child)->position = {0.f, 2.f, 0.f};
    scene.world().emplace<RefComponent>(child)->target = root; // child references parent
    EntityId grand = scene.world().create_entity("grand");
    scene.world().emplace<CameraComponent>(grand);
    scene.world().set_parent(child, root);
    scene.world().set_parent(grand, child);
    return scene;
}

EntityId find_by_name(World& w, const std::string& name) {
    for (EntityId e : w.all_entities())
        if (w.name_of(e) == name) return e;
    return NULL_ENTITY;
}

} // namespace

TEST(Subtree, RoundTripPreservesStructureAndComponents) {
    Scene scene = make_subtree_scene();
    const EntityId root = find_by_name(scene.world(), "root");
    const nlohmann::json j = serialize_subtree(scene.world(), root);

    Scene copy("copy");
    register_builtin_components(copy.world());
    copy.world().register_component_type<RefComponent>("Ref");
    const EntityId new_root = spawn_subtree(copy.world(), j);
    ASSERT_NE(new_root, NULL_ENTITY);
    EXPECT_EQ(copy.world().entity_count(), 3u);

    const EntityId new_child = find_by_name(copy.world(), "child");
    const EntityId new_grand = find_by_name(copy.world(), "grand");
    ASSERT_NE(new_child, NULL_ENTITY);
    ASSERT_NE(new_grand, NULL_ENTITY);
    EXPECT_EQ(copy.world().parent_of(new_root), NULL_ENTITY);
    EXPECT_EQ(copy.world().parent_of(new_child), new_root);
    EXPECT_EQ(copy.world().parent_of(new_grand), new_child);
    ASSERT_TRUE(copy.world().has<TransformComponent>(new_root));
    EXPECT_EQ(copy.world().get<TransformComponent>(new_root)->position.x, 1.f);
    EXPECT_TRUE(copy.world().has<CameraComponent>(new_grand));
}

TEST(Subtree, SpawnTwiceGetsFreshNonCollidingIds) {
    Scene scene = make_subtree_scene();
    const EntityId root = find_by_name(scene.world(), "root");
    const nlohmann::json j = serialize_subtree(scene.world(), root);

    Scene copy("copy");
    register_builtin_components(copy.world());
    copy.world().register_component_type<RefComponent>("Ref");
    const EntityId a = spawn_subtree(copy.world(), j);
    const EntityId b = spawn_subtree(copy.world(), j);
    EXPECT_NE(a, NULL_ENTITY);
    EXPECT_NE(b, NULL_ENTITY);
    EXPECT_NE(a, b);
    EXPECT_EQ(copy.world().entity_count(), 6u);
}

TEST(Subtree, EntityReferencesAreRemappedToNewIds) {
    Scene scene = make_subtree_scene();
    const EntityId root = find_by_name(scene.world(), "root");
    const nlohmann::json j = serialize_subtree(scene.world(), root);

    // Spawn into the SAME world so old and new ids actually differ.
    const EntityId new_root = spawn_subtree(scene.world(), j);
    ASSERT_NE(new_root, NULL_ENTITY);
    EXPECT_NE(new_root, root);
    // Find the copied child: the "child" whose parent is the new root.
    EntityId new_child = NULL_ENTITY;
    for (EntityId c : scene.world().children_of(new_root))
        if (scene.world().name_of(c) == "child") new_child = c;
    ASSERT_NE(new_child, NULL_ENTITY);
    ASSERT_TRUE(scene.world().has<RefComponent>(new_child));
    // The copy's reference must point at the NEW root, not the original one.
    EXPECT_EQ(scene.world().get<RefComponent>(new_child)->target, new_root);
    EXPECT_NE(scene.world().get<RefComponent>(new_child)->target, root);
}

TEST(Subtree, MalformedInputReturnsNullAndLeavesWorldUntouched) {
    Scene scene = make_subtree_scene();
    const u64 before = scene.world().entity_count();
    EXPECT_EQ(spawn_subtree(scene.world(), nlohmann::json::array()), NULL_ENTITY);
    EXPECT_EQ(spawn_subtree(scene.world(), nlohmann::json(42)), NULL_ENTITY);
    EXPECT_EQ(scene.world().entity_count(), before);
}

TEST(Subtree, PrefabWithChildrenSpawnsWholeSubtree) {
    Scene scene = make_subtree_scene();
    const EntityId root = find_by_name(scene.world(), "root");
    scene.add_prefab("Tree", serialize_subtree(scene.world(), root));

    const EntityId spawned = scene.spawn_prefab("Tree");
    ASSERT_NE(spawned, NULL_ENTITY);
    // root + child + grandchild = 3 original + 3 spawned
    EXPECT_EQ(scene.world().entity_count(), 6u);
    const EntityId s_child = find_by_name(scene.world(), "child");
    ASSERT_NE(s_child, NULL_ENTITY);
    // the spawned root has exactly one child (the copied "child")
    EXPECT_EQ(scene.world().children_of(spawned).size(), 1u);
}

TEST(Subtree, MalformedDeepChildLeavesWorldUntouched) {
    Scene scene = make_subtree_scene();
    const u64 before = scene.world().entity_count();
    // Root is valid, but a nested child is not an object -> whole spawn fails
    // up front, no orphaned entities.
    nlohmann::json j;
    j["name"] = "root";
    j["children"] = nlohmann::json::array();
    j["children"].push_back(42);                 // non-object child
    j["children"].push_back(nlohmann::json::object()); // object without "name"
    EXPECT_EQ(spawn_subtree(scene.world(), j), NULL_ENTITY);
    EXPECT_EQ(scene.world().entity_count(), before);
    // And a structurally valid tree still spawns fine.
    nlohmann::json ok;
    ok["name"] = "root";
    nlohmann::json child;
    child["name"] = "kid";
    ok["children"] = nlohmann::json::array();
    ok["children"].push_back(child);
    EXPECT_NE(spawn_subtree(scene.world(), ok), NULL_ENTITY);
    EXPECT_EQ(scene.world().entity_count(), before + 2);
}
