#include "ignis/renderer/renderer.h"

#include <glm/gtx/quaternion.hpp>

#include <cmath>
#include <limits>
#include <algorithm>

namespace ignis {

std::vector<f32> cascade_splits(f32 z_near, f32 z_far, u32 count, f32 lambda) {
    if (z_near <= 0.f || z_far <= z_near || count == 0) return {};
    std::vector<f32> splits(static_cast<size_t>(count) + 1u);
    splits[0] = z_near;
    splits[count] = z_far;
    const f32 l = glm::clamp(lambda, 0.f, 1.f);
    for (u32 i = 1; i < count; ++i) {
        const f32 p = static_cast<f32>(i) / static_cast<f32>(count);
        const f32 log_s = z_near * std::pow(z_far / z_near, p);
        const f32 uni = z_near + (z_far - z_near) * p;
        splits[i] = l * log_s + (1.f - l) * uni;
    }
    return splits;
}

} // namespace ignis

namespace ignis::picking {

Ray screen_ray(const Camera& cam, f32 aspect, f32 ndc_x, f32 ndc_y) {
    if (cam.orthographic) {
        // Parallel ray: origin offset across the view plane, direction = forward.
        const f32 half_h = cam.ortho_size;
        const f32 half_w = half_h * aspect;
        const math::Vec3 right = glm::rotate(cam.rotation, math::Vec3(1.f, 0.f, 0.f));
        const math::Vec3 up = glm::rotate(cam.rotation, math::Vec3(0.f, 1.f, 0.f));
        const math::Vec3 fwd = glm::rotate(cam.rotation, math::Vec3(0.f, 0.f, -1.f));
        const math::Vec3 origin = cam.position + right * (ndc_x * half_w) + up * (ndc_y * half_h);
        return Ray{origin, fwd};
    }
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
