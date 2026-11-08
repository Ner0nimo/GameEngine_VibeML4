#pragma once

#include "ignis/agent/commands.h"
#include "ignis/assets/asset_manager.h"
#include "ignis/audio/audio_engine.h"
#include "ignis/core/types.h"
#include "ignis/ecs/entity.h"
#include "ignis/physics/physics_world.h"
#include "ignis/renderer/vulkan_renderer.h"
#include "ignis/scene/scene.h"
#include "ignis/scripting/script_engine.h"
#include "ignis/window/window.h"

#include <memory>
#include <string>

namespace ignis {


// The editor application: window + Vulkan renderer + scene editing UI +
// gizmos + an AI-agent command interface (watches a .jsonl command file).
// Only built with IGNIS_WITH_GRAPHICS=ON.
class EditorApp {
public:
    EditorApp();
    ~EditorApp();

    int run(const std::string& scene_path, const std::string& commands_path);

private:
    std::unique_ptr<Window> window_;
    std::unique_ptr<VulkanRenderer> renderer_;
    std::unique_ptr<Scene> scene_;
    std::unique_ptr<AssetManager> assets_;
    std::unique_ptr<ScriptEngine> scripts_;
    std::unique_ptr<PhysicsSystem> physics_;
    std::unique_ptr<AudioEngine> audio_;
    std::unique_ptr<CommandProcessor> commands_;
    EntityId selected_ = 0;
    bool playing_ = false;
};

} // namespace ignis
