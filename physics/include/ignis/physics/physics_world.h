#pragma once

#include "ignis/core/result.h"
#include "ignis/core/types.h"
#include "ignis/ecs/components.h"
#include "ignis/ecs/entity.h"
#include "ignis/math/math.h"

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace reactphysics3d {
class PhysicsCommon;
class PhysicsWorld;
class RigidBody;
class Body;
class CollisionShape;
} // namespace reactphysics3d

namespace ignis {

class Scene;
class World;

// --- Components (registered as "RigidBody" / "Collider") --------------------

struct RigidBodyComponent {
    enum class Type : u8 { Static = 0, Kinematic, Dynamic };
    Type type = Type::Dynamic;
    f32 mass = 1.f;          // Dynamic bodies only
    f32 linear_damping = 0.f;
};

struct ColliderComponent {
    enum class Shape : u8 { Box = 0, Sphere, Capsule };
    Shape shape = Shape::Box;
    math::Vec3 half_extents{0.5f, 0.5f, 0.5f}; // Box
    f32 radius = 0.5f;                          // Sphere / Capsule
    f32 height = 1.f;                           // Capsule (cylinder part)
};

// Clamps untrusted physics component values into valid ranges so a malformed
// scene cannot inject NaN/inf or degenerate geometry into reactphysics3d.
// Applied at deserialization and again at the simulation boundary.
inline void sanitize(RigidBodyComponent& c) {
    if (!std::isfinite(c.mass) || c.mass <= 0.f) c.mass = 1.f; // positive mass for dynamic bodies
    if (!std::isfinite(c.linear_damping) || c.linear_damping < 0.f) c.linear_damping = 0.f;
}

inline void sanitize(ColliderComponent& c) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(c.half_extents[i]) || c.half_extents[i] <= 0.f) c.half_extents[i] = 0.5f;
    }
    if (!std::isfinite(c.radius) || c.radius <= 0.f) c.radius = 0.5f;
    if (!std::isfinite(c.height) || c.height <= 0.f) c.height = 1.f;
}

// --- Serialization traits ----------------------------------------------------

template <>
struct ComponentIO<RigidBodyComponent> {
    static nlohmann::json to_json(const RigidBodyComponent& c) {
        const char* t = c.type == RigidBodyComponent::Type::Static ? "Static"
                        : c.type == RigidBodyComponent::Type::Kinematic ? "Kinematic" : "Dynamic";
        return {{"type", t}, {"mass", c.mass}, {"linear_damping", c.linear_damping}};
    }
    static RigidBodyComponent from_json(const nlohmann::json& j) {
        RigidBodyComponent c;
        const std::string t = j.value("type", "Dynamic");
        if (t == "Static") c.type = RigidBodyComponent::Type::Static;
        else if (t == "Kinematic") c.type = RigidBodyComponent::Type::Kinematic;
        else c.type = RigidBodyComponent::Type::Dynamic;
        if (j.contains("mass")) c.mass = j["mass"].get<f32>();
        if (j.contains("linear_damping")) c.linear_damping = j["linear_damping"].get<f32>();
        sanitize(c);
        return c;
    }
};

template <>
struct ComponentIO<ColliderComponent> {
    static nlohmann::json to_json(const ColliderComponent& c) {
        nlohmann::json j;
        const char* s = c.shape == ColliderComponent::Shape::Box ? "Box"
                        : c.shape == ColliderComponent::Shape::Sphere ? "Sphere" : "Capsule";
        j["shape"] = s;
        ::ignis::to_json(j["half_extents"], c.half_extents);
        j["radius"] = c.radius;
        j["height"] = c.height;
        return j;
    }
    static ColliderComponent from_json(const nlohmann::json& j) {
        ColliderComponent c;
        const std::string s = j.value("shape", "Box");
        if (s == "Sphere") c.shape = ColliderComponent::Shape::Sphere;
        else if (s == "Capsule") c.shape = ColliderComponent::Shape::Capsule;
        else c.shape = ColliderComponent::Shape::Box;
        if (j.contains("half_extents")) ::ignis::from_json(j["half_extents"], c.half_extents);
        if (j.contains("radius")) c.radius = j["radius"].get<f32>();
        if (j.contains("height")) c.height = j["height"].get<f32>();
        sanitize(c);
        return c;
    }
};

struct CollisionEvent {
    EntityId a;
    EntityId b;
};

struct RaycastHit {
    bool hit = false;
    EntityId entity = NULL_ENTITY;
    math::Vec3 point{0.f};
    math::Vec3 normal{0.f};
    f32 distance = 0.f;
};

// Physics simulation backed by reactphysics3d. Add as a scene system:
//   PhysicsSystem physics(scene);
//   scene.add_system([&](World& w, f32 dt){ physics.update(w, dt); });
class PhysicsSystem {
public:
    explicit PhysicsSystem(Scene& scene);
    ~PhysicsSystem();

    PhysicsSystem(const PhysicsSystem&) = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;

    Result<void> init(math::Vec3 gravity = {0.f, -9.81f, 0.f});
    void shutdown();
    bool is_initialized() const { return world_ != nullptr; }

    void update(World& world, f32 dt);

    void set_gravity(math::Vec3 g);
    math::Vec3 gravity() const;

    RaycastHit raycast(math::Vec3 origin, math::Vec3 direction, f32 max_distance) const;

    // Collision events since the last call of take_collision_events().
    std::vector<CollisionEvent> take_collision_events();

    // Lua bindings: ignis.physics.raycast(origin, dir, max_dist) -> table|nil
    static void register_lua_bindings(lua_State* L);
    static PhysicsSystem* active() { return active_; }

    // Registers RigidBodyComponent/ColliderComponent on the world registry.
    static void register_components(World& world);

    // Used by the collision listener (friend-style access via method).
    void push_collision(reactphysics3d::Body* a, reactphysics3d::Body* b);

private:
    void sync_bodies(World& world);
    void sync_transforms(World& world);

    Scene* scene_ = nullptr;
    reactphysics3d::PhysicsCommon* common_ = nullptr;
    reactphysics3d::PhysicsWorld* world_ = nullptr;
    class CollisionListener* listener_ = nullptr;
    // Shapes owned by PhysicsCommon; kind 0=box, 1=sphere, 2=capsule (matches
    // ColliderComponent::Shape) so they can be destroyed with the right call.
    std::vector<std::pair<reactphysics3d::CollisionShape*, u8>> shapes_;
    std::unordered_map<EntityId, reactphysics3d::RigidBody*> bodies_;
    std::unordered_map<reactphysics3d::Body*, EntityId> entities_;
    std::vector<CollisionEvent> collisions_;
    static PhysicsSystem* active_;
};

} // namespace ignis
