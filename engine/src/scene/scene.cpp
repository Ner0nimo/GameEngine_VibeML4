#include "ignis/scene/scene.h"

#include "ignis/core/json_parse.h"
#include "ignis/core/log.h"
#include "ignis/ecs/component_registry.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace ignis {

namespace {

void check_string_len(const std::string& s, const SceneLimits& limits, const char* what) {
    if (s.size() > limits.max_string_bytes)
        throw std::runtime_error(std::string("scene: ") + what + " too long");
}

// Tolerant readers for the optional "settings" object: malformed fields fall
// back to defaults instead of failing the whole scene load.
f32 settings_f32(const nlohmann::json& j, const char* key, f32 fallback) {
    return (j.contains(key) && j[key].is_number()) ? j[key].get<f32>() : fallback;
}

math::Vec3 settings_vec3(const nlohmann::json& j, const char* key, math::Vec3 fallback) {
    if (j.contains(key) && j[key].is_array() && j[key].size() == 3 &&
        j[key][0].is_number() && j[key][1].is_number() && j[key][2].is_number()) {
        return {j[key][0].get<f32>(), j[key][1].get<f32>(), j[key][2].get<f32>()};
    }
    return fallback;
}

math::Vec4 settings_vec4(const nlohmann::json& j, const char* key, math::Vec4 fallback) {
    if (j.contains(key) && j[key].is_array() && j[key].size() == 4 &&
        j[key][0].is_number() && j[key][1].is_number() && j[key][2].is_number() &&
        j[key][3].is_number()) {
        return {j[key][0].get<f32>(), j[key][1].get<f32>(), j[key][2].get<f32>(), j[key][3].get<f32>()};
    }
    return fallback;
}

} // namespace

Scene::Scene(std::string name) : name_(std::move(name)) {}

void Scene::add_prefab(std::string name, nlohmann::json entity_template) {
    entity_template.erase("id");
    prefabs_[std::move(name)] = std::move(entity_template);
}

std::vector<std::string> Scene::prefab_names() const {
    std::vector<std::string> out;
    out.reserve(prefabs_.size());
    for (const auto& [n, _] : prefabs_) out.push_back(n);
    return out;
}

EntityId Scene::spawn_prefab(const std::string& name) {
    auto it = prefabs_.find(name);
    if (it == prefabs_.end()) {
        IGNIS_WARN("spawn_prefab: unknown prefab '{}'", name);
        return NULL_ENTITY;
    }
    const nlohmann::json& tpl = it->second;
    EntityId e = world_.create_entity(tpl.value("name", name));
    if (tpl.contains("components") && tpl["components"].is_object()) {
        for (auto& [comp_name, comp_json] : tpl["components"].items()) {
            const ComponentTypeInfo* info = world_.registry().find_by_name(comp_name);
            if (!info) {
                IGNIS_WARN("spawn_prefab: unknown component '{}'", comp_name);
                continue;
            }
            if (!info->deserialize(world_, e, comp_json)) {
                IGNIS_WARN("spawn_prefab: bad data for component '{}'", comp_name);
            }
        }
    }
    return e;
}

nlohmann::json Scene::serialize() const {
    nlohmann::json j;
    j["name"] = name_;
    j["entities"] = nlohmann::json::array();

    std::vector<EntityId> entities = world_.all_entities();
    std::sort(entities.begin(), entities.end());

    for (EntityId e : entities) {
        nlohmann::json ej;
        ej["id"] = e;
        ej["name"] = world_.name_of(e);
        EntityId p = world_.parent_of(e);
        ej["parent"] = p == NULL_ENTITY ? 0 : p;
        ej["components"] = nlohmann::json::object();
        for (const auto& [type, storage] : world_.storages()) {
            if (!storage->has(e)) continue;
            const ComponentTypeInfo* info = world_.registry().find_by_type(type);
            if (!info) continue; // not registered -> not serializable
            ej["components"][info->name] = info->serialize(world_, e);
        }
        j["entities"].push_back(std::move(ej));
    }

    j["prefabs"] = nlohmann::json::object();
    for (const auto& [n, tpl] : prefabs_) j["prefabs"][n] = tpl;

    nlohmann::json sj;
    sj["gravity"] = {settings_.gravity.x, settings_.gravity.y, settings_.gravity.z};
    sj["time_scale"] = settings_.time_scale;
    sj["ambient_color"] = {settings_.ambient_color.x, settings_.ambient_color.y, settings_.ambient_color.z};
    sj["ambient_intensity"] = settings_.ambient_intensity;
    sj["clear_color"] = {settings_.clear_color.x, settings_.clear_color.y, settings_.clear_color.z,
                         settings_.clear_color.w};
    j["settings"] = std::move(sj);

    return j;
}

Scene Scene::deserialize(const nlohmann::json& j, const SceneLimits& limits) {
    if (!j.is_object())
        throw std::runtime_error("scene JSON is not an object");
    Scene scene(j.value("name", "Untitled"));
    check_string_len(scene.name(), limits, "scene name");

    if (j.contains("settings") && j["settings"].is_object()) {
        const nlohmann::json& s = j["settings"];
        SceneSettings def;
        scene.settings_.gravity = settings_vec3(s, "gravity", def.gravity);
        // Clamp to the same ranges the editor widgets enforce, so a hand-edited
        // scene file can't produce a negative time scale or negative light.
        scene.settings_.time_scale = std::clamp(settings_f32(s, "time_scale", def.time_scale), 0.f, 4.f);
        scene.settings_.ambient_color = settings_vec3(s, "ambient_color", def.ambient_color);
        scene.settings_.ambient_intensity =
            std::clamp(settings_f32(s, "ambient_intensity", def.ambient_intensity), 0.f, 10.f);
        scene.settings_.clear_color = settings_vec4(s, "clear_color", def.clear_color);
    }

    if (j.contains("prefabs") && j["prefabs"].is_object()) {
        if (j["prefabs"].size() > limits.max_prefabs)
            throw std::runtime_error("scene: too many prefabs");
        for (auto& [n, tpl] : j["prefabs"].items()) {
            check_string_len(n, limits, "prefab name");
            scene.add_prefab(n, tpl);
        }
    }

    if (j.contains("entities") && j["entities"].is_array()) {
        if (j["entities"].size() > limits.max_entities)
            throw std::runtime_error("scene: too many entities");
        std::unordered_set<EntityId> seen_ids;
        // First pass: create entities with their ids so parent links resolve.
        for (const auto& ej : j["entities"]) {
            if (!ej.is_object() || !ej.contains("id") || !ej["id"].is_number())
                throw std::runtime_error("scene: entity entry missing numeric 'id'");
            const EntityId id = ej["id"].get<EntityId>();
            if (!seen_ids.insert(id).second)
                throw std::runtime_error("scene: duplicate entity id " + std::to_string(id));
            const std::string name = ej.value("name", "");
            check_string_len(name, limits, "entity name");
            scene.world_.create_entity_reserved(id, name);
        }
        // Second pass: components, then hierarchy.
        for (const auto& ej : j["entities"]) {
            const EntityId id = ej["id"].get<EntityId>();
            if (!scene.world_.alive(id)) continue;
            if (ej.contains("components") && ej["components"].is_object()) {
                if (ej["components"].size() > limits.max_components_per_entity)
                    throw std::runtime_error("scene: too many components on entity " +
                                             std::to_string(id));
                for (auto& [comp_name, comp_json] : ej["components"].items()) {
                    const ComponentTypeInfo* info = scene.world_.registry().find_by_name(comp_name);
                    if (!info) {
                        IGNIS_WARN("deserialize: unknown component '{}'", comp_name);
                        continue;
                    }
                    info->deserialize(scene.world_, id, comp_json);
                }
            }
        }
        for (const auto& ej : j["entities"]) {
            const EntityId id = ej["id"].get<EntityId>();
            const EntityId parent = ej.value("parent", 0u);
            if (parent != 0 && scene.world_.alive(id) &&
                !scene.world_.set_parent(id, parent)) {
                IGNIS_WARN("deserialize: entity {} has invalid parent {} (dead or cyclic)", id, parent);
            }
        }
    }
    return scene;
}

Result<void> Scene::save_to_file(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return Result<void>::failure("cannot open '" + path + "' for writing");
    f << serialize().dump(2);
    if (!f.good()) return Result<void>::failure("write failed for '" + path + "'");
    return Result<void>::success();
}

Result<Scene> Scene::load_from_file(const std::string& path, const SceneLimits& limits) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return Result<Scene>::failure("cannot open scene '" + path + "'");
    const auto end = f.tellg();
    if (end < 0) return Result<Scene>::failure("cannot stat scene '" + path + "'");
    if (static_cast<u64>(end) > limits.max_file_bytes)
        return Result<Scene>::failure("scene file too large '" + path + "'");
    f.seekg(0, std::ios::beg);
    std::stringstream ss;
    ss << f.rdbuf();
    auto parsed = parse_json_bounded(ss.str(), static_cast<int>(limits.max_json_depth));
    if (!parsed.ok())
        return Result<Scene>::failure("cannot parse scene '" + path + "': " + parsed.error());
    try {
        return Result<Scene>::success(deserialize(parsed.value(), limits));
    } catch (const std::exception& e) {
        return Result<Scene>::failure("cannot parse scene '" + path + "': " + e.what());
    }
}

Result<void> Scene::load_json(const nlohmann::json& j, const SceneLimits& limits) {
    try {
        // Load INTO this scene: swap in the parsed world/prefabs/name, keep
        // the system pipeline (and anything else registered on this object).
        Scene tmp = deserialize(j, limits);
        world_ = std::move(tmp.world_);
        prefabs_ = std::move(tmp.prefabs_);
        name_ = std::move(tmp.name_);
        settings_ = tmp.settings_;
        return Result<void>::success();
    } catch (const std::exception& e) {
        return Result<void>::failure(std::string("malformed scene JSON: ") + e.what());
    }
}

Result<void> Scene::load_file(const std::string& path, const SceneLimits& limits) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return Result<void>::failure("cannot open scene '" + path + "'");
    const auto end = f.tellg();
    if (end < 0) return Result<void>::failure("cannot stat scene '" + path + "'");
    if (static_cast<u64>(end) > limits.max_file_bytes)
        return Result<void>::failure("scene file too large '" + path + "'");
    f.seekg(0, std::ios::beg);
    std::stringstream ss;
    ss << f.rdbuf();
    auto parsed = parse_json_bounded(ss.str(), static_cast<int>(limits.max_json_depth));
    if (!parsed.ok())
        return Result<void>::failure("cannot parse scene '" + path + "': " + parsed.error());
    return load_json(parsed.value(), limits);
}

} // namespace ignis
