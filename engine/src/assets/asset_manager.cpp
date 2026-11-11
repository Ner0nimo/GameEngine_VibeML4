#include "ignis/assets/asset_manager.h"

#include "ignis/core/pak.h"
#include "ignis/core/path_util.h"
#include "texture_decode.h"

#include <stb_image.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ignis {
namespace {

// IGN-002 (TOCTOU mitigation): open the final path component with O_NOFOLLOW
// so a symlink swapped in after the canonicalization check is rejected at the
// filesystem boundary, not just at check time. Intermediate-directory races
// remain (documented in path_util.h); this closes the leaf-swap window.
Result<std::vector<u8>> read_file_no_follow(const std::filesystem::path& p) {
#if defined(__linux__)
    const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return Result<std::vector<u8>>::failure("cannot open '" + p.string() + "'");
    std::vector<u8> data;
    char buf[65536];
    ssize_t n = 0;
    while ((n = ::read(fd, buf, sizeof buf)) > 0)
        data.insert(data.end(), buf, buf + n);
    ::close(fd);
    if (n < 0)
        return Result<std::vector<u8>>::failure("read error on '" + p.string() + "'");
    return Result<std::vector<u8>>::success(std::move(data));
#else
    std::error_code ec;
    if (std::filesystem::is_symlink(p, ec))
        return Result<std::vector<u8>>::failure("symlink leaf rejected: '" + p.string() + "'");
    std::ifstream f(p, std::ios::binary);
    if (!f) return Result<std::vector<u8>>::failure("cannot open '" + p.string() + "'");
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    return Result<std::vector<u8>>::success(std::vector<u8>(s.begin(), s.end()));
#endif
}

} // namespace
} // namespace ignis

namespace ignis {

namespace fs = std::filesystem;

// Defined below; decodes an in-memory image (png/jpg/hdr).
Result<std::shared_ptr<Texture>> decode_texture_memory(
    const std::string& name, const std::vector<u8>& bytes, bool force_hdr);

Result<std::vector<u8>> AssetManager::read_file(const std::string& path) {
    if (mode_ == AssetAccessMode::PackageOnly) {
        if (!pak_) return Result<std::vector<u8>>::failure("package-only mode but no pak set");
        return pak_->read(path); // a miss is an error, never a disk fallback
    }
    if (mode_ != AssetAccessMode::EditorFilesystem && is_unsafe_resource_path(path))
        return Result<std::vector<u8>>::failure("unsafe asset path '" + path + "'");
    if (pak_) {
        auto from_pak = pak_->read(path);
        if (from_pak.ok()) return from_pak;
    }
    fs::path resolved = path;
    if (mode_ == AssetAccessMode::ProjectFilesystem) {
        if (project_root_.empty())
            return Result<std::vector<u8>>::failure("no project root set for asset access");
        resolved = fs::path(project_root_) / path;
        if (!is_path_within_root(project_root_, resolved))
            return Result<std::vector<u8>>::failure("asset path '" + path + "' escapes project root");
    }
    return read_file_no_follow(resolved);
}

Result<std::string> AssetManager::read_text(const std::string& path) {
    auto bytes = read_file(path);
    if (!bytes.ok()) return Result<std::string>::failure(bytes.error());
    const auto& b = bytes.value();
    return Result<std::string>::success(std::string(b.begin(), b.end()));
}

Result<std::shared_ptr<Texture>> decode_texture_memory(
    const std::string& name, const std::vector<u8>& bytes, bool force_hdr) {
    using TexResult = Result<std::shared_ptr<Texture>>;
    constexpr size_t kMaxImageBytes = 256ull << 20; // reject before size_t -> int
    if (bytes.size() > kMaxImageBytes)
        return TexResult::failure(std::format("'{}' too large ({} bytes)", name, bytes.size()));
    // Reject oversized decoded dimensions before stb allocates the output:
    // a tiny header can claim gigabytes of pixels.
    int iw = 0, ih = 0, icomp = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &iw, &ih, &icomp))
        return TexResult::failure(
            std::format("stb_image failed on '{}': {}", name, stbi_failure_reason()));
    const long long max_pixels = force_hdr ? kMaxHdrTexturePixels : kMaxLdrTexturePixels;
    if (static_cast<long long>(iw) * static_cast<long long>(ih) > max_pixels)
        return TexResult::failure(std::format("'{}' claims {}x{} (over the {} pixel cap)",
                                              name, iw, ih, max_pixels));
    auto tex = std::make_shared<Texture>();
    tex->name = name;
    int w = 0, h = 0, comp = 0;
    if (force_hdr) {
        float* data = stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
        if (!data)
            return TexResult::failure(
                std::format("stb_image failed on '{}': {}", name, stbi_failure_reason()));
        tex->hdr = true;
        tex->width = w; tex->height = h; tex->channels = 4;
        tex->hdr_pixels.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        stbi_image_free(data);
    } else {
        u8* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
        if (!data)
            return TexResult::failure(
                std::format("stb_image failed on '{}': {}", name, stbi_failure_reason()));
        tex->hdr = false;
        tex->width = w; tex->height = h; tex->channels = 4;
        tex->pixels.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        stbi_image_free(data);
    }
    return TexResult::success(std::move(tex));
}

} // namespace ignis
