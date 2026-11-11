#include "ignis/ecs/subtree.h"

#include "ignis/core/log.h"
#include "ignis/ecs/component_registry.h"
#include "ignis/ecs/world.h"

#include <functional>

namespace ignis {

namespace {

nlohmann::json serialize_entity(const World& w, EntityId e) {
    nlohmann::json j;
    j["id"] = e;
    j["name"] = w.name_of(e);
    j["components"] = nlohmann::json::object();
    for (const auto& [type, storage] : w.storages()) {
        if (!storage->has(e)) continue;
        const ComponentTypeInfo* info = w.registry().find_by_type(type);
        if (!info) continue;
        j["components"][info->name] = info->serialize(w, e);
    }
    j["children"] = nlohmann::json::array();
    for (EntityId c : w.children_of(e)) j["children"].push_back(serialize_entity(w, c));
    return j;
}

// Structural validity of a subtree node: an object with a string "name",
// and recursively valid "children". Validated up front in spawn_subtree so a
// malformed tree never leaves orphaned entities behind.
bool valid_subtree_node(const nlohmann::json& n) {
    if (!n.is_object()) return false;
    if (!n.contains("name") || !n["name"].is_string()) return false;
    if (n.contains("children")) {
        if (!n["children"].is_array()) return false;
        for (const auto& c : n["children"])
            if (!valid_subtree_node(c)) return false;
    }
    return true;
}

} // namespace

nlohmann::json serialize_subtree(const World& w, EntityId root) {
    if (!w.alive(root)) return nullptr;
    return serialize_entity(w, root);
}

void remap_entity_refs(nlohmann::json& j, const std::unordered_map<EntityId, EntityId>& id_map) {
    if (j.is_object()) {
        for (auto& [key, val] : j.items()) {
            if (key == "entity" && val.is_number_unsigned()) {
                const EntityId old_id = val.get<EntityId>();
                const auto it = id_map.find(old_id);
                if (it != id_map.end()) val = it->second;
            } else {
                remap_entity_refs(val, id_map);
            }
        }
    } else if (j.is_array()) {
        for (auto& el : j) remap_entity_refs(el, id_map);
    }
}

EntityId spawn_subtree(World& w, const nlohmann::json& j, EntityId parent) {
    // Validate the whole tree BEFORE creating anything, so malformed input
    // really does leave the world unchanged (no orphaned entities).
    if (!valid_subtree_node(j)) return NULL_ENTITY;
    if (parent != NULL_ENTITY && !w.alive(parent)) return NULL_ENTITY;

    // Phase 1: create every entity with a fresh id. Nodes are keyed by pointer
    // (stable: they live in the caller's JSON), real "id" values additionally
    // go into the old->new map used for entity-reference remapping.
    std::unordered_map<const nlohmann::json*, EntityId> by_node;
    std::unordered_map<EntityId, EntityId> id_map;
    std::function<void(const nlohmann::json&)> create = [&](const nlohmann::json& node) {
        if (!node.is_object()) return;
        const EntityId e = w.create_entity(node.value("name", "Entity"));
        by_node[&node] = e;
        if (node.contains("id") && node["id"].is_number_unsigned())
            id_map[node["id"].get<EntityId>()] = e;
        if (node.contains("children") && node["children"].is_array())
            for (const auto& c : node["children"]) create(c);
    };
    create(j);
    const EntityId root = by_node[&j];

    // Phase 2: deserialize components (entity refs remapped), then link the
    // hierarchy to the new parent.
    std::function<void(const nlohmann::json&, EntityId)> link = [&](const nlohmann::json& node,
                                                                     EntityId new_parent) {
        const EntityId e = by_node[&node];
        if (node.contains("components") && node["components"].is_object()) {
            for (auto& [comp_name, comp_json] : node["components"].items()) {
                const ComponentTypeInfo* info = w.registry().find_by_name(comp_name);
                if (!info) {
                    IGNIS_WARN("spawn_subtree: unknown component '{}'", comp_name);
                    continue;
                }
                nlohmann::json remapped = comp_json;
                remap_entity_refs(remapped, id_map);
                if (!info->deserialize(w, e, remapped))
                    IGNIS_WARN("spawn_subtree: bad data for component '{}'", comp_name);
            }
        }
        if (new_parent != NULL_ENTITY) w.set_parent(e, new_parent);
        if (node.contains("children") && node["children"].is_array())
            for (const auto& c : node["children"]) link(c, e);
    };
    link(j, parent);
    return root;
}

} // namespace ignis
