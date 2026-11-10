#pragma once

#include "ignis/agent/commands.h"
#include "ignis/assets/asset_manager.h"
#include "ignis/audio/audio_engine.h"
#include "ignis/audio/audio_system.h"
#include "ignis/core/result.h"
#include "ignis/core/types.h"
#include "ignis/ecs/entity.h"
#include "ignis/physics/physics_world.h"
#include "ignis/renderer/vulkan_renderer.h"
#include "ignis/scene/scene.h"
#include "ignis/scripting/script_engine.h"
#include "ignis/window/window.h"

#include <memory>
#include <string>
#include <vector>

#ifdef IGNIS_WITH_GRAPHICS
#include <vulkan/vulkan.h>
#endif

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
    std::unique_ptr<AudioSystem> audio_sys_;
    std::unique_ptr<CommandProcessor> commands_;
    EntityId selected_ = 0;
    bool playing_ = false;

#ifdef IGNIS_WITH_GRAPHICS
    // Raw Vulkan + ImGui overlay state (graphics builds only). run() is a
    // short orchestrator: probe -> init window/renderer -> init scene/systems
    // -> init UI -> main loop -> shutdown; each phase is one function.
    struct UiHandles {
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkRenderPass render_pass = VK_NULL_HANDLE;
        VkCommandPool cmd_pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;
        std::vector<VkImageView> views;
        std::vector<VkFramebuffer> fbs;
        u32 image_count = 0;
    };
    UiHandles ui_;

    static bool probe_vulkan_device();
    Result<void> init_window_and_renderer();
    Result<void> init_scene_and_systems(const std::string& scene_path, const std::string& commands_path);
    Result<void> init_imgui_ui();
    void main_loop(const std::string& scene_path); // one frame = input, simulate, UI, render
    void shutdown_ui();
#endif
};

} // namespace ignis
