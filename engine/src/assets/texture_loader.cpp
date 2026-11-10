#include "ignis/assets/asset_manager.h"

#include "ignis/core/log.h"
#include "ignis/core/pak.h"

#include <stb_image.h>

#include <cstring>

namespace ignis {

std::shared_ptr<Texture> AssetManager::load_texture_impl(const std::string& path) {
    auto bytes_r = read_file(path);
    if (!bytes_r.ok()) {
        IGNIS_ERROR("load_texture: {}", bytes_r.error());
        return nullptr;
    }
    const std::vector<u8>& bytes = bytes_r.value();
    // Reject oversized inputs before the size_t -> int conversion to stb.
    constexpr size_t kMaxImageBytes = 256ull << 20;
    if (bytes.size() > kMaxImageBytes) {
        IGNIS_ERROR("load_texture: input too large ({} bytes) '{}'", bytes.size(), path);
        return nullptr;
    }

    auto tex = std::make_shared<Texture>();
    tex->name = path;

    const bool is_hdr = path.ends_with(".hdr") || path.ends_with(".HDR");
    int w = 0, h = 0, comp = 0;
    if (is_hdr) {
        stbi_set_flip_vertically_on_load(1);
        float* data = stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                             &w, &h, &comp, 4);
        stbi_set_flip_vertically_on_load(0);
        if (!data) {
            IGNIS_ERROR("load_texture: stb_image failed on '{}': {}", path, stbi_failure_reason());
            return nullptr;
        }
        tex->hdr = true;
        tex->width = w;
        tex->height = h;
        tex->channels = 4;
        tex->hdr_pixels.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        stbi_image_free(data);
    } else {
        stbi_set_flip_vertically_on_load(1);
        u8* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                         &w, &h, &comp, 4);
        stbi_set_flip_vertically_on_load(0);
        if (!data) {
            IGNIS_ERROR("load_texture: stb_image failed on '{}': {}", path, stbi_failure_reason());
            return nullptr;
        }
        tex->hdr = false;
        tex->width = w;
        tex->height = h;
        tex->channels = 4;
        tex->pixels.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        stbi_image_free(data);
    }
    return tex;
}

std::shared_ptr<Texture> AssetManager::load_texture(const std::string& path) {
    auto it = cache_.find(path);
    if (it != cache_.end()) return std::static_pointer_cast<Texture>(it->second);
    auto tex = load_texture_impl(path);
    if (tex) cache_[path] = tex;
    return tex;
}

void AssetManager::cache_texture(const std::string& path, std::shared_ptr<Texture> tex) {
    cache_[path] = std::move(tex);
}

} // namespace ignis
