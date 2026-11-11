#include <gtest/gtest.h>

#include "ignis/ecs/components.h"
#include "ignis/ecs/world.h"

using namespace ignis;

TEST(Ecs, CreateDestroyRecycle) {
    World w;
    EntityId a = w.create_entity("a");
    EntityId b = w.create_entity("b");
    EXPECT_TRUE(w.alive(a));
    EXPECT_TRUE(w.alive(b));
    EXPECT_EQ(w.entity_count(), 2u);
    w.destroy_entity(a);
    EXPECT_FALSE(w.alive(a));
    EXPECT_EQ(w.entity_count(), 1u);
    EntityId c = w.create_entity("c");
    EXPECT_EQ(c, a); // id recycled
    EXPECT_EQ(w.name_of(c), "c");
}

TEST(Ecs, Components) {
    World w;
    register_builtin_components(w);
    EntityId e = w.create_entity("e");
    EXPECT_FALSE(w.has<TransformComponent>(e));
    auto* t = w.emplace<TransformComponent>(e);
    t->position = {1.f, 2.f, 3.f};
    EXPECT_TRUE(w.has<TransformComponent>(e));
    EXPECT_EQ(w.get<TransformComponent>(e)->position.y, 2.f);
    w.remove<TransformComponent>(e);
    EXPECT_FALSE(w.has<TransformComponent>(e));
    EXPECT_EQ(w.get<TransformComponent>(e), nullptr);
}

TEST(Ecs, ViewFilters) {
    World w;
    register_builtin_components(w);
    EntityId a = w.create_entity("a");
    EntityId b = w.create_entity("b");
    w.emplace<TransformComponent>(a);
    w.emplace<TransformComponent>(b);
    w.emplace<CameraComponent>(b);
    u32 count_all = 0;
    for (EntityId e : w.view<TransformComponent>()) { (void)e; ++count_all; }
    EXPECT_EQ(count_all, 2u);
    u32 count_cam = 0;
    for (EntityId e : w.view<TransformComponent, CameraComponent>()) {
        EXPECT_EQ(e, b);
        ++count_cam;
    }
    EXPECT_EQ(count_cam, 1u);
}

TEST(Ecs, Hierarchy) {
    World w;
    register_builtin_components(w);
    EntityId parent = w.create_entity("parent");
    EntityId child = w.create_entity("child");
    EXPECT_TRUE(w.set_parent(child, parent));
    EXPECT_EQ(w.parent_of(child), parent);
    ASSERT_EQ(w.children_of(parent).size(), 1u);
    EXPECT_EQ(w.children_of(parent)[0], child);

    // Destroying the parent re-parents the child to root.
    w.destroy_entity(parent);
    EXPECT_EQ(w.parent_of(child), NULL_ENTITY);
}

TEST(Ecs, SetParentRejectsCyclesAndDead) {
    World w;
    register_builtin_components(w);
    EntityId a = w.create_entity("a");
    EntityId b = w.create_entity("b");
    EntityId c = w.create_entity("c");
    ASSERT_TRUE(w.set_parent(b, a));
    ASSERT_TRUE(w.set_parent(c, b));
    // a -> b -> c chain; making a a child of c (or of itself) must fail.
    EXPECT_FALSE(w.set_parent(a, c));
    EXPECT_FALSE(w.set_parent(a, a));
    // Dead parent / dead child must fail and change nothing.
    EXPECT_FALSE(w.set_parent(a, 999u));
    EXPECT_FALSE(w.set_parent(999u, a));
    EXPECT_EQ(w.parent_of(a), NULL_ENTITY);
    EXPECT_EQ(w.parent_of(c), b);
    // world_matrix still terminates and is correct.
    w.emplace<TransformComponent>(a)->position = {1.f, 0.f, 0.f};
    w.emplace<TransformComponent>(b)->position = {0.f, 2.f, 0.f};
    w.emplace<TransformComponent>(c)->position = {0.f, 0.f, 3.f};
    const math::Mat4 m = world_matrix(w, c);
    const glm::vec4 p = m * glm::vec4(0.f, 0.f, 0.f, 1.f);
    EXPECT_NEAR(p.x, 1.f, 1e-5f);
    EXPECT_NEAR(p.y, 2.f, 1e-5f);
    EXPECT_NEAR(p.z, 3.f, 1e-5f);
}

TEST(Ecs, WorldMatrix) {
    World w;
    register_builtin_components(w);
    EntityId parent = w.create_entity("parent");
    EntityId child = w.create_entity("child");
    w.emplace<TransformComponent>(parent)->position = {10.f, 0.f, 0.f};
    auto* ct = w.emplace<TransformComponent>(child);
    ct->position = {0.f, 5.f, 0.f};
    EXPECT_TRUE(w.set_parent(child, parent));
    const math::Mat4 m = world_matrix(w, child);
    const glm::vec4 p = m * glm::vec4(0.f, 0.f, 0.f, 1.f);
    EXPECT_NEAR(p.x, 10.f, 1e-5f);
    EXPECT_NEAR(p.y, 5.f, 1e-5f);
    EXPECT_NEAR(p.z, 0.f, 1e-5f);
}

TEST(Ecs, RegistryByName) {
    World w;
    register_builtin_components(w);
    const auto* info = w.registry().find_by_name("Transform");
    ASSERT_NE(info, nullptr);
    EntityId e = w.create_entity("e");
    nlohmann::json props = {{"position", {7.f, 8.f, 9.f}}};
    ASSERT_TRUE(info->deserialize(w, e, props));
    EXPECT_EQ(w.get<TransformComponent>(e)->position.x, 7.f);
    const nlohmann::json out = info->serialize(w, e);
    EXPECT_EQ(out["position"][0].get<f32>(), 7.f);
}

// Security regression (#46): component operations must reject dead entities.
// A stale id (from untrusted scene JSON, Lua or agent commands) must never
// attach components to a destroyed entity, and has() must report liveness,
// not raw storage presence.
TEST(Ecs, ComponentOpsRejectDeadEntities) {
    World w;
    register_builtin_components(w);
    EntityId e = w.create_entity("e");
    ASSERT_NE(w.emplace<TransformComponent>(e), nullptr);
    w.destroy_entity(e);
    ASSERT_FALSE(w.alive(e));

    // has/get answer "nothing on this id" for a dead entity.
    EXPECT_FALSE(w.has<TransformComponent>(e));
    EXPECT_EQ(w.get<TransformComponent>(e), nullptr);

    // emplace refuses to attach; remove is a safe no-op.
    EXPECT_EQ(w.emplace<TransformComponent>(e), nullptr);
    w.remove<TransformComponent>(e);

    // NULL_ENTITY is never alive either.
    EXPECT_EQ(w.emplace<TransformComponent>(NULL_ENTITY), nullptr);
    EXPECT_FALSE(w.has<TransformComponent>(NULL_ENTITY));
    EXPECT_EQ(w.get<TransformComponent>(NULL_ENTITY), nullptr);

    // The destroyed id is recycled: the new entity starts component-clean,
    // and can opt in again.
    EntityId r = w.create_entity("r");
    EXPECT_EQ(r, e); // id recycled
    EXPECT_FALSE(w.has<TransformComponent>(r));
    EXPECT_NE(w.emplace<TransformComponent>(r), nullptr);
    EXPECT_TRUE(w.has<TransformComponent>(r));
}

// The registry (scene loading, Lua add_component, agent add_component) must
// report failure instead of writing components onto dead entities.
TEST(Ecs, RegistryDeserializeRejectsDeadEntity) {
    World w;
    register_builtin_components(w);
    const auto* info = w.registry().find_by_name("Transform");
    ASSERT_NE(info, nullptr);
    EntityId e = w.create_entity("e");
    w.destroy_entity(e);

    const nlohmann::json props = {{"position", {1.f, 2.f, 3.f}}};
    EXPECT_FALSE(info->deserialize(w, e, props));
    EXPECT_FALSE(w.has<TransformComponent>(e));
    EXPECT_TRUE(info->serialize(w, e).empty());
    info->remove(w, e); // no-op, must not crash
}
