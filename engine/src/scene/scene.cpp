#include "ignis/scene/scene.h"

#include "ignis/core/log.h"
#include "ignis/ecs/component_registry.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace ignis {

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
            info->deserialize(world_, e, comp_json);
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

    return j;
}

Scene Scene::deserialize(const nlohmann::json& j) {
    Scene scene(j.value("name", "Untitled"));

    if (j.contains("prefabs") && j["prefabs"].is_object()) {
        for (auto& [n, tpl] : j["prefabs"].items()) {
            scene.add_prefab(n, tpl);
        }
    }

    if (j.contains("entities") && j["entities"].is_array()) {
        // First pass: create entities with their ids so parent links resolve.
        for (const auto& ej : j["entities"]) {
            EntityId id = ej.value("id", 0u);
            scene.world_.create_entity_reserved(id, ej.value("name", ""));
        }
        // Second pass: components, then hierarchy.
        for (const auto& ej : j["entities"]) {
            EntityId id = ej.value("id", 0u);
            if (!scene.world_.alive(id)) continue;
            if (ej.contains("components") && ej["components"].is_object()) {
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
            EntityId id = ej.value("id", 0u);
            EntityId parent = ej.value("parent", 0u);
            if (parent != 0 && scene.world_.alive(id)) {
                scene.world_.set_parent(id, parent);
            }
        }
    }
    return scene;
}

bool Scene::save_to_file(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    f << serialize().dump(2);
    return f.good();
}

Scene Scene::load_from_file(const std::string& path, bool& ok) {
    std::ifstream f(path);
    if (!f) {
        ok = false;
        return Scene{};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        Scene scene = deserialize(nlohmann::json::parse(ss.str()));
        ok = true;
        return scene;
    } catch (const std::exception& e) {
        IGNIS_ERROR("Failed to parse scene '{}': {}", path, e.what());
        ok = false;
        return Scene{};
    }
}

bool Scene::load_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        IGNIS_ERROR("cannot open scene '{}'", path);
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        *this = deserialize(nlohmann::json::parse(ss.str()));
        return true;
    } catch (const std::exception& e) {
        IGNIS_ERROR("Failed to parse scene '{}': {}", path, e.what());
        return false;
    }
}

} // namespace ignis
