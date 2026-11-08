#include <gtest/gtest.h>

#include "ignis/ecs/components.h"
#include "ignis/physics/physics_world.h"
#include "ignis/scene/scene.h"

using namespace ignis;

namespace {

Scene make_physics_scene() {
    Scene scene("physics_test");
    register_builtin_components(scene.world());
    PhysicsSystem::register_components(scene.world());

    // Static ground: 20x1x20 box centered at origin.
    EntityId ground = scene.world().create_entity("ground");
    scene.world().emplace<TransformComponent>(ground);
    auto& grb = scene.world().emplace<RigidBodyComponent>(ground);
    grb.type = RigidBodyComponent::Type::Static;
    auto& gc = scene.world().emplace<ColliderComponent>(ground);
    gc.shape = ColliderComponent::Shape::Box;
    gc.half_extents = {10.f, 1.f, 10.f};

    // Dynamic box 5 units above the ground.
    EntityId box = scene.world().create_entity("box");
    auto& bt = scene.world().emplace<TransformComponent>(box);
    bt.position = {0.f, 5.f, 0.f};
    auto& brb = scene.world().emplace<RigidBodyComponent>(box);
    brb.type = RigidBodyComponent::Type::Dynamic;
    brb.mass = 1.f;
    auto& bc = scene.world().emplace<ColliderComponent>(box);
    bc.shape = ColliderComponent::Shape::Box;
    bc.half_extents = {0.5f, 0.5f, 0.5f};
    return scene;
}

} // namespace

TEST(Physics, BoxFallsAndRestsOnGround) {
    Scene scene = make_physics_scene();
    PhysicsSystem physics(scene);
    std::string err;
    ASSERT_TRUE(physics.init({0.f, -9.81f, 0.f}, &err)) << err;

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
    std::string err;
    ASSERT_TRUE(physics.init({0.f, -9.81f, 0.f}, &err));
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
    auto& grb = scene.world().emplace<RigidBodyComponent>(ground);
    grb.type = RigidBodyComponent::Type::Static;
    auto& gc = scene.world().emplace<ColliderComponent>(ground);
    gc.shape = ColliderComponent::Shape::Box;
    gc.half_extents = {10.f, 1.f, 10.f};

    EntityId ball = scene.world().create_entity("ball");
    auto& bt = scene.world().emplace<TransformComponent>(ball);
    bt.position = {0.f, 4.f, 0.f};
    scene.world().emplace<RigidBodyComponent>(ball); // dynamic, mass 1
    auto& bc = scene.world().emplace<ColliderComponent>(ball);
    bc.shape = ColliderComponent::Shape::Sphere;
    bc.radius = 0.5f;

    PhysicsSystem physics(scene);
    std::string err;
    ASSERT_TRUE(physics.init({0.f, -9.81f, 0.f}, &err));
    for (int i = 0; i < 180; ++i) physics.update(scene.world(), 1.f / 60.f);
    // Sphere rests at ground top (1) + radius (0.5).
    EXPECT_NEAR(scene.world().get<TransformComponent>(ball)->position.y, 1.5f, 0.05f);
}
