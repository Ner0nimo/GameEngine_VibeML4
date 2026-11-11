#pragma once

#include "ignis/assets/texture.h"

#include <memory>
#include <string>
#include <vector>

namespace ignis {

// Decoded-image area caps, checked BEFORE stb allocates the output buffer:
// a hostile header can claim enormous dimensions with a tiny input.
// LDR: 64M pixels = 256 MiB RGBA8; HDR: 16M pixels = 256 MiB RGBA f32.
inline constexpr long long kMaxLdrTexturePixels = 64LL * 1024 * 1024;
inline constexpr long long kMaxHdrTexturePixels = 16LL * 1024 * 1024;

// Decode an in-memory image (png/jpg via stb_image, or HDR when force_hdr).
// Returns nullptr on failure. Implemented in asset_manager.cpp.
std::shared_ptr<Texture> decode_texture_memory(
    const std::string& name, const std::vector<u8>& bytes, bool force_hdr);

} // namespace ignis
