#pragma once

#include "ignis/assets/texture.h"
#include "ignis/core/types.h"
#include "ignis/math/math.h"

#include <vector>

namespace ignis {

// CPU-side IBL helpers: convert an equirectangular HDR image into cubemaps
// used for PBR image-based lighting, without a GPU prefilter pass.
// Pure CPU -> unit-testable headless.
struct Cubemap {
    int face_size = 0;
    std::vector<f32> faces; // 6 * face_size * face_size * 4 floats
};

// Sample the equirect HDR at direction d (linear interpolation).
math::Vec3 sample_equirect(const Texture& hdr, const math::Vec3& d);

// Build an irradiance cubemap by cosine-weighted hemisphere sampling
// (diffuse IBL).
Cubemap equirect_to_irradiance(const Texture& hdr, int face_size = 32, int samples = 64);

// Build a mipmapped radiance cubemap by GGX (Trowbridge-Reitz) importance
// sampling (specular IBL, split-sum approximation). levels[i] has
// face_size = base_face_size >> i and corresponds to
// roughness = i / (mip_levels - 1); the shader picks the mip with textureLod.
std::vector<Cubemap> equirect_to_prefiltered(const Texture& hdr, int base_face_size = 64,
                                             int mip_levels = 5, int samples = 512);

// Split-sum BRDF integration LUT: size x size RG float texels.
// uv = (NdotV, roughness) -> (scale, bias) so the shader computes
// specular = prefilteredColor * (fresnelRoughness(NdotV, F0, rough) * scale + bias).
std::vector<f32> build_brdf_lut(int size = 128, int samples = 1024);

} // namespace ignis
