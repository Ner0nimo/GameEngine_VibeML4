#include "ignis/physics/physics_world.h"

#include "ignis/core/log.h"
#include "ignis/ecs/component_registry.h"
#include "ignis/ecs/components.h"
#include "ignis/ecs/world.h"
#include "ignis/scene/scene.h"

#include "lua_all.h"

#include <reactphysics3d/reactphysics3d.h>

#include <vector>

namespace ignis {

namespace rp3d = reactphysics3d;

PhysicsSystem* PhysicsSystem::active_ = nullptr;

namespace {

rp3d::Vector3 to_rp3d(const math::Vec3& v) { return {v.x, v.y, v.z}; }
math::Vec3 from_rp3d(const rp3d::Vector3& v) { return {v.x, v.y, v.z}; }
rp3d::Quaternion to_rp3d(const math::Quat& q) { return {q.x, q.y, q.z, q.w}; }
math::Quat from_rp3d(const rp3d::Quaternion& q) { return {q.w, q.x, q.y, q.z}; }

rp3d::BodyType to_rp3d(RigidBodyComponent::Type t) {
    switch (t) {
        case RigidBodyComponent::Type::Static: return rp3d::BodyType::STATIC;
        case RigidBodyComponent::Type::Kinematic: return rp3d::BodyType::KINEMATIC;
        case RigidBodyComponent::Type::Dynamic: return rp3d::BodyType::DYNAMIC;
    }
    return rp3d::BodyType::DYNAMIC;
}

// Raycast callback: keeps the closest hit.
class RaycastCollector : public rp3d::RaycastCallback {
public:
    rp3d::decimal notifyRaycastHit(const rp3d::RaycastInfo& info) override {
        if (!hit_.hit || info.hitFraction < hit_.fraction) {
            hit_.hit = true;
            hit_.fraction = info.hitFraction;
            hit_.body = info.body;
            hit_.point = from_rp3d(info.worldPoint);
            hit_.normal = from_rp3d(info.worldNormal);
        }
        return info.hitFraction; // continue traversal up to this fraction
    }
    struct Result {
        bool hit = false;
        rp3d::decimal fraction = 0;
        rp3d::Body* body = nullptr;
        math::Vec3 point{0.f};
        math::Vec3 normal{0.f};
    } hit_;
};

} // namespace

// Collects collision pairs into the PhysicsSystem.
class CollisionListener final : public rp3d::EventListener {
public:
    explicit CollisionListener(PhysicsSystem& sys) : sys_(sys) {}
    void onContact(const CollisionCallback::CallbackData& data) override {
        const rp3d::uint32 n = data.getNbContactPairs();
        for (rp3d::uint32 i = 0; i < n; ++i) {
            const auto pair = data.getContactPair(i);
            sys_.push_collision(pair.getBody1(), pair.getBody2());
        }
    }

private:
    PhysicsSystem& sys_;
};

void PhysicsSystem::register_components(World& world) {
    world.register_component_type<RigidBodyComponent>("RigidBody");
    world.register_component_type<ColliderComponent>("Collider");

    // Serialization traits.
    // (ComponentIO specializations live next to the components; register them here.)
}

PhysicsSystem::PhysicsSystem(Scene&) {} // scene no longer stored; world passed to update()

PhysicsSystem::~PhysicsSystem() {
    shutdown();
}

Result<void> PhysicsSystem::init(math::Vec3 gravity) {
    shutdown();
    common_ = new rp3d::PhysicsCommon();
    rp3d::PhysicsWorld::WorldSettings settings;
    settings.gravity = to_rp3d(gravity);
    world_ = common_->createPhysicsWorld(settings);
    if (!world_) {
        delete common_;
        common_ = nullptr;
        return Result<void>::failure("createPhysicsWorld failed");
    }
    listener_ = new CollisionListener(*this);
    world_->setEventListener(listener_);
    active_ = this;
    return Result<void>::success();
}

void PhysicsSystem::shutdown() {
    if (world_) {
        // Destroy all bodies first.
        for (auto& [e, body] : bodies_) {
            (void)e;
            world_->destroyRigidBody(body);
        }
        bodies_.clear();
        entities_.clear();
        common_->destroyPhysicsWorld(world_);
        world_ = nullptr;
    }
    for (auto& [shape, kind] : shapes_) {
        switch (kind) {
            case 0: common_->destroyBoxShape(static_cast<rp3d::BoxShape*>(shape)); break;
            case 1: common_->destroySphereShape(static_cast<rp3d::SphereShape*>(shape)); break;
            default: common_->destroyCapsuleShape(static_cast<rp3d::CapsuleShape*>(shape)); break;
        }
    }
    shapes_.clear();
    delete listener_;
    listener_ = nullptr;
    delete common_;
    common_ = nullptr;
    if (active_ == this) active_ = nullptr;
}

void PhysicsSystem::set_gravity(math::Vec3 g) {
    if (world_) world_->setGravity(to_rp3d(g));
}

math::Vec3 PhysicsSystem::gravity() const {
    if (!world_) return {0.f, -9.81f, 0.f};
    return from_rp3d(world_->getGravity());
}

void PhysicsSystem::sync_bodies(World& world) {
    // Remove bodies of destroyed entities or entities that lost their components.
    for (auto it = bodies_.begin(); it != bodies_.end();) {
        EntityId e = it->first;
        const bool keep = world.alive(e) && world.has<RigidBodyComponent>(e) &&
                          world.has<ColliderComponent>(e) && world.has<TransformComponent>(e);
        if (!keep) {
            world_->destroyRigidBody(it->second);
            entities_.erase(it->second);
            it = bodies_.erase(it);
        } else {
            ++it;
        }
    }

    // Create bodies for new entities; update type/mass/collider for existing.
    for (EntityId e : world.view<RigidBodyComponent, ColliderComponent, TransformComponent>()) {
        // Sanitize at the simulation boundary too: components can also be set
        // via direct C++ emplace (editor), bypassing deserialization checks.
        RigidBodyComponent rbv = *world.get<RigidBodyComponent>(e);
        ColliderComponent colv = *world.get<ColliderComponent>(e);
        TransformComponent trv = *world.get<TransformComponent>(e);
        sanitize(rbv);
        sanitize(colv);
        sanitize(trv);
        *world.get<RigidBodyComponent>(e) = rbv;   // write back so world state
        *world.get<ColliderComponent>(e) = colv;   // stays consistent with the
        *world.get<TransformComponent>(e) = trv;   // values the sim actually uses
        const RigidBodyComponent* rb = &rbv;
        const ColliderComponent* col = &colv;
        const TransformComponent* tr = &trv;

        auto it = bodies_.find(e);
        if (it == bodies_.end()) {
            rp3d::CollisionShape* shape = nullptr;
            switch (col->shape) {
                case ColliderComponent::Shape::Box:
                    shape = common_->createBoxShape(to_rp3d(col->half_extents));
                    break;
                case ColliderComponent::Shape::Sphere:
                    shape = common_->createSphereShape(col->radius);
                    break;
                case ColliderComponent::Shape::Capsule:
                    shape = common_->createCapsuleShape(col->radius, col->height);
                    break;
            }
            if (!shape) continue;
            shapes_.emplace_back(shape, static_cast<u8>(col->shape));

            rp3d::Transform t(to_rp3d(tr->position), to_rp3d(tr->rotation));
            rp3d::RigidBody* body = world_->createRigidBody(t);
            body->setType(to_rp3d(rb->type));
            if (rb->type == RigidBodyComponent::Type::Dynamic) {
                body->setMass(static_cast<rp3d::decimal>(rb->mass));
            }
            body->setLinearDamping(static_cast<rp3d::decimal>(rb->linear_damping));
            body->addCollider(shape, rp3d::Transform::identity());
            bodies_[e] = body;
            entities_[body] = e;
        } else {
            rp3d::RigidBody* body = it->second;
            body->setType(to_rp3d(rb->type));
            if (rb->type == RigidBodyComponent::Type::Dynamic) {
                body->setMass(static_cast<rp3d::decimal>(rb->mass));
            }
            // Kinematic/static bodies follow the entity transform.
            if (rb->type != RigidBodyComponent::Type::Dynamic) {
                body->setTransform(rp3d::Transform(to_rp3d(tr->position), to_rp3d(tr->rotation)));
            }
        }
    }
}

void PhysicsSystem::sync_transforms(World& world) {
    for (auto& [e, body] : bodies_) {
        auto* tr = world.get<TransformComponent>(e);
        if (!tr) continue;
        const rp3d::Transform& t = body->getTransform();
        // Dynamic bodies are driven by the simulation.
        const auto* rb = world.get<RigidBodyComponent>(e);
        if (rb && rb->type == RigidBodyComponent::Type::Dynamic) {
            tr->position = from_rp3d(t.getPosition());
            tr->rotation = from_rp3d(t.getOrientation());
        }
    }
}

void PhysicsSystem::update(World& world, f32 dt) {
    if (!world_ || dt <= 0.f) return;
    sync_bodies(world);
    world_->update(static_cast<rp3d::decimal>(dt));
    sync_transforms(world);
}

RaycastHit PhysicsSystem::raycast(math::Vec3 origin, math::Vec3 direction, f32 max_distance) const {
    RaycastHit out;
    if (!world_) return out;
    const math::Vec3 dir = glm::normalize(direction);
    const rp3d::Vector3 p1 = to_rp3d(origin);
    const rp3d::Vector3 p2 = to_rp3d(origin + dir * max_distance);
    rp3d::Ray ray(p1, p2);
    RaycastCollector collector;
    world_->raycast(ray, &collector);
    if (collector.hit_.hit) {
        out.hit = true;
        out.point = collector.hit_.point;
        out.normal = collector.hit_.normal;
        out.distance = static_cast<f32>(collector.hit_.fraction) * max_distance;
        auto it = entities_.find(collector.hit_.body);
        if (it != entities_.end()) out.entity = it->second;
    }
    return out;
}

std::vector<CollisionEvent> PhysicsSystem::take_collision_events() {
    std::vector<CollisionEvent> out = std::move(collisions_);
    collisions_.clear();
    return out;
}

void PhysicsSystem::push_collision(rp3d::Body* a, rp3d::Body* b) {
    CollisionEvent ev;
    auto ia = entities_.find(static_cast<rp3d::RigidBody*>(a));
    auto ib = entities_.find(static_cast<rp3d::RigidBody*>(b));
    if (ia != entities_.end()) ev.a = ia->second;
    if (ib != entities_.end()) ev.b = ib->second;
    if (ev.a != NULL_ENTITY && ev.b != NULL_ENTITY) collisions_.push_back(ev);
}

// --- Lua bindings --------------------------------------------------------------

namespace {

int l_physics_raycast(lua_State* L) {
    PhysicsSystem* sys = PhysicsSystem::active();
    if (!sys) { lua_pushnil(L); return 1; }
    auto read_vec = [&](int idx) {
        math::Vec3 v{0.f};
        if (lua_istable(L, idx)) {
            lua_geti(L, idx, 1); v.x = static_cast<f32>(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
            lua_geti(L, idx, 2); v.y = static_cast<f32>(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
            lua_geti(L, idx, 3); v.z = static_cast<f32>(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
        }
        return v;
    };
    const math::Vec3 origin = read_vec(1);
    const math::Vec3 dir = read_vec(2);
    const f32 max_dist = static_cast<f32>(luaL_optnumber(L, 3, 1000.0));
    const RaycastHit hit = sys->raycast(origin, dir, max_dist);
    if (!hit.hit) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushboolean(L, 1); lua_setfield(L, -2, "hit");
    lua_pushinteger(L, static_cast<lua_Integer>(hit.entity)); lua_setfield(L, -2, "entity");
    lua_pushnumber(L, static_cast<lua_Number>(hit.distance)); lua_setfield(L, -2, "distance");
    lua_createtable(L, 3, 0);
    lua_pushnumber(L, static_cast<lua_Number>(hit.point.x)); lua_rawseti(L, -2, 1);
    lua_pushnumber(L, static_cast<lua_Number>(hit.point.y)); lua_rawseti(L, -2, 2);
    lua_pushnumber(L, static_cast<lua_Number>(hit.point.z)); lua_rawseti(L, -2, 3);
    lua_setfield(L, -2, "point");
    lua_createtable(L, 3, 0);
    lua_pushnumber(L, static_cast<lua_Number>(hit.normal.x)); lua_rawseti(L, -2, 1);
    lua_pushnumber(L, static_cast<lua_Number>(hit.normal.y)); lua_rawseti(L, -2, 2);
    lua_pushnumber(L, static_cast<lua_Number>(hit.normal.z)); lua_rawseti(L, -2, 3);
    lua_setfield(L, -2, "normal");
    return 1;
}

} // namespace

void PhysicsSystem::register_lua_bindings(lua_State* L) {
    lua_getglobal(L, "ignis");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "ignis");
    }
    lua_newtable(L); // ignis.physics
    lua_pushcfunction(L, l_physics_raycast);
    lua_setfield(L, -2, "raycast");
    lua_setfield(L, -2, "physics");
    lua_pop(L, 1);
}

} // namespace ignis
