#pragma once

#include "ignis/core/result.h"
#include "ignis/core/types.h"
#include "ignis/math/math.h"

#include <optional>
#include <string>
#include <vector>

namespace ignis {

class Scene;
class World;
class Window;

// Camera used by the renderer. Position/rotation are world-space.
struct Camera {
    math::Vec3 position{0.f, 0.f, 0.f};
    math::Quat rotation{1.f, 0.f, 0.f, 0.f};
    f32 fov_y_deg = 60.f;
    f32 z_near = 0.1f;
    f32 z_far = 1000.f;

    math::Mat4 view_matrix() const {
        return glm::inverse(glm::translate(math::Mat4(1.f), position) * glm::mat4_cast(rotation));
    }
    math::Mat4 projection_matrix(f32 aspect) const {
        return math::perspective_vk(math::radians(fov_y_deg), aspect, z_near, z_far);
    }
};

// Debug/gizmo drawing: world-space line segments, consumed by the renderer.
struct DebugLine {
    math::Vec3 a, b;
    math::Vec4 color{1.f, 1.f, 0.f, 1.f};
};

struct RenderStats {
    u32 draw_calls = 0;
    u32 triangles = 0;
    f32 frame_ms = 0.f;
};

// Renderer interface. The Vulkan/NVRHI implementation lives in
// vulkan_renderer.h; a headless build simply never creates one.
class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual Result<void> init(Window* window) = 0;
    virtual void shutdown() = 0;
    virtual void on_resize(u32 width, u32 height) = 0;
    virtual void render(Scene& scene, const Camera& camera, f32 dt) = 0;
    virtual void set_debug_lines(const std::vector<DebugLine>& lines) = 0;
    virtual RenderStats stats() const = 0;
};

// CPU-side helpers shared by editor picking and tests.
namespace picking {
    struct Ray {
        math::Vec3 origin{0.f};
        math::Vec3 dir{0.f, 0.f, -1.f};
    };
    // Ray from camera through a normalized screen position (x,y in [-1,1]).
    Ray screen_ray(const Camera& cam, f32 aspect, f32 ndc_x, f32 ndc_y);
    // Ray vs. axis-aligned box (center, half extents); distance, or nullopt on miss.
    std::optional<f32> ray_aabb(const math::Vec3& origin, const math::Vec3& dir,
                                const math::Vec3& center, const math::Vec3& half_extents);
} // namespace picking

} // namespace ignis
