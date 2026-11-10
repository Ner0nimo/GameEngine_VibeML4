#include "ignis/renderer/renderer.h"

#include <glm/gtx/quaternion.hpp>

#include <cmath>
#include <limits>
#include <algorithm>

namespace ignis::picking {

Ray screen_ray(const Camera& cam, f32 aspect, f32 ndc_x, f32 ndc_y) {
    const f32 fov_y = math::radians(cam.fov_y_deg);
    const f32 tan_half = glm::tan(fov_y * 0.5f);
    // Camera-space direction (looking down -Z).
    const math::Vec3 dir_cam(ndc_x * tan_half * aspect, ndc_y * tan_half, -1.f);
    return Ray{cam.position, glm::normalize(glm::rotate(cam.rotation, dir_cam))};
}

std::optional<f32> ray_aabb(const math::Vec3& origin, const math::Vec3& dir,
                            const math::Vec3& center, const math::Vec3& half_extents) {
    f32 tmin = 0.f;
    f32 tmax = std::numeric_limits<f32>::max();
    for (int i = 0; i < 3; ++i) {
        const f32 o = origin[i];
        const f32 d = dir[i];
        const f32 mn = center[i] - half_extents[i];
        const f32 mx = center[i] + half_extents[i];
        if (std::abs(d) < 1e-8f) {
            if (o < mn || o > mx) return std::nullopt;
        } else {
            f32 t1 = (mn - o) / d;
            f32 t2 = (mx - o) / d;
            if (t1 > t2) std::swap(t1, t2);
            tmin = glm::max(tmin, t1);
            tmax = glm::min(tmax, t2);
            if (tmin > tmax) return std::nullopt;
        }
    }
    return tmin;
}

} // namespace ignis::picking
