#pragma once

#include "ignis/assets/texture.h"

#include <memory>
#include <string>
#include <vector>

namespace ignis {

// Decode an in-memory image (png/jpg via stb_image, or HDR when force_hdr).
// Returns nullptr on failure. Implemented in asset_manager.cpp.
std::shared_ptr<Texture> decode_texture_memory(
    const std::string& name, const std::vector<u8>& bytes, bool force_hdr);

} // namespace ignis
