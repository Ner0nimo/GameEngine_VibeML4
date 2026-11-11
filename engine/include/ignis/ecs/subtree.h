#pragma once

#include "ignis/core/types.h"
#include "ignis/ecs/entity.h"

#include <nlohmann/json.hpp>

#include <unordered_map>

namespace ignis {

class World;

// Serialize an entity and its whole subtree (children recursively) to JSON:
// { "id", "name", "components": {...}, "children": [...] }.
nlohmann::json serialize_subtree(const World& w, EntityId root);

// Spawn a subtree from JSON produced by serialize_subtree (or a prefab
// template with "children"). Two phases: create all entities with fresh ids
// while recording an old->new id map, then deserialize components (remapping
// any integer component field named "entity" through the map) and link the
// hierarchy. `parent` = NULL_ENTITY spawns at root. Returns the new root id,
// or NULL_ENTITY on malformed input (the world is left unchanged).
EntityId spawn_subtree(World& w, const nlohmann::json& j, EntityId parent = NULL_ENTITY);

// Recursively rewrite integer fields named "entity" in component JSON through
// the old->new id map (entity-reference convention shared with the agent
// command API). Unknown old ids are left untouched.
void remap_entity_refs(nlohmann::json& j, const std::unordered_map<EntityId, EntityId>& id_map);

} // namespace ignis
