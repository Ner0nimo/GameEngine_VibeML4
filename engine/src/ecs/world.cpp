#include "ignis/ecs/world.h"

#include "ignis/ecs/component_registry.h"

namespace ignis {

const std::vector<EntityId> World::empty_children_{};

World::World() = default;
World::~World() = default;

EntityId World::create_entity(std::string name) {
    EntityId id = 0;
    if (!free_ids_.empty()) {
        id = free_ids_.back();
        free_ids_.pop_back();
    } else {
        id = next_id_++;
    }
    alive_.insert(id);
    names_[id] = std::move(name);
    return id;
}

EntityId World::create_entity_reserved(EntityId id, std::string name) {
    if (id == NULL_ENTITY) id = next_id_;
    if (id >= next_id_) next_id_ = id + 1;
    // Remove from free list if present.
    for (auto it = free_ids_.begin(); it != free_ids_.end(); ++it) {
        if (*it == id) { free_ids_.erase(it); break; }
    }
    alive_.insert(id);
    names_[id] = std::move(name);
    return id;
}

void World::destroy_entity(EntityId e) {
    if (!alive_.contains(e)) return;

    for (auto& [_, storage] : storages_) {
        storage->remove(e);
    }

    // Detach from parent, re-attach children to our parent (or root).
    EntityId p = parent_of(e);
    auto cit = children_.find(e);
    if (cit != children_.end()) {
        for (EntityId child : cit->second) {
            parent_[child] = p;
            if (p != NULL_ENTITY) children_[p].push_back(child);
        }
        children_.erase(cit);
    }
    if (p != NULL_ENTITY) {
        auto& siblings = children_[p];
        for (auto it = siblings.begin(); it != siblings.end(); ++it) {
            if (*it == e) { siblings.erase(it); break; }
        }
    }
    parent_.erase(e);
    names_.erase(e);

    alive_.erase(e);
    free_ids_.push_back(e);
}

const std::string& World::name_of(EntityId e) const noexcept {
    static const std::string empty;
    auto it = names_.find(e);
    return it != names_.end() ? it->second : empty;
}

void World::set_name(EntityId e, std::string name) {
    if (alive_.contains(e)) names_[e] = std::move(name);
}

bool World::set_parent(EntityId child, EntityId parent) {
    if (!alive_.contains(child)) return false;
    if (parent != NULL_ENTITY && !alive_.contains(parent)) return false;
    // Reject cycles: parent must not be the child itself or one of its
    // descendants. The hierarchy stays acyclic by construction.
    for (EntityId cur = parent; cur != NULL_ENTITY; cur = parent_of(cur)) {
        if (cur == child) return false;
    }
    EntityId old = parent_of(child);
    if (old == parent) return true;
    if (old != NULL_ENTITY) {
        auto& siblings = children_[old];
        for (auto it = siblings.begin(); it != siblings.end(); ++it) {
            if (*it == child) { siblings.erase(it); break; }
        }
    }
    parent_[child] = parent;
    if (parent != NULL_ENTITY) children_[parent].push_back(child);
    return true;
}

EntityId World::parent_of(EntityId e) const noexcept {
    auto it = parent_.find(e);
    return it != parent_.end() ? it->second : NULL_ENTITY;
}

const std::vector<EntityId>& World::children_of(EntityId e) const noexcept {
    auto it = children_.find(e);
    return it != children_.end() ? it->second : empty_children_;
}

std::vector<EntityId> World::all_entities() const {
    std::vector<EntityId> out;
    out.reserve(alive_.size());
    for (EntityId e : alive_) out.push_back(e);
    return out;
}

} // namespace ignis
