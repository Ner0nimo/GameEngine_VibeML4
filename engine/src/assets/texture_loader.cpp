#include "ignis/assets/asset_manager.h"

#include "ignis/core/pak.h"
#include "texture_decode.h"

#include <stb_image.h>

#include <cstring>
#include <format>

namespace ignis {

Result<std::shared_ptr<Texture>> AssetManager::load_texture_impl(const std::string& path) {
    using TexResult = Result<std::shared_ptr<Texture>>;
    auto bytes_r = read_file(path);
    if (!bytes_r.ok())
        return TexResult::failure(std::format("load_texture: {}", bytes_r.error()));
    const std::vector<u8>& bytes = bytes_r.value();
    // Reject oversized inputs before the size_t -> int conversion to stb.
    constexpr size_t kMaxImageBytes = 256ull << 20;
    if (bytes.size() > kMaxImageBytes)
        return TexResult::failure(
            std::format("load_texture: input too large ({} bytes) '{}'", bytes.size(), path));

    auto tex = std::make_shared<Texture>();
    tex->name = path;

    const bool is_hdr = path.ends_with(".hdr") || path.ends_with(".HDR");
    // Reject oversized decoded dimensions before stb allocates the output:
    // a tiny header can claim gigabytes of pixels.
    int iw = 0, ih = 0, icomp = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &iw, &ih, &icomp))
        return TexResult::failure(
            std::format("load_texture: stb_image failed on '{}': {}", path, stbi_failure_reason()));
    const long long max_pixels = is_hdr ? kMaxHdrTexturePixels : kMaxLdrTexturePixels;
    if (static_cast<long long>(iw) * static_cast<long long>(ih) > max_pixels)
        return TexResult::failure(std::format("load_texture: '{}' claims {}x{} (over the {} pixel cap)",
                                              path, iw, ih, max_pixels));
    int w = 0, h = 0, comp = 0;
    if (is_hdr) {
        stbi_set_flip_vertically_on_load(1);
        float* data = stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                             &w, &h, &comp, 4);
        stbi_set_flip_vertically_on_load(0);
        if (!data)
            return TexResult::failure(
                std::format("load_texture: stb_image failed on '{}': {}", path, stbi_failure_reason()));
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
        if (!data)
            return TexResult::failure(
                std::format("load_texture: stb_image failed on '{}': {}", path, stbi_failure_reason()));
        tex->hdr = false;
        tex->width = w;
        tex->height = h;
        tex->channels = 4;
        tex->pixels.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        stbi_image_free(data);
    }
    return TexResult::success(std::move(tex));
}

Result<std::shared_ptr<Texture>> AssetManager::load_texture(const std::string& path) {
    auto it = cache_.find(path);
    if (it != cache_.end())
        return Result<std::shared_ptr<Texture>>::success(
            std::static_pointer_cast<Texture>(it->second));
    auto tex_r = load_texture_impl(path);
    if (tex_r.ok()) cache_[path] = tex_r.value();
    return tex_r;
}

void AssetManager::cache_texture(const std::string& path, std::shared_ptr<Texture> tex) {
    cache_[path] = std::move(tex);
}

} // namespace ignis
