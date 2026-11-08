#include "ignis/scripting/script_engine.h"

#include "ignis/assets/asset_manager.h"
#include "ignis/core/log.h"
#include "ignis/ecs/component_registry.h"
#include "ignis/ecs/components.h"
#include "ignis/scene/scene.h"

#include "lua_all.h"

#include <vector>

namespace ignis {

namespace {

// --- lua <-> json conversion ------------------------------------------------

nlohmann::json lua_to_json(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
        case LUA_TNIL: return nullptr;
        case LUA_TBOOLEAN: return lua_toboolean(L, idx) != 0;
        case LUA_TNUMBER: {
            if (lua_isinteger(L, idx)) return lua_tointeger(L, idx);
            return lua_tonumber(L, idx);
        }
        case LUA_TSTRING: return lua_tostring(L, idx);
        case LUA_TTABLE: {
            // Array if it has [1] and no other non-integer keys, else object.
            bool is_array = lua_geti(L, idx, 1) != LUA_TNIL;
            lua_pop(L, 1);
            if (is_array) {
                nlohmann::json arr = nlohmann::json::array();
                for (lua_Integer i = 1;; ++i) {
                    if (lua_geti(L, idx, i) == LUA_TNIL) { lua_pop(L, 1); break; }
                    arr.push_back(lua_to_json(L, -1));
                    lua_pop(L, 1);
                }
                return arr;
            }
            nlohmann::json obj = nlohmann::json::object();
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                std::string key = lua_tostring(L, -2);
                if (key.empty()) key = std::to_string(lua_tointeger(L, -2));
                obj[key] = lua_to_json(L, -1);
                lua_pop(L, 1);
            }
            return obj;
        }
        default: return nullptr;
    }
}

void push_json(lua_State* L, const nlohmann::json& j) {
    switch (j.type()) {
        case nlohmann::json::value_t::null: lua_pushnil(L); break;
        case nlohmann::json::value_t::boolean: lua_pushboolean(L, j.get<bool>() ? 1 : 0); break;
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
            lua_pushinteger(L, static_cast<lua_Integer>(j.get<i64>()));
            break;
        case nlohmann::json::value_t::number_float:
            lua_pushnumber(L, static_cast<lua_Number>(j.get<f64>()));
            break;
        case nlohmann::json::value_t::string: lua_pushstring(L, j.get<std::string>().c_str()); break;
        case nlohmann::json::value_t::array: {
            lua_createtable(L, static_cast<int>(j.size()), 0);
            int i = 1;
            for (const auto& el : j) { push_json(L, el); lua_rawseti(L, -2, i++); }
            break;
        }
        case nlohmann::json::value_t::object: {
            lua_createtable(L, 0, static_cast<int>(j.size()));
            for (auto it = j.begin(); it != j.end(); ++it) {
                push_json(L, it.value());
                lua_setfield(L, -2, it.key().c_str());
            }
            break;
        }
        default: lua_pushnil(L); break;
    }
}

// --- engine access -----------------------------------------------------------
// The Scene*/AssetManager*/ScriptEngine* pointers are stored as light userdata
// in the Lua registry under well-known keys (see init()).

Scene* get_scene(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "ignis.scene");
    Scene* s = static_cast<Scene*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return s;
}

AssetManager* get_assets(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "ignis.assets");
    AssetManager* a = static_cast<AssetManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return a;
}

ScriptEngine* get_engine(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "ignis.engine");
    ScriptEngine* e = static_cast<ScriptEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return e;
}

// --- ignis.* bindings --------------------------------------------------------

int l_log(lua_State* L) {
    const int level = static_cast<int>(luaL_checkinteger(L, 1));
    const char* msg = luaL_checkstring(L, 2);
    Logger::instance().log(static_cast<LogLevel>(level), std::string(msg ? msg : ""));
    return 0;
}

int l_dt(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(get_engine(L)->delta_time()));
    return 1;
}

int l_frame(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(get_engine(L)->frame()));
    return 1;
}

int l_entity_create(lua_State* L) {
    const char* name = luaL_optstring(L, 1, "");
    lua_pushinteger(L, static_cast<lua_Integer>(get_scene(L)->world().create_entity(name)));
    return 1;
}

int l_entity_destroy(lua_State* L) {
    EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    get_engine(L)->on_entity_destroyed(e); // fires on_destroy
    get_scene(L)->world().destroy_entity(e);
    return 0;
}

int l_entity_exists(lua_State* L) {
    EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, get_scene(L)->world().alive(e) ? 1 : 0);
    return 1;
}

int l_entity_name(lua_State* L) {
    EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    lua_pushstring(L, get_scene(L)->world().name_of(e).c_str());
    return 1;
}

int l_entity_set_name(lua_State* L) {
    EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    get_scene(L)->world().set_name(e, luaL_checkstring(L, 2));
    return 0;
}

int l_entity_parent(lua_State* L) {
    EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    lua_pushinteger(L, static_cast<lua_Integer>(get_scene(L)->world().parent_of(e)));
    return 1;
}

int l_entity_set_parent(lua_State* L) {
    EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    EntityId p = static_cast<EntityId>(luaL_checkinteger(L, 2));
    get_scene(L)->world().set_parent(e, p);
    return 0;
}

int l_entity_children(lua_State* L) {
    EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    const auto& children = get_scene(L)->world().children_of(e);
    lua_createtable(L, static_cast<int>(children.size()), 0);
    int i = 1;
    for (EntityId c : children) { lua_pushinteger(L, static_cast<lua_Integer>(c)); lua_rawseti(L, -2, i++); }
    return 1;
}

int l_entity_add_component(lua_State* L) {
    EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    const char* comp = luaL_checkstring(L, 2);
    World& w = get_scene(L)->world();
    const ComponentTypeInfo* info = w.registry().find_by_name(comp ? comp : "");
    if (!info) { lua_pushnil(L); lua_pushstring(L, "unknown component"); return 2; }
    nlohmann::json props = lua_isnoneornil(L, 3) ? nlohmann::json::object() : lua_to_json(L, 3);
    info->deserialize(w, e, props);
    lua_pushboolean(L, 1);
    return 1;
}

int l_entity_get_component(lua_State* L) {
    EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    const char* comp = luaL_checkstring(L, 2);
    const World& w = get_scene(L)->world();
    const ComponentTypeInfo* info = w.registry().find_by_name(comp ? comp : "");
    if (!info || !info->serialize) { lua_pushnil(L); return 1; }
    // serialize needs a non-const World& signature; const_cast is safe here
    // because serialize only reads.
    nlohmann::json j = info->serialize(const_cast<World&>(w), e);
    if (j.is_null()) { lua_pushnil(L); return 1; }
    push_json(L, j);
    return 1;
}

int l_entity_has_component(lua_State* L) {
    EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    const char* comp = luaL_checkstring(L, 2);
    const World& w = get_scene(L)->world();
    const ComponentTypeInfo* info = w.registry().find_by_name(comp ? comp : "");
    bool has = false;
    if (info) {
        for (const auto& [type, storage] : w.storages()) {
            if (type == info->type) { has = storage->has(e); break; }
        }
    }
    lua_pushboolean(L, has ? 1 : 0);
    return 1;
}

int l_entity_remove_component(lua_State* L) {
    EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    const char* comp = luaL_checkstring(L, 2);
    World& w = get_scene(L)->world();
    const ComponentTypeInfo* info = w.registry().find_by_name(comp ? comp : "");
    if (!info) return 0;
    info->remove(w, e);
    return 0;
}

int l_entity_list(lua_State* L) {
    const auto entities = get_scene(L)->world().all_entities();
    lua_createtable(L, static_cast<int>(entities.size()), 0);
    int i = 1;
    for (EntityId e : entities) { lua_pushinteger(L, static_cast<lua_Integer>(e)); lua_rawseti(L, -2, i++); }
    return 1;
}

int l_scene_spawn_prefab(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(get_scene(L)->spawn_prefab(name ? name : "")));
    return 1;
}

int l_scene_prefabs(lua_State* L) {
    const auto names = get_scene(L)->prefab_names();
    lua_createtable(L, static_cast<int>(names.size()), 0);
    int i = 1;
    for (const auto& n : names) { lua_pushstring(L, n.c_str()); lua_rawseti(L, -2, i++); }
    return 1;
}

int l_scene_save(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    lua_pushboolean(L, get_scene(L)->save_to_file(path) ? 1 : 0);
    return 1;
}

int l_asset_load_model(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    auto model = get_assets(L)->load_model(path ? path : "");
    if (!model) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, 1);
    return 1;
}

void register_bindings(lua_State* L) {
    lua_newtable(L); // ignis

    lua_pushcfunction(L, l_log); lua_setfield(L, -2, "log");
    lua_pushcfunction(L, l_dt); lua_setfield(L, -2, "dt");
    lua_pushcfunction(L, l_frame); lua_setfield(L, -2, "frame");

    lua_newtable(L); // ignis.entity
    lua_pushcfunction(L, l_entity_create); lua_setfield(L, -2, "create");
    lua_pushcfunction(L, l_entity_destroy); lua_setfield(L, -2, "destroy");
    lua_pushcfunction(L, l_entity_exists); lua_setfield(L, -2, "exists");
    lua_pushcfunction(L, l_entity_name); lua_setfield(L, -2, "name");
    lua_pushcfunction(L, l_entity_set_name); lua_setfield(L, -2, "set_name");
    lua_pushcfunction(L, l_entity_parent); lua_setfield(L, -2, "parent");
    lua_pushcfunction(L, l_entity_set_parent); lua_setfield(L, -2, "set_parent");
    lua_pushcfunction(L, l_entity_children); lua_setfield(L, -2, "children");
    lua_pushcfunction(L, l_entity_add_component); lua_setfield(L, -2, "add_component");
    lua_pushcfunction(L, l_entity_get_component); lua_setfield(L, -2, "get_component");
    lua_pushcfunction(L, l_entity_has_component); lua_setfield(L, -2, "has_component");
    lua_pushcfunction(L, l_entity_remove_component); lua_setfield(L, -2, "remove_component");
    lua_pushcfunction(L, l_entity_list); lua_setfield(L, -2, "list");
    lua_setfield(L, -2, "entity");

    lua_newtable(L); // ignis.scene
    lua_pushcfunction(L, l_scene_spawn_prefab); lua_setfield(L, -2, "spawn_prefab");
    lua_pushcfunction(L, l_scene_prefabs); lua_setfield(L, -2, "prefabs");
    lua_pushcfunction(L, l_scene_save); lua_setfield(L, -2, "save");
    lua_setfield(L, -2, "scene");

    lua_newtable(L); // ignis.assets
    lua_pushcfunction(L, l_asset_load_model); lua_setfield(L, -2, "load_model");
    lua_setfield(L, -2, "assets");

    lua_setglobal(L, "ignis");
}

} // namespace

ScriptEngine::~ScriptEngine() {
    shutdown();
}

bool ScriptEngine::init(Scene* scene, AssetManager* assets, std::string& err) {
    scene_ = scene;
    assets_ = assets;
    L_ = luaL_newstate();
    if (!L_) {
        err = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(L_);
    register_bindings(L_);

    lua_pushlightuserdata(L_, scene);
    lua_setfield(L_, LUA_REGISTRYINDEX, "ignis.scene");
    lua_pushlightuserdata(L_, assets);
    lua_setfield(L_, LUA_REGISTRYINDEX, "ignis.assets");
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, LUA_REGISTRYINDEX, "ignis.engine");
    return true;
}

void ScriptEngine::shutdown() {
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
    }
    env_refs_.clear();
    chunks_.clear();
}

bool ScriptEngine::load_chunk(const std::string& path, int& registry_ref, std::string& err) {
    auto it = chunks_.find(path);
    if (it != chunks_.end()) {
        registry_ref = it->second;
        return true;
    }
    std::string source;
    if (!assets_->read_text(path, source)) {
        err = "cannot read script '" + path + "'";
        return false;
    }
    if (luaL_loadbuffer(L_, source.data(), source.size(), path.c_str()) != LUA_OK) {
        err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return false;
    }
    registry_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    chunks_[path] = registry_ref;
    return true;
}

bool ScriptEngine::ensure_env(EntityId e, std::string& err) {
    World& world = scene_->world();
    const auto* script = world.get<ScriptComponent>(e);
    if (!script || script->path.empty()) {
        err = "no ScriptComponent with a path";
        return false;
    }
    int chunk_ref = 0;
    if (!load_chunk(script->path, chunk_ref, err)) return false;

    // env = setmetatable({entity = e}, {__index = _G})
    lua_newtable(L_);
    lua_pushinteger(L_, static_cast<lua_Integer>(e));
    lua_setfield(L_, -2, "entity");
    lua_newtable(L_);
    lua_getglobal(L_, "_G");
    lua_setfield(L_, -2, "__index");
    lua_setmetatable(L_, -2);

    // Run the chunk with env as _ENV so its functions land in env.
    lua_rawgeti(L_, LUA_REGISTRYINDEX, chunk_ref);
    lua_setupvalue(L_, -2, 1); // set _ENV of the chunk to env
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return false;
    }

    const int env_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    env_refs_[e] = env_ref;

    // on_create(self)
    lua_rawgeti(L_, LUA_REGISTRYINDEX, env_ref);
    lua_getfield(L_, -1, "on_create");
    if (lua_isfunction(L_, -1)) {
        lua_pushvalue(L_, -2); // self = env
        if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
            IGNIS_ERROR("script on_create({}): {}", script->path, lua_tostring(L_, -1));
            lua_pop(L_, 1);
        }
    } else {
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1); // env
    return true;
}

void ScriptEngine::update(f32 dt) {
    ++frame_;
    dt_ = dt;
    if (!L_ || !scene_) return;
    World& world = scene_->world();

    std::vector<EntityId> entities;
    for (EntityId e : world.view<ScriptComponent>()) entities.push_back(e);

    for (EntityId e : entities) {
        const auto* script = world.get<ScriptComponent>(e);
        if (!script || !script->enabled) continue;
        if (!world.alive(e)) continue;

        std::string err;
        auto it = env_refs_.find(e);
        if (it == env_refs_.end()) {
            if (!ensure_env(e, err)) {
                IGNIS_ERROR("script setup failed for entity {}: {}", e, err);
                continue;
            }
            it = env_refs_.find(e);
        }

        lua_rawgeti(L_, LUA_REGISTRYINDEX, it->second);
        lua_getfield(L_, -1, "on_update");
        if (lua_isfunction(L_, -1)) {
            lua_pushvalue(L_, -2);          // self
            lua_pushnumber(L_, static_cast<lua_Number>(dt));
            if (lua_pcall(L_, 2, 0, 0) != LUA_OK) {
                IGNIS_ERROR("script on_update({}): {}", script->path, lua_tostring(L_, -1));
                lua_pop(L_, 1);
            }
        } else {
            lua_pop(L_, 1);
        }
        lua_pop(L_, 1); // env
    }
}

void ScriptEngine::on_entity_destroyed(EntityId e) {
    if (!L_) return;
    auto it = env_refs_.find(e);
    if (it == env_refs_.end()) return;

    lua_rawgeti(L_, LUA_REGISTRYINDEX, it->second);
    lua_getfield(L_, -1, "on_destroy");
    if (lua_isfunction(L_, -1)) {
        lua_pushvalue(L_, -2);
        if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
            IGNIS_ERROR("script on_destroy: {}", lua_tostring(L_, -1));
            lua_pop(L_, 1);
        }
    } else {
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1); // env
    luaL_unref(L_, LUA_REGISTRYINDEX, it->second);
    env_refs_.erase(it);
}

} // namespace ignis
