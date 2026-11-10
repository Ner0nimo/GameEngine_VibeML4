#include "ignis/renderer/ibl.h"

#include <algorithm>
#include <cmath>

namespace ignis {

namespace {

// Face directions: +X, -X, +Y, -Y, +Z, -Z (right/up per face).
const math::Vec3 kFaceDir[6] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
};
const math::Vec3 kFaceUp[6] = {
    {0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0},
};

math::Vec3 face_direction(int face, int x, int y, int face_size) {
    const math::Vec3 n = kFaceDir[face];
    const math::Vec3 up = kFaceUp[face];
    const math::Vec3 right = glm::cross(up, n);
    const f32 fs = static_cast<f32>(face_size);
    const f32 u = (static_cast<f32>(x) + 0.5f) / fs * 2.f - 1.f;
    const f32 v = (static_cast<f32>(y) + 0.5f) / fs * 2.f - 1.f;
    return glm::normalize(n + u * right + v * up);
}

// van der Corput radical inverse (base 2), used by the Hammersley sequence.
f32 radical_inverse_vdc(u32 bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<f32>(bits) * 2.3283064365386963e-10f; // / 2^32
}

math::Vec2 hammersley(u32 i, u32 n) {
    return {static_cast<f32>(i) / static_cast<f32>(n), radical_inverse_vdc(i)};
}

// GGX (Trowbridge-Reitz) importance sampling of the half vector around n.
math::Vec3 importance_sample_ggx(const math::Vec2& xi, f32 roughness, const math::Vec3& n) {
    const f32 a = roughness * roughness;
    const f32 phi = 2.f * math::PI * xi.x;
    const f32 cos_t = std::sqrt((1.f - xi.y) / (1.f + (a * a - 1.f) * xi.y));
    const f32 sin_t = std::sqrt(std::max(1.f - cos_t * cos_t, 0.f));
    const math::Vec3 up_ref = std::abs(n.y) < 0.99f ? math::Vec3(0.f, 1.f, 0.f) : math::Vec3(1.f, 0.f, 0.f);
    const math::Vec3 t = glm::normalize(glm::cross(up_ref, n));
    const math::Vec3 b = glm::cross(n, t);
    return glm::normalize(t * (sin_t * std::cos(phi)) + b * (sin_t * std::sin(phi)) + n * cos_t);
}

} // namespace

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

    const f32 inv_samples = 1.f / static_cast<f32>(samples);

    for (int face = 0; face < 6; ++face) {
        for (int y = 0; y < face_size; ++y) {
            for (int x = 0; x < face_size; ++x) {
                const math::Vec3 dir = face_direction(face, x, y, face_size);
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

std::vector<Cubemap> equirect_to_prefiltered(const Texture& hdr, int base_face_size,
                                             int mip_levels, int samples) {
    std::vector<Cubemap> levels;
    if (base_face_size <= 0 || mip_levels <= 0 || samples <= 0) return levels;
    levels.reserve(static_cast<size_t>(mip_levels));
    for (int level = 0; level < mip_levels; ++level) {
        const int face_size = base_face_size >> level;
        if (face_size < 1) break;
        const f32 roughness = mip_levels > 1
            ? static_cast<f32>(level) / static_cast<f32>(mip_levels - 1) : 0.f;
        // Fewer samples are needed as the lobe widens with roughness.
        const int level_samples = std::max(samples >> level, 64);

        Cubemap cube;
        cube.face_size = face_size;
        const size_t face_pixels = static_cast<size_t>(face_size) * static_cast<size_t>(face_size);
        cube.faces.assign(6 * face_pixels * 4, 0.f);

        for (int face = 0; face < 6; ++face) {
            for (int y = 0; y < face_size; ++y) {
                for (int x = 0; x < face_size; ++x) {
                    const math::Vec3 n = face_direction(face, x, y, face_size);
                    const math::Vec3 v = n; // view == normal for the prefilter convolution
                    math::Vec3 sum{0.f};
                    f32 weight = 0.f;
                    for (int s = 0; s < level_samples; ++s) {
                        const math::Vec2 xi = hammersley(static_cast<u32>(s), static_cast<u32>(level_samples));
                        const math::Vec3 h = importance_sample_ggx(xi, roughness, n);
                        const math::Vec3 l = glm::normalize(2.f * glm::dot(v, h) * h - v);
                        const f32 ndotl = std::max(glm::dot(n, l), 0.f);
                        if (ndotl > 0.f) {
                            sum += sample_equirect(hdr, l) * ndotl;
                            weight += ndotl;
                        }
                    }
                    const math::Vec3 avg = weight > 0.f ? sum / weight : sample_equirect(hdr, n);
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
        levels.push_back(std::move(cube));
    }
    return levels;
}

std::vector<f32> build_brdf_lut(int size, int samples) {
    std::vector<f32> lut;
    if (size <= 0 || samples <= 0) return lut;
    lut.assign(static_cast<size_t>(size) * static_cast<size_t>(size) * 2u, 0.f);
    const math::Vec3 n(0.f, 0.f, 1.f);
    const f32 inv_samples = 1.f / static_cast<f32>(samples);
    for (int y = 0; y < size; ++y) {
        const f32 roughness = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(size);
        // IBL variant of the Schlick-GGX visibility term: k = roughness^2 / 2.
        const f32 k = roughness * roughness * 0.5f;
        auto g1 = [k](f32 ndotx) {
            return ndotx / (ndotx * (1.f - k) + k + 1e-7f);
        };
        for (int x = 0; x < size; ++x) {
            const f32 ndotv = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(size);
            const math::Vec3 v(std::sqrt(std::max(1.f - ndotv * ndotv, 0.f)), 0.f, ndotv);
            f32 scale = 0.f, bias = 0.f;
            for (int s = 0; s < samples; ++s) {
                const math::Vec2 xi = hammersley(static_cast<u32>(s), static_cast<u32>(samples));
                const math::Vec3 h = importance_sample_ggx(xi, roughness, n);
                const math::Vec3 l = glm::normalize(2.f * glm::dot(v, h) * h - v);
                const f32 ndotl = std::max(l.z, 0.f);
                const f32 ndoth = std::max(h.z, 0.f);
                const f32 vdoth = std::max(glm::dot(v, h), 0.f);
                if (ndotl > 0.f) {
                    const f32 g_vis = g1(ndotv) * g1(ndotl) * vdoth /
                                      std::max(ndoth * ndotv, 1e-7f);
                    const f32 fc = std::pow(1.f - vdoth, 5.f); // Fresnel with F0 = 1
                    scale += (1.f - fc) * g_vis;
                    bias += fc * g_vis;
                }
            }
            const size_t base = (static_cast<size_t>(y) * static_cast<size_t>(size) +
                                 static_cast<size_t>(x)) * 2u;
            lut[base] = scale * inv_samples;
            lut[base + 1] = bias * inv_samples;
        }
    }
    return lut;
}

} // namespace ignis
