#pragma once

#include "ignis/core/types.h"
#include "ignis/ecs/entity.h"

#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ignis {

class ComponentRegistry;

template <typename... Ts>
class View;

// Type-erased component storage. Concrete storage is ComponentStorage<T>.
class IComponentStorage {
public:
    virtual ~IComponentStorage() = default;
    virtual std::type_index type() const noexcept = 0;
    virtual bool has(EntityId e) const = 0;
    virtual void remove(EntityId e) = 0;
    virtual void clear() = 0;
    virtual std::vector<EntityId> entities() const = 0;
    virtual void copy(EntityId from, EntityId to) = 0;
};

template <typename T>
class ComponentStorage final : public IComponentStorage {
public:
    std::type_index type() const noexcept override { return std::type_index(typeid(T)); }
    bool has(EntityId e) const override { return data_.contains(e); }
    void remove(EntityId e) override { data_.erase(e); }
    void clear() override { data_.clear(); }
    std::vector<EntityId> entities() const override {
        std::vector<EntityId> out;
        out.reserve(data_.size());
        for (const auto& [e, _] : data_) out.push_back(e);
        return out;
    }
    void copy(EntityId from, EntityId to) override {
        auto it = data_.find(from);
        if (it != data_.end()) data_[to] = it->second;
    }

    std::unordered_map<EntityId, T> data_;
};

// The ECS world: owns entities, their names/hierarchy and all component storage.
class World {
public:
    World();
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) noexcept = default;
    World& operator=(World&&) noexcept = default;

    // --- entities ---------------------------------------------------------
    EntityId create_entity(std::string name = "");
    EntityId create_entity_reserved(EntityId id, std::string name = ""); // scene loading
    void destroy_entity(EntityId e);
    bool alive(EntityId e) const noexcept { return alive_.contains(e); }
    u64 entity_count() const noexcept { return alive_.size(); }

    const std::string& name_of(EntityId e) const noexcept;
    void set_name(EntityId e, std::string name);

    // --- hierarchy --------------------------------------------------------
    // Sets child's parent (NULL_ENTITY = root). Returns false and changes
    // nothing if child/parent is not alive or if parent is child's own
    // descendant (which would create a cycle) — the hierarchy invariant is
    // "acyclic", enforced here at write time so readers never have to guard.
    bool set_parent(EntityId child, EntityId parent);
    EntityId parent_of(EntityId e) const noexcept;
    const std::vector<EntityId>& children_of(EntityId e) const noexcept;

    // --- components -------------------------------------------------------
    template <typename T>
    void register_component_type(const std::string& name);

    template <typename T, typename... Args>
    T& emplace(EntityId e, Args&&... args) {
        auto& storage = storage_for<T>();
        // Assign (not map::emplace) so re-adding a component overwrites it.
        storage.data_[e] = T(std::forward<Args>(args)...);
        return storage.data_[e];
    }

    template <typename T>
    T* get(EntityId e) {
        auto* s = find_storage<T>();
        if (!s) return nullptr;
        auto it = s->data_.find(e);
        return it != s->data_.end() ? &it->second : nullptr;
    }

    template <typename T>
    const T* get(EntityId e) const {
        auto* s = find_storage<T>();
        if (!s) return nullptr;
        auto it = s->data_.find(e);
        return it != s->data_.end() ? &it->second : nullptr;
    }

    template <typename T>
    bool has(EntityId e) const {
        auto* s = find_storage<T>();
        return s && s->data_.contains(e);
    }

    template <typename... Ts>
    bool has_all(EntityId e) const { return (has<Ts>(e) && ...); }

    template <typename T>
    void remove(EntityId e) {
        if (auto* s = find_storage<T>()) s->data_.erase(e);
    }

    template <typename... Ts>
    View<Ts...> view() const; // defined below, after View

    // --- introspection (serialization / scripting) ------------------------
    // Defined at the bottom of this header, after ComponentRegistry is complete.
    ComponentRegistry& registry() noexcept;
    const ComponentRegistry& registry() const noexcept;

    const std::unordered_map<std::type_index, std::unique_ptr<IComponentStorage>>&
    storages() const noexcept { return storages_; }

    std::vector<EntityId> all_entities() const;

private:
    template <typename T>
    ComponentStorage<T>* find_storage() {
        auto it = storages_.find(std::type_index(typeid(T)));
        return it != storages_.end() ? static_cast<ComponentStorage<T>*>(it->second.get()) : nullptr;
    }
    template <typename T>
    const ComponentStorage<T>* find_storage() const {
        auto it = storages_.find(std::type_index(typeid(T)));
        return it != storages_.end() ? static_cast<const ComponentStorage<T>*>(it->second.get()) : nullptr;
    }
    template <typename T>
    ComponentStorage<T>& storage_for() {
        auto key = std::type_index(typeid(T));
        auto it = storages_.find(key);
        if (it == storages_.end()) {
            auto s = std::make_unique<ComponentStorage<T>>();
            auto* raw = s.get();
            storages_.emplace(key, std::move(s));
            return *raw;
        }
        return *static_cast<ComponentStorage<T>*>(it->second.get());
    }

    EntityId next_id_ = 1;
    std::vector<EntityId> free_ids_;
    std::unordered_set<EntityId> alive_;

    std::unordered_map<EntityId, std::string> names_;
    std::unordered_map<EntityId, EntityId> parent_;
    std::unordered_map<EntityId, std::vector<EntityId>> children_;
    static const std::vector<EntityId> empty_children_;

    std::unordered_map<std::type_index, std::unique_ptr<IComponentStorage>> storages_;
};

// Read-only iteration over entities that have all of Ts...
template <typename... Ts>
class View {
public:
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = EntityId;
        using difference_type = std::ptrdiff_t;
        using pointer = const EntityId*;
        using reference = EntityId;

        Iterator() = default;
        Iterator(const World* w, std::vector<EntityId>::const_iterator it,
                 std::vector<EntityId>::const_iterator end)
            : world_(w), it_(it), end_(end) { skip(); }

        EntityId operator*() const { return *it_; }
        Iterator& operator++() { ++it_; skip(); return *this; }
        Iterator operator++(int) { Iterator tmp = *this; ++(*this); return tmp; }
        bool operator==(const Iterator& o) const { return it_ == o.it_; }
        bool operator!=(const Iterator& o) const { return it_ != o.it_; }

    private:
        void skip() {
            while (it_ != end_ && !world_->template has_all<Ts...>(*it_)) ++it_;
        }
        const World* world_ = nullptr;
        std::vector<EntityId>::const_iterator it_{};
        std::vector<EntityId>::const_iterator end_{};
    };

    View(const World* w, std::vector<EntityId> first) : world_(w), first_(std::move(first)) {}
    Iterator begin() const { return Iterator(world_, first_.begin(), first_.end()); }
    Iterator end() const { return Iterator(world_, first_.end(), first_.end()); }

private:
    const World* world_;
    std::vector<EntityId> first_;
};

template <typename T, typename... Rest>
struct first_of { using type = T; };
template <typename... Ts>
using first_of_t = typename first_of<Ts...>::type;

template <typename... Ts>
View<Ts...> World::view() const {
    const auto* first = find_storage<first_of_t<Ts...>>();
    if (!first) return View<Ts...>(this, {});
    return View<Ts...>(this, first->entities());
}

} // namespace ignis

// ComponentRegistry needs the complete World for its lambdas; include it here
// (component_registry.h only forward-declares World, so this is not circular).
#include "ignis/ecs/component_registry.h"

namespace ignis {

template <typename T>
void World::register_component_type(const std::string& name) {
    ComponentRegistry::global().register_type<T>(name);
}

inline ComponentRegistry& World::registry() noexcept { return ComponentRegistry::global(); }
inline const ComponentRegistry& World::registry() const noexcept { return ComponentRegistry::global(); }

} // namespace ignis
