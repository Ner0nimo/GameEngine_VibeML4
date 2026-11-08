#pragma once

#include "ignis/assets/texture.h"
#include "ignis/core/types.h"
#include "ignis/math/math.h"

#include <vector>

namespace ignis {

// CPU-side IBL helper: converts an equirectangular HDR image into a small
// irradiance cubemap (6 faces, face_size x face_size, RGBA float).
// Used to light PBR materials without a GPU prefilter pass (v1 of Ignis).
// Pure CPU -> unit-testable headless.
struct Cubemap {
    int face_size = 0;
    std::vector<f32> faces; // 6 * face_size * face_size * 4 floats
};

// Sample the equirect HDR at direction d (linear interpolation).
math::Vec3 sample_equirect(const Texture& hdr, const math::Vec3& d);

// Build an irradiance cubemap by cosine-weighted hemisphere sampling.
Cubemap equirect_to_irradiance(const Texture& hdr, int face_size = 32, int samples = 64);

} // namespace ignis
