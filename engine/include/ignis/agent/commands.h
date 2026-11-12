#pragma once

#include "ignis/core/result.h"
#include "ignis/core/types.h"

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ignis {

class Scene;
class AssetManager;

struct CommandResult {
    bool ok = false;
    std::string message;
    nlohmann::json data = nlohmann::json::object();
};

// Metadata describing one agent command. This is the single source of truth
// for the editor console autocomplete, the command palette and AI-agent
// discoverability (issue #18) — never keep a separate hard-coded list.
struct CommandInfo {
    std::string name;
    std::string description;
    std::vector<std::string> params; // notable fields, e.g. "entity", "name"
};

// JSON command interface that makes the editor (and headless runtime)
// fully controllable by AI agents: every editor action is expressible as a
// JSON command, e.g.
//   {"cmd": "create_entity", "name": "Player"}
//   {"cmd": "add_component", "entity": 1, "component": "Transform",
//    "props": {"position": [1, 2, 3]}}
//   {"cmd": "spawn_prefab", "prefab": "Enemy"}
//   {"cmd": "run_commands", "file": "commands.jsonl"}   (one JSON per line)
class CommandProcessor {
public:
    CommandProcessor(Scene& scene, AssetManager& assets) : scene_(scene), assets_(assets) {}

    CommandResult execute(const nlohmann::json& cmd);
    // Execute a .jsonl file (one command per line); returns per-line results.
    std::vector<CommandResult> execute_file(const std::string& jsonl_path);
    // Every command execute() understands, with description and params.
    static std::vector<CommandInfo> known_commands();

    // Security boundary for path-taking commands (save_scene, load_model,
    // load_texture, run_commands). With host paths disallowed (exported
    // runtime) every such command fails. With a project root set, paths must
    // resolve inside it: absolute paths and traversal are rejected.
    void set_project_root(const std::string& root) { project_root_ = root; }
    void set_host_paths_allowed(bool allowed) { host_paths_allowed_ = allowed; }
    u32 max_batch_commands = 10000;
    size_t max_command_bytes = 1ull << 20;

private:
    Result<std::string> scope_path(const std::string& path) const;

    // run_commands recursion guard: execute_file may be re-entered via the
    // run_commands command; cap the nesting depth and reject batches that are
    // already on the active stack (self-referencing .jsonl files).
    static constexpr u32 kMaxBatchDepth = 8;
    u32 batch_depth_ = 0;
    std::vector<std::string> active_batches_;

    Scene& scene_;
    AssetManager& assets_;
    std::string project_root_;
    bool host_paths_allowed_ = true;
};

} // namespace ignis
