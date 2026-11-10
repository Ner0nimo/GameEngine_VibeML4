#pragma once

#include "ignis/core/result.h"
#include "ignis/core/types.h"
#include "ignis/ecs/entity.h"
#include "ignis/ecs/world.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace ignis {

// A system is any callable that advances the world, e.g. scripting, physics.
using System = std::function<void(World&, f32 dt)>;

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
    static Scene deserialize(const nlohmann::json& j);
    Result<void> save_to_file(const std::string& path) const;
    static Result<Scene> load_from_file(const std::string& path);
    // Load a scene file INTO this scene: replaces this scene's world contents,
    // prefabs and name, but keeps systems added via add_system. Components
    // must be registered beforehand (e.g. register_builtin_components +
    // physics/audio), since the registry is process-global.
    Result<void> load_file(const std::string& path);

private:
    World world_;
    std::string name_;
    std::vector<System> systems_;
    std::unordered_map<std::string, nlohmann::json> prefabs_;
};

} // namespace ignis
