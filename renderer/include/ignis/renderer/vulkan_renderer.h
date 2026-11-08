#pragma once

#include "ignis/renderer/renderer.h"

#include <memory>
#include <string>
#include <vector>

namespace ignis {

class AssetManager;

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

    bool init(Window* window, std::string& err) override;
    void shutdown() override;
    void on_resize(u32 width, u32 height) override;
    void render(Scene& scene, const Camera& camera, f32 dt) override;
    void set_debug_lines(const std::vector<DebugLine>& lines) override;
    RenderStats stats() const override { return stats_; }

    // Offscreen mode (no window). Used by tests / headless GPU rendering.
    bool init_offscreen(u32 width, u32 height, std::string& err);
    // Read back the last rendered frame as RGBA8 (width*height*4 bytes).
    bool readback(std::vector<u8>& out_rgba);
    u32 offscreen_width() const;
    u32 offscreen_height() const;

    // Assets: models referenced by MeshRendererComponent are loaded through
    // this. If none is set, an internal AssetManager is used.
    void set_asset_manager(AssetManager* am) { assets_ = am; }

    // IBL: load a polyhaven-style .hdr equirect and build the irradiance cubemap.
    bool set_environment_hdr(const std::string& path, std::string& err);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RenderStats stats_;
    std::vector<DebugLine> debug_lines_;
    AssetManager* assets_ = nullptr;
};

} // namespace ignis
