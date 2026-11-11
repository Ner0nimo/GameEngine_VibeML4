#pragma once

#include "ignis/core/types.h"
#include "ignis/ecs/entity.h"

#include <functional>
#include <string>
#include <typeindex>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace ignis {

class World;

// Serialization traits for a component. Specialize per component type:
//   template <> struct ComponentIO<MyComponent> {
//       static nlohmann::json to_json(const MyComponent&);
//       static MyComponent from_json(const nlohmann::json&);
//   };
template <typename T>
struct ComponentIO {
    static nlohmann::json to_json(const T&) { return nlohmann::json::object(); }
    static T from_json(const nlohmann::json&) { return T{}; }
};

// Type-erased operations for one registered component type.
struct ComponentTypeInfo {
    std::type_index type{typeid(void)};
    std::string name;
    std::function<void(World&, EntityId)> add_default;
    std::function<void(World&, EntityId)> remove;
    std::function<nlohmann::json(const World&, EntityId)> serialize;
    std::function<bool(World&, EntityId, const nlohmann::json&)> deserialize;
    std::function<void(World&, EntityId from, EntityId to)> copy;
};

// Maps component type names ("Transform", ...) <-> C++ types. Used by scene
// serialization, the Lua scripting API and the agent command interface.
class ComponentRegistry {
public:
    // Component types are process-global (they are C++ types), so one global
    // registry is shared by all worlds. This lets Scene::deserialize work no
    // matter when components were registered.
    static ComponentRegistry& global() {
        static ComponentRegistry instance;
        return instance;
    }

    // The impl is a separate template with the world type as a parameter so
    // that member access on World is only checked when register_type<T> is
    // instantiated (at which point World is complete at the call site).
    template <typename T>
    void register_type(std::string name) {
        register_type_impl<T, World>(*this, std::move(name));
    }

    bool has(const std::string& name) const { return by_name_.contains(name); }
    const ComponentTypeInfo* find_by_name(const std::string& name) const {
        auto it = by_name_.find(name);
        return it != by_name_.end() ? &it->second : nullptr;
    }
    const ComponentTypeInfo* find_by_type(std::type_index t) const {
        auto it = by_type_.find(t);
        return it != by_type_.end() ? &it->second : nullptr;
    }
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(by_name_.size());
        for (const auto& [n, _] : by_name_) out.push_back(n);
        return out;
    }

private:
    template <typename T, typename W>
    static void register_type_impl(ComponentRegistry& reg, std::string name) {
        ComponentTypeInfo info;
        info.type = std::type_index(typeid(T));
        info.name = name;
        info.add_default = [](W& w, EntityId e) { w.template emplace<T>(e); };
        info.remove = [](W& w, EntityId e) { w.template remove<T>(e); };
        info.serialize = [](const W& w, EntityId e) -> nlohmann::json {
            const T* c = w.template get<T>(e);
            return c ? ComponentIO<T>::to_json(*c) : nlohmann::json::object();
        };
        info.deserialize = [](W& w, EntityId e, const nlohmann::json& j) -> bool {
            if (!j.is_object()) return false;
            // nullptr = dead entity: refuse, and let the caller (scene
            // loader, Lua add_component, agent command) report it.
            return w.template emplace<T>(e, ComponentIO<T>::from_json(j)) != nullptr;
        };
        info.copy = [](W& w, EntityId from, EntityId to) {
            if (const T* c = w.template get<T>(from)) w.template emplace<T>(to, *c);
        };
        reg.by_name_[name] = info;
        reg.by_type_[info.type] = std::move(info);
    }

    std::unordered_map<std::string, ComponentTypeInfo> by_name_;
    std::unordered_map<std::type_index, ComponentTypeInfo> by_type_;
};

// NOTE: the out-of-line definition of World::register_component_type lives at
// the bottom of world.h, where World is complete.

} // namespace ignis
