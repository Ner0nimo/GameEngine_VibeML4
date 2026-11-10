#pragma once

#include "ignis/core/result.h"
#include "ignis/core/types.h"
#include "ignis/ecs/entity.h"

#include <string>
#include <unordered_map>

struct lua_State;
struct lua_Debug;

namespace ignis {

class Scene;
class AssetManager;

// Lua scripting for scenes. Every entity with a ScriptComponent runs a Lua
// file that may define:
//   function on_create(self)  ... end   -- self.entity is the EntityId
//   function on_update(self, dt) ... end
//   function on_destroy(self) ... end
//
// The global `ignis` table exposes the full ECS/scene API to scripts
// (see script_engine.cpp for the list).
class ScriptEngine {
public:
    ScriptEngine() = default;
    ~ScriptEngine();

    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    Result<void> init(Scene* scene, AssetManager* assets);
    void shutdown();

    lua_State* state() noexcept { return L_; }

    // Advance all script components by dt. Call once per frame.
    void update(f32 dt);
    // Call on_destroy and release the entity's script environment.
    void on_entity_destroyed(EntityId e);

    u64 frame() const noexcept { return frame_; }
    f32 delta_time() const noexcept { return dt_; }

    // Execution budget: max Lua instructions across all scripts per update()
    // call. A script that exceeds it gets a controlled Lua error, not a hang.
    void set_instruction_budget(u64 budget) noexcept { instruction_budget_ = budget; }
    u64 instruction_budget() const noexcept { return instruction_budget_; }

    // Memory budget: hard cap on total Lua heap bytes (custom lua_Alloc).
    // Allocations beyond the cap fail -> controlled Lua "not enough memory"
    // error instead of an OOM kill (instruction budgets don't cover this:
    // string.rep is a single instruction).
    void set_memory_budget(size_t bytes) noexcept { mem_cap_ = bytes; }
    size_t memory_budget() const noexcept { return mem_cap_; }
    size_t memory_used() const noexcept { return mem_used_; }

    // Path policy for path-taking Lua bindings (ignis.scene.save): mirrors
    // the agent-command policy. With host paths disallowed (exported runtime
    // / PackageOnly) every save is rejected; with a project root set, the
    // path must resolve inside it.
    void set_project_root(const std::string& root) { project_root_ = root; }
    void set_host_paths_allowed(bool allowed) { host_paths_allowed_ = allowed; }

    // Authorize a save path for ignis.scene.save per the path policy above.
    bool save_path_allowed(const std::string& path, std::string& resolved) const;

    // Reads a global from the private script environment of entity e
    // (introspection/tests). Returns false if there is no env or the global
    // is not boolean.
    bool script_env_bool(EntityId e, const char* name) const;

private:
    Result<void> ensure_env(EntityId e);
    Result<int> load_chunk(const std::string& path);
    // Call env[name](self[, dt]) if it is a function; logs and swallows runtime
    // errors here (top of the script-call boundary) so one bad script cannot
    // take down the frame loop.
    void run_callback(int env_ref, const char* name, bool pass_dt, f32 dt);
    static void instruction_hook(lua_State* L, lua_Debug* ar);
    static int readonly_newindex(lua_State* L);
    static void* mem_alloc(void* ud, void* ptr, size_t osize, size_t nsize);

    lua_State* L_ = nullptr;
    Scene* scene_ = nullptr;
    AssetManager* assets_ = nullptr;
    std::unordered_map<EntityId, int> env_refs_;   // entity -> registry ref of env table
    std::unordered_map<std::string, int> chunks_;   // path  -> registry ref of compiled chunk
    u64 frame_ = 0;
    f32 dt_ = 0.f;
    u64 instruction_budget_ = 1000000;
    u64 instruction_used_ = 0;
    size_t mem_used_ = 0;
    size_t mem_cap_ = 256ull * 1024 * 1024;
    std::string project_root_;
    bool host_paths_allowed_ = true;
};

} // namespace ignis
