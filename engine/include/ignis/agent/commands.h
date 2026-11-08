#pragma once

#include "ignis/core/types.h"

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

private:
    Scene& scene_;
    AssetManager& assets_;
};

} // namespace ignis
