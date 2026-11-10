#include "ignis/scripting/script_engine.h"

#include "ignis/assets/asset_manager.h"
#include "ignis/core/log.h"
#include "ignis/ecs/component_registry.h"
#include "ignis/ecs/components.h"
#include "ignis/scene/scene.h"

#include "lua_all.h"

#include <unordered_set>
#include <vector>

namespace ignis {

namespace {

// --- lua <-> json conversion ------------------------------------------------
// Bounded conversion: cyclic tables, deep nesting and huge tables raise a Lua
// error (caught by the pcall at the script boundary) instead of exhausting the
// native stack or memory.

constexpr int kMaxLuaTableDepth = 64;
constexpr u64 kMaxLuaTableEntries = 100000;

nlohmann::json lua_to_json_impl(lua_State* L, int idx, int depth,
                                std::unordered_set<const void*>& visited, u64& entries,
                                bool& error) {
    if (error) return nullptr;
    if (depth > kMaxLuaTableDepth || ++entries > kMaxLuaTableEntries) {
        error = true;
        return nullptr;
    }
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
            const void* ptr = lua_topointer(L, idx);
            if (!visited.insert(ptr).second) {
                error = true;
                return nullptr;
            }
            struct Guard { std::unordered_set<const void*>& v; const void* p; ~Guard() { v.erase(p); } } g{visited, ptr};
            // Array if it has [1] and no other non-integer keys, else object.
            bool is_array = lua_geti(L, idx, 1) != LUA_TNIL;
            lua_pop(L, 1);
            if (is_array) {
                nlohmann::json arr = nlohmann::json::array();
                for (lua_Integer i = 1;; ++i) {
                    if (lua_geti(L, idx, i) == LUA_TNIL) { lua_pop(L, 1); break; }
                    arr.push_back(lua_to_json_impl(L, -1, depth + 1, visited, entries, error));
                    lua_pop(L, 1);
                    if (error) return nullptr;
                }
                return arr;
            }
            nlohmann::json obj = nlohmann::json::object();
            lua_pushnil(L);
            while (!error && lua_next(L, idx) != 0) {
                // Key handling: only string keys and integer keys are supported;
                // anything else (boolean, table, function, float, ...) is a
                // controlled error. lua_tostring() may return nullptr for
                // non-string/non-number keys and mutates number keys in place,
                // so the key type is checked explicitly first.
                const int kt = lua_type(L, -2);
                std::string key;
                if (kt == LUA_TSTRING) {
                    const char* ks = lua_tostring(L, -2);
                    key = ks != nullptr ? ks : "";
                } else if (kt == LUA_TNUMBER && lua_isinteger(L, -2)) {
                    key = std::to_string(lua_tointeger(L, -2));
                } else {
                    error = true;
                    lua_pop(L, 2); // value + key
                    break;
                }
                obj[key] = lua_to_json_impl(L, -1, depth + 1, visited, entries, error);
                lua_pop(L, 1);
            }
            if (error) return nullptr;
            return obj;
        }
        default: return nullptr;
    }
}

nlohmann::json lua_to_json(lua_State* L, int idx) {
    bool error = false;
    nlohmann::json out;
    {
        std::unordered_set<const void*> visited;
        u64 entries = 0;
        nlohmann::json tmp = lua_to_json_impl(L, idx, 0, visited, entries, error);
        if (!error) out = std::move(tmp); // on error, keep out empty (no allocation)
    }
    // Raise the Lua error only after the C++ locals above are destroyed, so the
    // longjmp does not skip destructors (keeps LeakSanitizer clean).
    if (error) luaL_error(L, "lua table is cyclic, too deep or too large");
    return out;
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
    lua_pushboolean(L, get_scene(L)->world().set_parent(e, p) ? 1 : 0);
    return 1;
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
    lua_pushboolean(L, info->deserialize(w, e, props) ? 1 : 0);
    return 1;
}

int l_entity_get_component(lua_State* L) {
    EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    const char* comp = luaL_checkstring(L, 2);
    const World& w = get_scene(L)->world();
    const ComponentTypeInfo* info = w.registry().find_by_name(comp ? comp : "");
    if (!info || !info->serialize) { lua_pushnil(L); return 1; }
    nlohmann::json j = info->serialize(w, e);
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
    lua_pushboolean(L, get_scene(L)->save_to_file(path ? path : "").ok() ? 1 : 0);
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

void ScriptEngine::instruction_hook(lua_State* L, lua_Debug* /*ar*/) {
    ScriptEngine* eng = get_engine(L);
    if (!eng) return;
    eng->instruction_used_ += 1000; // hook fires every 1000 instructions (LUA_MASKCOUNT)
    if (eng->instruction_used_ > eng->instruction_budget_)
        luaL_error(L, "script instruction budget exceeded");
}

Result<void> ScriptEngine::init(Scene* scene, AssetManager* assets) {
    scene_ = scene;
    assets_ = assets;
    L_ = luaL_newstate();
    if (!L_) return Result<void>::failure("luaL_newstate failed");
    // Sandbox: open only the safe standard libraries. io, os, package, debug,
    // coroutine and dynamic loading (load/dofile/loadfile) are never exposed,
    // so scripts only get the explicit ignis.* API plus safe base/string/
    // table/math/utf8 functionality. Per-entity envs inherit this sandboxed
    // _G via __index, not the host's.
    luaL_requiref(L_, "_G", luaopen_base, 1); lua_pop(L_, 1);
    luaL_requiref(L_, "string", luaopen_string, 1); lua_pop(L_, 1);
    luaL_requiref(L_, "table", luaopen_table, 1); lua_pop(L_, 1);
    luaL_requiref(L_, "math", luaopen_math, 1); lua_pop(L_, 1);
    luaL_requiref(L_, "utf8", luaopen_utf8, 1); lua_pop(L_, 1);
    for (const char* fn : {"load", "dofile", "loadfile"}) {
        lua_pushnil(L_);
        lua_setglobal(L_, fn);
    }
    register_bindings(L_);

    lua_pushlightuserdata(L_, scene);
    lua_setfield(L_, LUA_REGISTRYINDEX, "ignis.scene");
    lua_pushlightuserdata(L_, assets);
    lua_setfield(L_, LUA_REGISTRYINDEX, "ignis.assets");
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, LUA_REGISTRYINDEX, "ignis.engine");
    lua_sethook(L_, &ScriptEngine::instruction_hook, LUA_MASKCOUNT, 1000);
    return Result<void>::success();
}

void ScriptEngine::shutdown() {
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
    }
    env_refs_.clear();
    chunks_.clear();
}

Result<int> ScriptEngine::load_chunk(const std::string& path) {
    auto it = chunks_.find(path);
    if (it != chunks_.end()) return Result<int>::success(it->second);
    auto source_r = assets_->read_text(path);
    if (!source_r.ok())
        return Result<int>::failure("cannot read script '" + path + "': " + source_r.error());
    const std::string& source = source_r.value();
    if (luaL_loadbuffer(L_, source.data(), source.size(), path.c_str()) != LUA_OK) {
        std::string err = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "syntax error";
        lua_pop(L_, 1);
        return Result<int>::failure("cannot compile script '" + path + "': " + err);
    }
    const int ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    chunks_[path] = ref;
    return Result<int>::success(ref);
}

Result<void> ScriptEngine::ensure_env(EntityId e) {
    World& world = scene_->world();
    const auto* script = world.get<ScriptComponent>(e);
    if (!script || script->path.empty())
        return Result<void>::failure("no ScriptComponent with a path");
    auto chunk_r = load_chunk(script->path);
    if (!chunk_r.ok()) return Result<void>::failure(chunk_r.error());
    const int chunk_ref = chunk_r.value();

    // env = setmetatable({entity = e}, {__index = _G})
    lua_newtable(L_);
    lua_pushinteger(L_, static_cast<lua_Integer>(e));
    lua_setfield(L_, -2, "entity");
    lua_newtable(L_);
    lua_getglobal(L_, "_G");
    lua_setfield(L_, -2, "__index");
    lua_setmetatable(L_, -2);

    // Run the chunk with env as _ENV so its functions land in env.
    lua_rawgeti(L_, LUA_REGISTRYINDEX, chunk_ref); // [env, chunk]
    lua_pushvalue(L_, -2);                           // [env, chunk, env]
    lua_setupvalue(L_, -2, 1);                       // chunk._ENV = env -> [env, chunk]
    instruction_used_ = 0;
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        std::string err = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "runtime error";
        lua_pop(L_, 1);
        return Result<void>::failure("script '" + script->path + "' failed to run: " + err);
    }

    const int env_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    env_refs_[e] = env_ref;

    run_callback(env_ref, "on_create", false, 0.f);
    return Result<void>::success();
}

void ScriptEngine::run_callback(int env_ref, const char* name, bool pass_dt, f32 dt) {
    lua_rawgeti(L_, LUA_REGISTRYINDEX, env_ref);
    lua_getfield(L_, -1, name);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 2);
        return;
    }
    lua_pushvalue(L_, -2); // self = env
    if (pass_dt) lua_pushnumber(L_, static_cast<lua_Number>(dt));
    instruction_used_ = 0; // budget is per script callback, not per frame
    if (lua_pcall(L_, pass_dt ? 2 : 1, 0, 0) != LUA_OK) {
        IGNIS_ERROR("script {}: {}", name, lua_tostring(L_, -1));
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1); // env
}

bool ScriptEngine::script_env_bool(EntityId e, const char* name) const {
    if (!L_) return false;
    const auto it = env_refs_.find(e);
    if (it == env_refs_.end()) return false;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, it->second); // env
    lua_getfield(L_, -1, name);
    const bool v = lua_toboolean(L_, -1) != 0;
    lua_pop(L_, 2);
    return v;
}

void ScriptEngine::update(f32 dt) {
    ++frame_;
    dt_ = dt;
    instruction_used_ = 0;
    if (!L_ || !scene_) return;
    World& world = scene_->world();

    std::vector<EntityId> entities;
    for (EntityId e : world.view<ScriptComponent>()) entities.push_back(e);

    for (EntityId e : entities) {
        const auto* script = world.get<ScriptComponent>(e);
        if (!script || !script->enabled) continue;
        if (!world.alive(e)) continue;

        auto it = env_refs_.find(e);
        if (it == env_refs_.end()) {
            auto env_r = ensure_env(e);
            if (!env_r.ok()) {
                IGNIS_ERROR("script setup failed for entity {}: {}", e, env_r.error());
                continue;
            }
            it = env_refs_.find(e);
        }
        run_callback(it->second, "on_update", true, dt);
    }
}

void ScriptEngine::on_entity_destroyed(EntityId e) {
    if (!L_) return;
    auto it = env_refs_.find(e);
    if (it == env_refs_.end()) return;

    run_callback(it->second, "on_destroy", false, 0.f);
    luaL_unref(L_, LUA_REGISTRYINDEX, it->second);
    env_refs_.erase(it);
}

} // namespace ignis
