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
#include "ignis/scene/history.h"
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
#include <imgui.h>
#endif

namespace ignis {

// Viewport gizmo operation shown in the editor toolbar.
enum class GizmoOp : u8 { Translate = 0, Rotate, Scale };

// The editor application: window + Vulkan renderer + ImGui editor UI
// (menu bar with file/gameobject/help, toolbar with gizmo mode/space/snap,
// hierarchy with filter + drag&drop reparenting, inspector, console with
// level filters + JSON command input, asset browser, stats graph, status bar,
// viewport overlay + transform gizmo, play-mode snapshot/restore) + an
// AI-agent command interface (--commands). Only built with
// IGNIS_WITH_GRAPHICS=ON.
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
    std::unique_ptr<SceneHistory> history_; // undo/redo for scene mutations
    EntityId selected_ = 0;        // primary selection (drives inspector + gizmo)
    std::vector<EntityId> selection_; // multi-selection; editor state, never serialized
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
        u32 fb_width = 0;   // swapchain extent the views/fbs were built for
        u32 fb_height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    UiHandles ui_;
    ImGuiStyle base_style_{};  // pristine style captured after StyleColorsDark
    f32 content_scale_ = 1.f;  // last applied DPI content scale

    // --- editor UI state ------------------------------------------------------
    std::string scene_path_; // file the current scene is loaded from / saves to
    GizmoOp gizmo_op_ = GizmoOp::Translate;
    bool gizmo_world_space_ = false; // gizmo mode: LOCAL (object) vs WORLD
    bool gizmo_snap_ = false;
    f32 snap_translate_ = 0.5f;
    f32 snap_rotate_deg_ = 15.f;
    f32 snap_scale_ = 0.1f;
    bool show_hierarchy_ = true;
    bool show_inspector_ = true;
    bool show_bottom_ = true;
    bool show_statusbar_ = true;
    bool show_overlay_ = true;  // viewport text overlay
    bool show_help_ = false;
    int bottom_tab_ = 0; // 0 = console, 1 = assets, 2 = stats, 3 = scene settings
    std::vector<std::pair<LogLevel, std::string>> console_;
    std::mutex console_mutex_;
    char console_filter_[128] = "";
    bool console_autoscroll_ = true;
    bool log_show_[6] = {true, true, true, true, true, true}; // by LogLevel index
    char console_input_[512] = ""; // JSON agent command line
    std::string selected_asset_;
    EntityId rename_target_ = NULL_ENTITY;
    char rename_buf_[128] = "";
    char hierarchy_filter_[128] = "";
    bool save_as_open_ = false;
    char save_as_buf_[256] = "";
    std::vector<f32> fps_history_; // ring buffer (capped) for the stats graph
    nlohmann::json play_snapshot_;  // scene state captured when play starts
    bool has_snapshot_ = false;
    // viewport marquee (rubber-band selection) state, pixel coords
    bool lmb_down_viewport_ = false;
    math::Vec2 lmb_press_{0.f, 0.f};
    bool marquee_active_ = false;
    math::Vec2 marquee_end_{0.f, 0.f};
    // fly-camera input state
    f32 cam_yaw_ = 0.f, cam_pitch_ = 0.f;
    bool rmb_down_ = false;
    f64 last_mx_ = 0.0, last_my_ = 0.0;
    bool left_clicked_ = false;
    // Viewport camera state (editor-only: persisted to editor_state.json,
    // never serialized into the scene file).
    Camera editor_cam_;           // standalone editor camera, flown instead of the scene camera
    Camera view_cam_;             // the camera the viewport rendered last frame
    bool standalone_cam_ = false; // true: viewport input drives editor_cam_, not the scene
    bool editor_ortho_ = false;   // viewport projection mode: perspective vs orthographic
    f32 editor_ortho_size_ = 10.f;
    f32 cam_speed_ = 6.f;         // fly speed in units/s (Shift = 4x)
    f64 scroll_pending_ = 0.0;    // accumulated wheel delta, consumed by handle_viewport_input
    // Viewport debug-draw toggles (editor state, persisted in editor_state.json).
    bool dbg_colliders_ = true;    // collider wireframes, colored by rigid-body type
    bool dbg_selection_ = true;    // world AABB outline around the selected entity
    bool dbg_grid_ = false;        // XZ ground grid
    bool dbg_axes_ = false;        // RGB world axes at the origin
    bool dbg_cap_warned_ = false;  // latch for the 4096-segment cap warning
    bool apply_default_layout_ = false; // no saved dock layout yet -> build the default once (#56)

    static bool probe_vulkan_device();
    Result<void> init_window_and_renderer();
    Result<void> init_scene_and_systems(const std::string& scene_path, const std::string& commands_path);
    Result<void> init_imgui_ui();
    void main_loop(); // one frame = input, simulate, UI, render
    void shutdown_ui();

    // Swapchain-dependent UI resources (issue #53): rebuilt whenever the
    // renderer recreates the swapchain. The ImGui context/GLFW backend and
    // descriptor/command pools survive; views/framebuffers/render pass and
    // the ImGui Vulkan backend registration do not.
    Result<void> create_ui_swapchain_resources();
    void destroy_ui_swapchain_resources();
    void rebuild_ui_after_swapchain_change();
    void apply_content_scale(f32 scale);
    void load_window_settings();
    void save_window_settings() const;

    // One frame of UI, one function per panel (one level of abstraction).
    EntityId find_active_camera() const;
    void update_camera(Camera& camera, EntityId cam_entity) const;
    // Publish the camera the viewport renders this frame: the standalone
    // editor camera when enabled, else the scene's active camera, with the
    // editor-side projection mode applied on top.
    void publish_view_camera(Camera& camera, EntityId cam_entity);
    void handle_viewport_input(Camera& camera, EntityId cam_entity, f32 dt);
    void handle_shortcuts();
    // Write pose/offset to whichever camera the viewport input drives.
    void set_view_pose(const math::Vec3& pos, const math::Quat& rot, EntityId cam_entity);
    void apply_view_offset(const math::Vec3& delta, EntityId cam_entity);
    void apply_view_rotation(const math::Quat& rot, EntityId cam_entity);
    void sync_yaw_pitch_from(const math::Quat& rot);
    void set_standalone_camera(bool on, EntityId cam_entity);
    void load_editor_state();
    void save_editor_state() const;
    std::vector<DebugLine> build_debug_lines();
    void build_default_dock_layout(u32 dockspace_id); // #56: default docked workspace
    void frame_view(const math::Vec3& center, f32 radius, f32 aspect, EntityId cam_entity);
    void draw_menu_bar();
    void draw_open_scene_menu();
    void draw_gameobject_menu();
    void draw_help_menu();
    void draw_help_window();
    void draw_save_as_modal();
    void draw_toolbar();
    void draw_hierarchy();
    void draw_hierarchy_node(EntityId e);
    void draw_inspector();
    void draw_bottom_panel(f32 dt);
    void draw_console();
    void draw_console_input();
    void draw_asset_browser();
    void draw_stats(f32 dt);
    void draw_scene_settings();
    void draw_status_bar(f32 dt);
    void draw_viewport_gizmo(const Camera& camera, f32 aspect);
    void draw_view_nav_gizmo(const Camera& camera, f32 aspect, EntityId cam_entity);
    void draw_viewport_overlay(const Camera& camera, EntityId cam_entity);
    // scene actions (shared by menus, toolbar and shortcuts)
    void update_window_title();
    void save_scene();
    void new_scene();
    void open_scene(const std::string& path);
    void reload_scene();
    void start_play(); // snapshot scene, then simulate
    void stop_play();  // restore the snapshot taken by start_play
    EntityId create_gameobject(const std::string& kind);
    void frame_selected();
    void frame_scene();
    void run_console_command(const std::string& line);
    void assign_selected_asset();
    void delete_entity(EntityId e);
    void duplicate_entity(EntityId e);
    void undo();
    void redo();
    // multi-selection (replace / add / toggle / remove / range in tree order)
    void select_replace(EntityId e);
    void select_add(EntityId e);
    void select_toggle(EntityId e);
    void select_remove(EntityId e);
    void select_range(EntityId anchor, EntityId e, bool add);
    void clear_selection();
    void prune_selection();
    std::vector<EntityId> tree_order() const;
    void marquee_select(const picking::ScreenRect& rect, bool add, bool remove);
    void delete_selected();
    void duplicate_selected();
#endif
};

} // namespace ignis
