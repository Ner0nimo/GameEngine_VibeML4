#include "ignis/renderer/ibl.h"

#include <algorithm>
#include <cmath>

namespace ignis {

math::Vec3 sample_equirect(const Texture& hdr, const math::Vec3& d) {
    if (!hdr.valid() || hdr.width <= 0 || hdr.height <= 0) return {0.f, 0.f, 0.f};
    // Direction -> equirect UV. u in [0,1], v in [0,1] (v=0 at top).
    const f32 u = (std::atan2(d.x, -d.z) / (2.f * math::PI)) + 0.5f;
    const f32 v = std::acos(glm::clamp(d.y, -1.f, 1.f)) / math::PI;
    const f32 w = static_cast<f32>(hdr.width);
    const f32 h = static_cast<f32>(hdr.height);
    const f32 fx = glm::clamp(u, 0.f, 1.f) * (w - 1.f);
    const f32 fy = glm::clamp(v, 0.f, 1.f) * (h - 1.f);
    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, hdr.width - 1);
    const int y1 = std::min(y0 + 1, hdr.height - 1);
    const f32 tx = fx - static_cast<f32>(x0);
    const f32 ty = fy - static_cast<f32>(y0);

    auto px = [&](int x, int y) -> math::Vec3 {
        const size_t base = (static_cast<size_t>(y) * static_cast<size_t>(hdr.width) +
                             static_cast<size_t>(x)) * 4;
        if (hdr.hdr) {
            return {hdr.hdr_pixels[base], hdr.hdr_pixels[base + 1], hdr.hdr_pixels[base + 2]};
        }
        return {static_cast<f32>(hdr.pixels[base]) / 255.f,
                static_cast<f32>(hdr.pixels[base + 1]) / 255.f,
                static_cast<f32>(hdr.pixels[base + 2]) / 255.f};
    };
    const math::Vec3 c00 = px(x0, y0), c10 = px(x1, y0);
    const math::Vec3 c01 = px(x0, y1), c11 = px(x1, y1);
    return glm::mix(glm::mix(c00, c10, tx), glm::mix(c01, c11, tx), ty);
}

Cubemap equirect_to_irradiance(const Texture& hdr, int face_size, int samples) {
    Cubemap cube;
    cube.face_size = face_size;
    const size_t face_pixels = static_cast<size_t>(face_size) * static_cast<size_t>(face_size);
    cube.faces.assign(6 * face_pixels * 4, 0.f);

    // Face directions: +X, -X, +Y, -Y, +Z, -Z (right/up per face).
    static const math::Vec3 face_dir[6] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    static const math::Vec3 face_up[6] = {
        {0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0},
    };

    const f32 fs = static_cast<f32>(face_size);
    const f32 inv_samples = 1.f / static_cast<f32>(samples);

    for (int face = 0; face < 6; ++face) {
        const math::Vec3 n = face_dir[face];
        const math::Vec3 up = face_up[face];
        const math::Vec3 right = glm::cross(up, n);
        for (int y = 0; y < face_size; ++y) {
            for (int x = 0; x < face_size; ++x) {
                const f32 u = (static_cast<f32>(x) + 0.5f) / fs * 2.f - 1.f;
                const f32 v = (static_cast<f32>(y) + 0.5f) / fs * 2.f - 1.f;
                const math::Vec3 dir = glm::normalize(n + u * right + v * up);
                // Cosine-weighted hemisphere sampling around dir.
                math::Vec3 sum{0.f};
                for (int s = 0; s < samples; ++s) {
                    const f32 r1 = (static_cast<f32>(s) + 0.5f) * inv_samples;
                    const f32 r2 = (static_cast<f32>((s * 37) % samples) + 0.5f) * inv_samples;
                    const f32 phi = 2.f * math::PI * r1;
                    const f32 cos_t = std::sqrt(1.f - r2); // cosine-weighted
                    const f32 sin_t = std::sqrt(r2);
                    const math::Vec3 up_ref = std::abs(dir.y) < 0.99f ? math::Vec3(0, 1, 0) : math::Vec3(1, 0, 0);
                    const math::Vec3 t = glm::normalize(glm::cross(up_ref, dir));
                    const math::Vec3 b = glm::cross(dir, t);
                    const math::Vec3 sd = glm::normalize(dir * cos_t +
                        (t * std::cos(phi) + b * std::sin(phi)) * sin_t);
                    sum += sample_equirect(hdr, sd);
                }
                const math::Vec3 avg = sum * inv_samples;
                const size_t base = (static_cast<size_t>(face) * face_pixels +
                                     static_cast<size_t>(y) * static_cast<size_t>(face_size) +
                                     static_cast<size_t>(x)) * 4;
                cube.faces[base] = avg.x;
                cube.faces[base + 1] = avg.y;
                cube.faces[base + 2] = avg.z;
                cube.faces[base + 3] = 1.f;
            }
        }
    }
    return cube;
}

} // namespace ignis
