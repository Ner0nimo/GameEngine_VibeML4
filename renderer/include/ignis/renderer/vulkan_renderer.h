#pragma once

#include "ignis/renderer/renderer.h"

#include <memory>
#include <string>
#include <vector>

namespace ignis {

class AssetManager;

// Raw Vulkan handles for editor overlays (e.g. an ImGui pass drawn on top of
// the swapchain image). Opaque void* keeps vulkan.h out of this header.
// Only meaningful in windowed mode (windowed == true).
struct RawVulkanHandles {
    void* instance = nullptr;
    void* physical_device = nullptr;
    void* device = nullptr;
    void* queue = nullptr;
    u32 queue_family = 0;
    void* swapchain = nullptr;
    void* swap_image = nullptr; // image at swap_index
    u32 swap_index = 0;
    u32 width = 0;
    u32 height = 0;
    i32 swap_format = 0; // VkFormat as int
    u32 swap_image_count = 0;
    bool windowed = false;
};

// NVRHI/Vulkan implementation of IRenderer.
//
// Deferred pipeline: G-Buffer (albedo/normal/ORM/emissive/worldPos) ->
// shadow map (directional, 3x3 PCF soft shadows) -> SSAO (+blur) ->
// PBR lighting with IBL irradiance cubemap (HDR) -> ACES tonemap ->
// debug/gizmo lines.
//
// Two modes:
//  - windowed:  init(window) - renders into the GLFW swapchain
//  - offscreen: init_offscreen(w, h) - renders into an internal target,
//               readback() returns the tonemapped RGBA8 pixels (used by tests)
class VulkanRenderer : public IRenderer {
public:
    VulkanRenderer(); // defined in the .cpp (Impl is incomplete here)
    ~VulkanRenderer() override;

    Result<void> init(Window* window) override;
    void shutdown() override;
    void on_resize(u32 width, u32 height) override;
    // Records and submits the frame (waits for completion). Windowed mode does
    // NOT present; call present() afterwards (the editor draws its UI in
    // between). Offscreen mode: present() is a no-op.
    void render(Scene& scene, const Camera& camera, f32 dt) override;
    void present();
    RawVulkanHandles raw_handles() const;
    void set_debug_lines(const std::vector<DebugLine>& lines) override;
    RenderStats stats() const override { return stats_; }

    // Offscreen mode (no window). Used by tests / headless GPU rendering.
    Result<void> init_offscreen(u32 width, u32 height);
    // Read back the last rendered frame as RGBA8 (width*height*4 bytes).
    Result<std::vector<u8>> readback();
    u32 offscreen_width() const;
    u32 offscreen_height() const;

    // Assets: models referenced by MeshRendererComponent are loaded through
    // this. If none is set, an internal AssetManager is used.
    void set_asset_manager(AssetManager* am) { assets_ = am; }

    // IBL: load a polyhaven-style .hdr equirect and build the irradiance cubemap.
    Result<void> set_environment_hdr(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RenderStats stats_;
    std::vector<DebugLine> debug_lines_;
    AssetManager* assets_ = nullptr;
    u32 last_swap_index_ = 0;
};

} // namespace ignis
