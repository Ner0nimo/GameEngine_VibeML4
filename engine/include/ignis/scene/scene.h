#pragma once

#include "ignis/core/result.h"
#include "ignis/core/types.h"
#include "ignis/ecs/entity.h"
#include "ignis/ecs/world.h"
#include "ignis/math/math.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace ignis {

// A system is any callable that advances the world, e.g. scripting, physics.
using System = std::function<void(World&, f32 dt)>;

// Resource budgets enforced while loading untrusted scene JSON. Starting
// points, not universal requirements.
struct SceneLimits {
    u32 max_entities = 100000;
    u32 max_components_per_entity = 64;
    u32 max_prefabs = 10000;
    u32 max_json_depth = 64;
    u64 max_file_bytes = 64ull << 20;   // 64 MiB serialized scene
    u64 max_string_bytes = 1ull << 20;  // 1 MiB per string field
};

// Scene-level configuration, serialized into the scene JSON under "settings"
// (absent = defaults, so old scene files keep working). Kept deliberately
// separate from per-camera settings (e.g. CameraComponent::clear_color) and
// from renderer/environment settings (IBL): ambient here is a flat light
// term, not the environment map.
struct SceneSettings {
    math::Vec3 gravity{0.f, -9.81f, 0.f};
    f32 time_scale = 1.f; // scales simulation time only, never render frame rate
    math::Vec3 ambient_color{1.f, 1.f, 1.f};
    f32 ambient_intensity = 0.f; // flat ambient off by default: existing scenes look unchanged
    math::Vec4 clear_color{0.02f, 0.02f, 0.03f, 1.f}; // scene default
};

// A scene owns a world, named prefabs and the system pipeline.
// Scene files are JSON (see scenes/test_scene.scene.json).
class Scene {
public:
    explicit Scene(std::string name = "Untitled");

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) noexcept = default;
    Scene& operator=(Scene&&) noexcept = default;

    World& world() noexcept { return world_; }
    const World& world() const noexcept { return world_; }

    const std::string& name() const noexcept { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    SceneSettings& settings() noexcept { return settings_; }
    const SceneSettings& settings() const noexcept { return settings_; }

    // Systems run in registration order on every update().
    void add_system(System system) { systems_.push_back(std::move(system)); }
    void update(f32 dt) {
        for (auto& s : systems_) s(world_, dt);
    }

    // --- prefabs ----------------------------------------------------------
    // A prefab is a named entity template stored as JSON (same shape as a
    // serialized entity, minus the id).
    void add_prefab(std::string name, nlohmann::json entity_template);
    bool has_prefab(const std::string& name) const { return prefabs_.contains(name); }
    std::vector<std::string> prefab_names() const;
    EntityId spawn_prefab(const std::string& name);

    // --- serialization ----------------------------------------------------
    nlohmann::json serialize() const;
    // Throws std::runtime_error when the JSON violates the given limits or is
    // structurally malformed; callers at input boundaries must catch.
    static Scene deserialize(const nlohmann::json& j, const SceneLimits& limits = {});
    Result<void> save_to_file(const std::string& path) const;
    static Result<Scene> load_from_file(const std::string& path, const SceneLimits& limits = {});
    // Load a scene file INTO this scene: replaces this scene's world contents,
    // prefabs and name, but keeps systems added via add_system. Components
    // must be registered beforehand (e.g. register_builtin_components +
    // physics/audio), since the registry is process-global.
    Result<void> load_file(const std::string& path, const SceneLimits& limits = {});
    // Same contract as load_file, but for already-parsed JSON (used by the
    // editor to restore an in-memory snapshot without touching the file).
    // Fails if the JSON is structurally malformed (not a scene object).
    Result<void> load_json(const nlohmann::json& j, const SceneLimits& limits = {});

private:
    World world_;
    std::string name_;
    SceneSettings settings_;
    std::vector<System> systems_;
    std::unordered_map<std::string, nlohmann::json> prefabs_;
};

} // namespace ignis
