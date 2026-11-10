#pragma once

#include "ignis/core/result.h"
#include "ignis/core/types.h"

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

namespace ignis {

class Scene;
class AssetManager;

struct CommandResult {
    bool ok = false;
    std::string message;
    nlohmann::json data = nlohmann::json::object();
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

    Scene& scene_;
    AssetManager& assets_;
    std::string project_root_;
    bool host_paths_allowed_ = true;
};

} // namespace ignis
