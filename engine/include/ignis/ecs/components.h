#pragma once

#include "ignis/core/types.h"
#include "ignis/ecs/component_registry.h"
#include "ignis/ecs/world.h"
#include "ignis/math/math.h"

#include <glm/gtc/quaternion.hpp>

#include <string>

namespace ignis {

// --- Built-in components ---------------------------------------------------

struct TransformComponent {
    math::Vec3 position{0.f, 0.f, 0.f};
    math::Quat rotation{1.f, 0.f, 0.f, 0.f}; // identity, (w, x, y, z)
    math::Vec3 scale{1.f, 1.f, 1.f};

    math::Mat4 local_matrix() const {
        return glm::translate(math::Mat4(1.f), position) *
               glm::mat4_cast(rotation) *
               glm::scale(math::Mat4(1.f), scale);
    }
};

// World-space transform, computed by walking the parent chain.
inline math::Mat4 world_matrix(const World& world, EntityId e) {
    math::Mat4 m(1.f);
    EntityId cur = e;
    // Guard against cycles (should never happen, but a corrupt scene file
    // must not hang the engine).
    for (int guard = 0; guard < 1024 && cur != NULL_ENTITY; ++guard) {
        if (const auto* t = world.get<TransformComponent>(cur)) {
            m = t->local_matrix() * m;
        }
        cur = world.parent_of(cur);
    }
    return m;
}

struct ScriptComponent {
    std::string path;      // Lua file, relative to the scene/pak root
    bool enabled = true;
};

struct CameraComponent {
    f32 fov_y_deg = 60.f;
    f32 z_near = 0.1f;
    f32 z_far = 1000.f;
    math::Vec4 clear_color{0.02f, 0.02f, 0.03f, 1.f};
    bool active = true;
};

struct DirectionalLightComponent {
    math::Vec3 direction{0.f, -1.f, 0.f};
    math::Vec3 color{1.f, 1.f, 1.f};
    f32 intensity = 3.f;
    bool cast_shadow = true;
};

struct PointLightComponent {
    math::Vec3 color{1.f, 1.f, 1.f};
    f32 intensity = 10.f;
    f32 range = 20.f;
};

struct MeshRendererComponent {
    std::string model_path; // glTF asset
    // Material overrides by material slot (index -> partial material JSON
    // is applied at load time by the renderer).
};

// --- Serialization traits --------------------------------------------------

inline void to_json(nlohmann::json& j, const math::Vec3& v) { j = {v.x, v.y, v.z}; }
inline void from_json(const nlohmann::json& j, math::Vec3& v) {
    v.x = j.at(0).get<f32>(); v.y = j.at(1).get<f32>(); v.z = j.at(2).get<f32>();
}
inline void to_json(nlohmann::json& j, const math::Vec4& v) { j = {v.x, v.y, v.z, v.w}; }
inline void from_json(const nlohmann::json& j, math::Vec4& v) {
    v.x = j.at(0).get<f32>(); v.y = j.at(1).get<f32>();
    v.z = j.at(2).get<f32>(); v.w = j.at(3).get<f32>();
}
inline void to_json(nlohmann::json& j, const math::Quat& q) { j = {q.w, q.x, q.y, q.z}; }
inline void from_json(const nlohmann::json& j, math::Quat& q) {
    q.w = j.at(0).get<f32>(); q.x = j.at(1).get<f32>();
    q.y = j.at(2).get<f32>(); q.z = j.at(3).get<f32>();
}

template <>
struct ComponentIO<TransformComponent> {
    static nlohmann::json to_json(const TransformComponent& t) {
        nlohmann::json j;
        ::ignis::to_json(j["position"], t.position);
        ::ignis::to_json(j["rotation"], t.rotation);
        ::ignis::to_json(j["scale"], t.scale);
        return j;
    }
    static TransformComponent from_json(const nlohmann::json& j) {
        TransformComponent t;
        if (j.contains("position")) ::ignis::from_json(j["position"], t.position);
        if (j.contains("rotation")) ::ignis::from_json(j["rotation"], t.rotation);
        if (j.contains("scale")) ::ignis::from_json(j["scale"], t.scale);
        return t;
    }
};

template <>
struct ComponentIO<ScriptComponent> {
    static nlohmann::json to_json(const ScriptComponent& s) {
        return {{"path", s.path}, {"enabled", s.enabled}};
    }
    static ScriptComponent from_json(const nlohmann::json& j) {
        ScriptComponent s;
        if (j.contains("path")) s.path = j["path"].get<std::string>();
        if (j.contains("enabled")) s.enabled = j["enabled"].get<bool>();
        return s;
    }
};

template <>
struct ComponentIO<CameraComponent> {
    static nlohmann::json to_json(const CameraComponent& c) {
        nlohmann::json j;
        j["fov_y_deg"] = c.fov_y_deg;
        j["z_near"] = c.z_near;
        j["z_far"] = c.z_far;
        ::ignis::to_json(j["clear_color"], c.clear_color);
        j["active"] = c.active;
        return j;
    }
    static CameraComponent from_json(const nlohmann::json& j) {
        CameraComponent c;
        if (j.contains("fov_y_deg")) c.fov_y_deg = j["fov_y_deg"].get<f32>();
        if (j.contains("z_near")) c.z_near = j["z_near"].get<f32>();
        if (j.contains("z_far")) c.z_far = j["z_far"].get<f32>();
        if (j.contains("clear_color")) ::ignis::from_json(j["clear_color"], c.clear_color);
        if (j.contains("active")) c.active = j["active"].get<bool>();
        return c;
    }
};

template <>
struct ComponentIO<DirectionalLightComponent> {
    static nlohmann::json to_json(const DirectionalLightComponent& l) {
        nlohmann::json j;
        ::ignis::to_json(j["direction"], l.direction);
        ::ignis::to_json(j["color"], l.color);
        j["intensity"] = l.intensity;
        j["cast_shadow"] = l.cast_shadow;
        return j;
    }
    static DirectionalLightComponent from_json(const nlohmann::json& j) {
        DirectionalLightComponent l;
        if (j.contains("direction")) ::ignis::from_json(j["direction"], l.direction);
        if (j.contains("color")) ::ignis::from_json(j["color"], l.color);
        if (j.contains("intensity")) l.intensity = j["intensity"].get<f32>();
        if (j.contains("cast_shadow")) l.cast_shadow = j["cast_shadow"].get<bool>();
        return l;
    }
};

template <>
struct ComponentIO<PointLightComponent> {
    static nlohmann::json to_json(const PointLightComponent& l) {
        nlohmann::json j;
        ::ignis::to_json(j["color"], l.color);
        j["intensity"] = l.intensity;
        j["range"] = l.range;
        return j;
    }
    static PointLightComponent from_json(const nlohmann::json& j) {
        PointLightComponent l;
        if (j.contains("color")) ::ignis::from_json(j["color"], l.color);
        if (j.contains("intensity")) l.intensity = j["intensity"].get<f32>();
        if (j.contains("range")) l.range = j["range"].get<f32>();
        return l;
    }
};

template <>
struct ComponentIO<MeshRendererComponent> {
    static nlohmann::json to_json(const MeshRendererComponent& m) {
        return {{"model_path", m.model_path}};
    }
    static MeshRendererComponent from_json(const nlohmann::json& j) {
        MeshRendererComponent m;
        if (j.contains("model_path")) m.model_path = j["model_path"].get<std::string>();
        return m;
    }
};

// Register all built-in component types on a world.
inline void register_builtin_components(World& world) {
    world.register_component_type<TransformComponent>("Transform");
    world.register_component_type<ScriptComponent>("Script");
    world.register_component_type<CameraComponent>("Camera");
    world.register_component_type<DirectionalLightComponent>("DirectionalLight");
    world.register_component_type<PointLightComponent>("PointLight");
    world.register_component_type<MeshRendererComponent>("MeshRenderer");
}

} // namespace ignis
