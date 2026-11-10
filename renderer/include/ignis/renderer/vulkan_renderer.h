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
// shadow map (directional, PCSS soft shadows) -> SSAO (+blur) ->
// PBR lighting with split-sum IBL (irradiance + prefiltered radiance cubemap
// + BRDF LUT) in HDR -> ACES tonemap -> debug/gizmo lines.
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
    // True if the last render() actually produced a frame (windowed: acquire
    // succeeded). When false (e.g. swapchain out of date), callers must skip
    // presenting - presenting a stale image causes visible flicker.
    bool frame_rendered() const { return frame_rendered_; }
    // Windowed resize: only records a pending resize (safe from window event
    // callbacks). The next render() waits for the GPU and recreates the
    // swapchain + dependent targets. A 0x0 size means "minimized" - rendering
    // is suspended until a real size arrives.
    void request_resize(u32 framebuffer_width, u32 framebuffer_height);
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

    // IBL: load a polyhaven-style .hdr equirect and build the irradiance cubemap
    // plus the mipmapped prefiltered radiance cubemap (specular reflections).
    Result<void> set_environment_hdr(const std::string& path);

    // CSM: how many shadow cascades to render (clamped to 1..4). 1 disables
    // cascading and uses a single shadow map (fallback / perf toggle).
    // Safe to call before init(): cached and applied at initialization.
    void set_shadow_cascade_count(u32 count);

    // Bloom post-processing: enable/disable and tune strength (HDR, pre-tonemap),
    // luminance threshold and soft-knee width. Defaults: on, strength 0.6,
    // threshold 1.0, knee 0.5. Safe to call before init(): settings are cached
    // and applied when the renderer is initialized.
    void set_bloom(bool enabled, f32 strength = 0.6f, f32 threshold = 1.0f, f32 knee = 0.5f);

    // Screen-space reflections: enable/disable and set strength. Reflections
    // are traced against the G-buffer depth after the lighting pass and blended
    // over the IBL specular by hit confidence. Defaults: on, strength 1.0.
    // Safe to call before init(): cached and applied at initialization.
    void set_ssr(bool enabled, f32 strength = 1.0f);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RenderStats stats_;
    std::vector<DebugLine> debug_lines_;
    AssetManager* assets_ = nullptr;
    u32 last_swap_index_ = 0;
    bool resize_pending_ = false;
    u32 pending_width_ = 0;
    u32 pending_height_ = 0;
    bool frame_rendered_ = false;
    // Feature settings cached on the outer object so setters work before
    // init(); copied into Impl when it is created (and forwarded live after).
    u32 shadow_cascade_count_ = 4;
    bool bloom_enabled_ = true;
    f32 bloom_strength_ = 0.6f;
    f32 bloom_threshold_ = 1.0f;
    f32 bloom_knee_ = 0.5f;
    bool ssr_enabled_ = true;
    f32 ssr_strength_ = 1.0f;
};

} // namespace ignis
