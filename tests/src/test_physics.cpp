#include <gtest/gtest.h>

#include "ignis/ecs/components.h"
#include "ignis/physics/debug_draw.h"
#include "ignis/physics/physics_world.h"
#include "ignis/scene/scene.h"

#include <cmath>
#include <limits>

using namespace ignis;

namespace {

Scene make_physics_scene() {
    Scene scene("physics_test");
    register_builtin_components(scene.world());
    PhysicsSystem::register_components(scene.world());

    // Static ground: 20x1x20 box centered at origin.
    EntityId ground = scene.world().create_entity("ground");
    scene.world().emplace<TransformComponent>(ground);
    auto* grb = scene.world().emplace<RigidBodyComponent>(ground);
    grb->type = RigidBodyComponent::Type::Static;
    auto* gc = scene.world().emplace<ColliderComponent>(ground);
    gc->shape = ColliderComponent::Shape::Box;
    gc->half_extents = {10.f, 1.f, 10.f};

    // Dynamic box 5 units above the ground.
    EntityId box = scene.world().create_entity("box");
    auto* bt = scene.world().emplace<TransformComponent>(box);
    bt->position = {0.f, 5.f, 0.f};
    auto* brb = scene.world().emplace<RigidBodyComponent>(box);
    brb->type = RigidBodyComponent::Type::Dynamic;
    brb->mass = 1.f;
    auto* bc = scene.world().emplace<ColliderComponent>(box);
    bc->shape = ColliderComponent::Shape::Box;
    bc->half_extents = {0.5f, 0.5f, 0.5f};
    return scene;
}

} // namespace

TEST(Physics, BoxFallsAndRestsOnGround) {
    Scene scene = make_physics_scene();
    PhysicsSystem physics(scene);
    ASSERT_TRUE(physics.init({0.f, -9.81f, 0.f}).ok());

    EntityId box = NULL_ENTITY;
    for (EntityId e : scene.world().all_entities()) {
        if (scene.world().name_of(e) == "box") box = e;
    }
    ASSERT_NE(box, NULL_ENTITY);
    const f32 start_y = scene.world().get<TransformComponent>(box)->position.y;
    EXPECT_NEAR(start_y, 5.f, 1e-6f);

    const f32 dt = 1.f / 60.f;
    for (int i = 0; i < 180; ++i) physics.update(scene.world(), dt); // 3 seconds

    const f32 end_y = scene.world().get<TransformComponent>(box)->position.y;
    EXPECT_LT(end_y, start_y);                    // it fell
    EXPECT_NEAR(end_y, 1.5f, 0.05f);              // rests on ground top (y=1) + half extent (0.5)

    // At least one collision event between box and ground was reported.
    const auto events = physics.take_collision_events();
    EXPECT_FALSE(events.empty());
}

TEST(Physics, RaycastHitsGround) {
    Scene scene = make_physics_scene();
    PhysicsSystem physics(scene);
    ASSERT_TRUE(physics.init({0.f, -9.81f, 0.f}).ok());
    physics.update(scene.world(), 1.f / 60.f); // create bodies

    // Offset in X so the ray misses the falling box (at x=0) and hits the ground.
    const RaycastHit hit = physics.raycast({5.f, 10.f, 0.f}, {0.f, -1.f, 0.f}, 100.f);
    EXPECT_TRUE(hit.hit);
    EXPECT_NEAR(hit.point.y, 1.f, 0.05f);
    EXPECT_GT(hit.entity, 0u);

    const RaycastHit miss = physics.raycast({5.f, 10.f, 0.f}, {0.f, 1.f, 0.f}, 100.f);
    EXPECT_FALSE(miss.hit);
}

TEST(Physics, SphereCollider) {
    Scene scene("sphere_test");
    register_builtin_components(scene.world());
    PhysicsSystem::register_components(scene.world());
    EntityId ground = scene.world().create_entity("ground");
    scene.world().emplace<TransformComponent>(ground);
    auto* grb = scene.world().emplace<RigidBodyComponent>(ground);
    grb->type = RigidBodyComponent::Type::Static;
    auto* gc = scene.world().emplace<ColliderComponent>(ground);
    gc->shape = ColliderComponent::Shape::Box;
    gc->half_extents = {10.f, 1.f, 10.f};

    EntityId ball = scene.world().create_entity("ball");
    auto* bt = scene.world().emplace<TransformComponent>(ball);
    bt->position = {0.f, 4.f, 0.f};
    scene.world().emplace<RigidBodyComponent>(ball); // dynamic, mass 1
    auto* bc = scene.world().emplace<ColliderComponent>(ball);
    bc->shape = ColliderComponent::Shape::Sphere;
    bc->radius = 0.5f;

    PhysicsSystem physics(scene);
    ASSERT_TRUE(physics.init({0.f, -9.81f, 0.f}).ok());
    for (int i = 0; i < 180; ++i) physics.update(scene.world(), 1.f / 60.f);
    // Sphere rests at ground top (1) + radius (0.5).
    EXPECT_NEAR(scene.world().get<TransformComponent>(ball)->position.y, 1.5f, 0.05f);
}

namespace {

nlohmann::json bad_rigidbody_json() {
    nlohmann::json j;
    j["type"] = "Dynamic";
    j["mass"] = -5.f;                 // negative mass on a dynamic body
    j["linear_damping"] = -1.f;       // negative damping
    return j;
}

} // namespace

TEST(PhysicsSecurity, RigidBodyFromJsonSanitizesInvalidValues) {
    auto c = ComponentIO<RigidBodyComponent>::from_json(bad_rigidbody_json());
    EXPECT_EQ(c.mass, 1.f);
    EXPECT_EQ(c.linear_damping, 0.f);

    nlohmann::json nan_j;
    nan_j["mass"] = std::numeric_limits<double>::quiet_NaN(); // via Lua-converted props
    auto c2 = ComponentIO<RigidBodyComponent>::from_json(nan_j);
    EXPECT_EQ(c2.mass, 1.f);
    EXPECT_TRUE(std::isfinite(c2.mass));
}

TEST(PhysicsSecurity, ColliderFromJsonSanitizesInvalidValues) {
    nlohmann::json j;
    j["shape"] = "Sphere";
    j["radius"] = 0.f;                       // zero radius
    j["half_extents"] = {-1.f, 0.5f, 0.f};   // negative / zero half extents
    j["height"] = -2.f;                      // invalid capsule height
    auto c = ComponentIO<ColliderComponent>::from_json(j);
    EXPECT_EQ(c.radius, 0.5f);
    EXPECT_EQ(c.half_extents.x, 0.5f);
    EXPECT_EQ(c.half_extents.z, 0.5f);
    EXPECT_EQ(c.height, 1.f);
}

TEST(PhysicsSecurity, TransformFromJsonSanitizesInvalidValues) {
    nlohmann::json j;
    j["position"] = {std::numeric_limits<double>::quiet_NaN(), 1.f, 2.f};
    j["rotation"] = {0.f, 0.f, 0.f, 0.f};     // zero quaternion
    j["scale"] = {0.f, -2.f, 1.f};            // zero / negative scale
    auto t = ComponentIO<TransformComponent>::from_json(j);
    EXPECT_EQ(t.position.x, 0.f);
    EXPECT_TRUE(std::isfinite(t.position.y));
    EXPECT_NEAR(glm::length(t.rotation), 1.f, 1e-6f); // normalized, nonzero
    EXPECT_EQ(t.scale.x, 1.f);
    EXPECT_EQ(t.scale.y, 1.f);

    // Non-unit but valid quaternion gets normalized.
    nlohmann::json j2;
    j2["rotation"] = {2.f, 0.f, 0.f, 0.f};
    auto t2 = ComponentIO<TransformComponent>::from_json(j2);
    EXPECT_NEAR(glm::length(t2.rotation), 1.f, 1e-6f);
    EXPECT_NEAR(t2.rotation.w, 1.f, 1e-6f);
}

TEST(PhysicsSecurity, SceneLoadSanitizesPhysicsComponents) {
    Scene scene("sec");
    register_builtin_components(scene.world());
    PhysicsSystem::register_components(scene.world());
    nlohmann::json j;
    nlohmann::json ej;
    ej["id"] = 1;
    ej["name"] = "e";
    ej["components"]["RigidBody"] = bad_rigidbody_json();
    ej["components"]["Collider"] = {{"shape", "Sphere"}, {"radius", 0.f}};
    ej["components"]["Transform"] = {{"rotation", {0.f, 0.f, 0.f, 0.f}}};
    j["entities"] = nlohmann::json::array();
    j["entities"].push_back(ej);
    ASSERT_TRUE(scene.load_json(j).ok());
    const auto* rb = scene.world().get<RigidBodyComponent>(1);
    const auto* col = scene.world().get<ColliderComponent>(1);
    const auto* tr = scene.world().get<TransformComponent>(1);
    ASSERT_NE(rb, nullptr);
    ASSERT_NE(col, nullptr);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(rb->mass, 1.f);
    EXPECT_EQ(rb->linear_damping, 0.f);
    EXPECT_EQ(col->radius, 0.5f);
    EXPECT_NEAR(glm::length(tr->rotation), 1.f, 1e-6f);
}

TEST(PhysicsSecurity, SyncBodiesClampsDirectlyEmplacedGarbage) {
    Scene scene = make_physics_scene();
    PhysicsSystem physics(scene);
    ASSERT_TRUE(physics.init({0.f, -9.81f, 0.f}).ok());
    EntityId box = NULL_ENTITY;
    for (EntityId e : scene.world().all_entities()) {
        if (scene.world().name_of(e) == "box") box = e;
    }
    ASSERT_NE(box, NULL_ENTITY);
    // Bypass deserialization entirely (editor-style direct mutation).
    scene.world().get<RigidBodyComponent>(box)->mass =
        std::numeric_limits<float>::quiet_NaN();
    scene.world().get<ColliderComponent>(box)->radius = -1.f;
    for (int i = 0; i < 5; ++i) physics.update(scene.world(), 1.f / 60.f); // must not crash
    // Sanitized values are written back to the world.
    EXPECT_EQ(scene.world().get<RigidBodyComponent>(box)->mass, 1.f);
    EXPECT_EQ(scene.world().get<ColliderComponent>(box)->radius, 0.5f);
}

// --- editor debug-draw geometry (issue #14) -----------------------------------

TEST(PhysicsDebugDraw, BoxWireframeHas12Edges) {
    ColliderComponent c;
    c.shape = ColliderComponent::Shape::Box;
    c.half_extents = {1.f, 2.f, 3.f};
    const auto edges = collider_wireframe_edges(c, math::Mat4(1.f));
    EXPECT_EQ(edges.size(), 12u);
    // All corners of the box must appear as edge endpoints.
    for (const auto& [a, b] : edges) {
        for (const math::Vec3* p : {&a, &b}) {
            EXPECT_TRUE(std::fabs(std::fabs(p->x) - 1.f) < 1e-5f);
            EXPECT_TRUE(std::fabs(std::fabs(p->y) - 2.f) < 1e-5f);
            EXPECT_TRUE(std::fabs(std::fabs(p->z) - 3.f) < 1e-5f);
        }
    }
}

TEST(PhysicsDebugDraw, SphereWireframeHas12EdgesAtRadius) {
    ColliderComponent c;
    c.shape = ColliderComponent::Shape::Sphere;
    c.radius = 2.f;
    const auto edges = collider_wireframe_edges(c, math::Mat4(1.f));
    EXPECT_EQ(edges.size(), 12u);
    for (const auto& [a, b] : edges) {
        EXPECT_NEAR(glm::length(a), 2.f, 1e-4f);
        EXPECT_NEAR(glm::length(b), 2.f, 1e-4f);
    }
}

TEST(PhysicsDebugDraw, CapsuleWireframeHas24Edges) {
    ColliderComponent c;
    c.shape = ColliderComponent::Shape::Capsule;
    c.radius = 0.5f;
    c.height = 2.f;
    const auto edges = collider_wireframe_edges(c, math::Mat4(1.f));
    EXPECT_EQ(edges.size(), 24u);
}

TEST(PhysicsDebugDraw, WorldMatrixTransformsEdges) {
    ColliderComponent c;
    c.shape = ColliderComponent::Shape::Box;
    c.half_extents = {1.f, 1.f, 1.f};
    const math::Mat4 world = glm::translate(math::Mat4(1.f), math::Vec3(10.f, 5.f, -2.f));
    const auto edges = collider_wireframe_edges(c, world);
    EXPECT_EQ(edges.size(), 12u);
    for (const auto& [a, b] : edges) {
        for (const math::Vec3* p : {&a, &b}) {
            // every corner sits within half-extent 1 of the translated center
            EXPECT_TRUE(std::fabs(p->x - 10.f) <= 1.f + 1e-4f);
            EXPECT_TRUE(std::fabs(p->y - 5.f) <= 1.f + 1e-4f);
            EXPECT_TRUE(std::fabs(p->z + 2.f) <= 1.f + 1e-4f);
        }
    }
    // and the translated center is actually reached (corner at +x,+y,+z)
    bool found_corner = false;
    for (const auto& [a, b] : edges)
        for (const math::Vec3* p : {&a, &b})
            if (std::fabs(p->x - 11.f) < 1e-4f && std::fabs(p->y - 6.f) < 1e-4f &&
                std::fabs(p->z + 1.f) < 1e-4f)
                found_corner = true;
    EXPECT_TRUE(found_corner);
}

TEST(PhysicsDebugDraw, AabbEdges) {
    const auto edges = aabb_edges(math::Vec3(1.f, 2.f, 3.f), math::Vec3(0.5f, 0.5f, 0.5f));
    EXPECT_EQ(edges.size(), 12u);
    // corners span center +/- half
    f32 min_x = 1e30f, max_x = -1e30f;
    for (const auto& [a, b] : edges) {
        min_x = std::min({min_x, a.x, b.x});
        max_x = std::max({max_x, a.x, b.x});
    }
    EXPECT_NEAR(min_x, 0.5f, 1e-5f);
    EXPECT_NEAR(max_x, 1.5f, 1e-5f);
}
