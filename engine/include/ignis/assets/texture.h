#pragma once

#include "ignis/core/types.h"

#include <string>
#include <vector>

namespace ignis {

// Decoded texture image. LDR images are stored as 8-bit RGBA (sRGB content),
// HDR images (e.g. .hdr from polyhaven) as linear float RGBA.
struct Texture {
    std::string name;
    int width = 0;
    int height = 0;
    int channels = 0; // 4 = RGBA
    bool hdr = false;
    std::vector<u8> pixels;       // LDR: width*height*4 bytes
    std::vector<f32> hdr_pixels;  // HDR: width*height*4 floats

    bool valid() const { return hdr ? !hdr_pixels.empty() : !pixels.empty(); }
};

} // namespace ignis
