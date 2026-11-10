#pragma once

#include "ignis/agent/commands.h"
#include "ignis/assets/asset_manager.h"
#include "ignis/audio/audio_engine.h"
#include "ignis/audio/audio_system.h"
#include "ignis/core/log.h"
#include "ignis/core/result.h"
#include "ignis/core/types.h"
#include "ignis/ecs/entity.h"
#include "ignis/physics/physics_world.h"
#include "ignis/renderer/vulkan_renderer.h"
#include "ignis/scene/scene.h"
#include "ignis/scripting/script_engine.h"
#include "ignis/window/window.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef IGNIS_WITH_GRAPHICS
#include <vulkan/vulkan.h>
#endif

namespace ignis {

// Viewport gizmo operation shown in the editor toolbar.
enum class GizmoOp : u8 { Translate = 0, Rotate, Scale };

// The editor application: window + Vulkan renderer + ImGui editor UI
// (menu/toolbar, hierarchy, inspector, console, asset browser, status bar,
// viewport transform gizmo) + an AI-agent command interface (--commands).
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

    // --- editor UI state ------------------------------------------------------
    GizmoOp gizmo_op_ = GizmoOp::Translate;
    bool show_hierarchy_ = true;
    bool show_inspector_ = true;
    bool show_bottom_ = true;
    bool show_statusbar_ = true;
    int bottom_tab_ = 0; // 0 = console, 1 = assets, 2 = stats
    std::vector<std::pair<LogLevel, std::string>> console_;
    std::mutex console_mutex_;
    char console_filter_[128] = "";
    bool console_autoscroll_ = true;
    std::string selected_asset_;
    EntityId rename_target_ = NULL_ENTITY;
    char rename_buf_[128] = "";
    // fly-camera input state
    f32 cam_yaw_ = 0.f, cam_pitch_ = 0.f;
    bool rmb_down_ = false;
    f64 last_mx_ = 0.0, last_my_ = 0.0;
    bool left_clicked_ = false;

    static bool probe_vulkan_device();
    Result<void> init_window_and_renderer();
    Result<void> init_scene_and_systems(const std::string& scene_path, const std::string& commands_path);
    Result<void> init_imgui_ui();
    void main_loop(const std::string& scene_path); // one frame = input, simulate, UI, render
    void shutdown_ui();

    // One frame of UI, one function per panel (one level of abstraction).
    EntityId find_active_camera() const;
    void update_camera(Camera& camera, EntityId cam_entity) const;
    void handle_viewport_input(Camera& camera, EntityId cam_entity, f32 dt);
    void handle_shortcuts(const std::string& scene_path);
    void draw_menu_bar(const std::string& scene_path);
    void draw_toolbar(const std::string& scene_path);
    void draw_hierarchy();
    void draw_hierarchy_node(EntityId e);
    void draw_inspector();
    void draw_bottom_panel();
    void draw_console();
    void draw_asset_browser();
    void draw_stats(f32 dt);
    void draw_status_bar(f32 dt);
    void draw_viewport_gizmo(const Camera& camera, f32 aspect);
    void delete_entity(EntityId e);
    void duplicate_entity(EntityId e);
    void reload_scene(const std::string& scene_path);
#endif
};

} // namespace ignis
