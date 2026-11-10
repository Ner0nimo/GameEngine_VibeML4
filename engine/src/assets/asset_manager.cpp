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

Result<std::vector<u8>> AssetManager::read_file(const std::string& path) {
    if (pak_) {
        auto from_pak = pak_->read(path);
        if (from_pak.ok()) return from_pak;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return Result<std::vector<u8>>::failure("cannot open '" + path + "'");
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    return Result<std::vector<u8>>::success(
        std::vector<u8>(s.begin(), s.end()));
}

Result<std::string> AssetManager::read_text(const std::string& path) {
    auto bytes = read_file(path);
    if (!bytes.ok()) return Result<std::string>::failure(bytes.error());
    const auto& b = bytes.value();
    return Result<std::string>::success(std::string(b.begin(), b.end()));
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
