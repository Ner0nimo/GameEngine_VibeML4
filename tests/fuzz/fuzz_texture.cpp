// libFuzzer target for in-memory image decoding (stb_image: png/jpg/bmp/...,
// both LDR and HDR paths). Malformed images must fail cleanly: no crash, no
// unbounded allocation, and a decoded texture must be internally consistent.

#include "assets/texture_decode.h"

#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 8u * 1024u * 1024u) return 0;
    const std::vector<ignis::u8> bytes(data, data + size);
    if (auto tex = ignis::decode_texture_memory("fuzz.png", bytes, false)) {
        const size_t expected = static_cast<size_t>(tex->width) *
                                static_cast<size_t>(tex->height) * 4u;
        if (tex->pixels.size() != expected) return 1; // inconsistent decode
    }
    if (auto tex = ignis::decode_texture_memory("fuzz.hdr", bytes, true)) {
        const size_t expected = static_cast<size_t>(tex->width) *
                                static_cast<size_t>(tex->height) * 4u;
        if (tex->hdr_pixels.size() != expected) return 1;
    }
    return 0;
}
