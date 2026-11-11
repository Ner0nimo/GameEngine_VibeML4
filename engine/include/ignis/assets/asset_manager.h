#pragma once

#include "ignis/assets/material.h"
#include "ignis/assets/mesh.h"
#include "ignis/assets/texture.h"
#include "ignis/core/result.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ignis {

// A glTF model: one mesh plus its materials and textures.
struct Model {
    std::shared_ptr<Mesh> mesh;
    std::vector<Material> materials;
    std::vector<std::shared_ptr<Texture>> textures;
};

// Where asset bytes may come from. The exported runtime must use PackageOnly
// so the package boundary is a real security boundary; the editor (a trusted
// developer process) may use EditorFilesystem.
enum class AssetAccessMode {
    EditorFilesystem,   // trusted: absolute paths and host-disk fallback allowed
    ProjectFilesystem,  // relative paths resolved inside project_root only
    PackageOnly,        // release runtime: pak entries only, never the host FS
};

// Loads and caches assets from disk or from an in-memory pak (see core/pak.h).
// Loaders return Result: success carries the asset, failure carries the
// reason. The caller decides what to do with the error (log it, substitute a
// documented placeholder, or fail) — a failed load is never concealed.
class AssetManager {
public:
    AssetManager() = default;

    // Raw bytes of any file: pak entry wins when a pak is set and contains
    // the path; the disk fallback depends on the access mode (never in
    // PackageOnly). Non-editor modes reject absolute/traversal paths and,
    // in ProjectFilesystem, anything resolving outside the project root.
    Result<std::vector<u8>> read_file(const std::string& path);
    Result<std::string> read_text(const std::string& path);

    Result<std::shared_ptr<Model>> load_model(const std::string& path);    // .gltf/.glb
    Result<std::shared_ptr<Texture>> load_texture(const std::string& path); // .png/.jpg/.hdr/...

    // Optional pak overlay: when set, read_file prefers pak entries.
    void set_pak(class PakReader* pak) { pak_ = pak; }

    void set_access_mode(AssetAccessMode m) { mode_ = m; }
    AssetAccessMode access_mode() const noexcept { return mode_; }
    void set_project_root(const std::string& root) { project_root_ = root; }

    void clear_cache() { cache_.clear(); }
    u64 cache_size() const { return cache_.size(); }

    // Test/helper hook: inject a decoded texture directly.
    void cache_texture(const std::string& path, std::shared_ptr<Texture> tex);

private:
    Result<std::shared_ptr<Model>> load_model_impl(const std::string& path);
    Result<std::shared_ptr<Texture>> load_texture_impl(const std::string& path);

    std::unordered_map<std::string, std::shared_ptr<void>> cache_;
    class PakReader* pak_ = nullptr;
    AssetAccessMode mode_ = AssetAccessMode::EditorFilesystem;
    std::string project_root_;
};

} // namespace ignis
