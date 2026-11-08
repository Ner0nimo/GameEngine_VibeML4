#include "ignis/assets/asset_manager.h"

#include "ignis/core/log.h"
#include "ignis/core/pak.h"

#include <stb_image.h>

#include <fstream>
#include <sstream>

namespace ignis {

// Defined in texture_loader.cpp; decodes an in-memory image (png/jpg/hdr).
std::shared_ptr<Texture> decode_texture_memory(
    const std::string& name, const std::vector<u8>& bytes, bool force_hdr);

bool AssetManager::read_file(const std::string& path, std::vector<u8>& out_bytes) {
    if (pak_) {
        if (pak_->read(path, out_bytes)) return true;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    out_bytes.assign(s.begin(), s.end());
    return true;
}

bool AssetManager::read_text(const std::string& path, std::string& out_text) {
    std::vector<u8> bytes;
    if (!read_file(path, bytes)) return false;
    out_text.assign(bytes.begin(), bytes.end());
    return true;
}

std::shared_ptr<Texture> decode_texture_memory(
    const std::string& name, const std::vector<u8>& bytes, bool force_hdr) {
    auto tex = std::make_shared<Texture>();
    tex->name = name;
    int w = 0, h = 0, comp = 0;
    if (force_hdr) {
        float* data = stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
        if (!data) return nullptr;
        tex->hdr = true;
        tex->width = w; tex->height = h; tex->channels = 4;
        tex->hdr_pixels.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        stbi_image_free(data);
    } else {
        u8* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
        if (!data) return nullptr;
        tex->hdr = false;
        tex->width = w; tex->height = h; tex->channels = 4;
        tex->pixels.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        stbi_image_free(data);
    }
    return tex;
}

} // namespace ignis
