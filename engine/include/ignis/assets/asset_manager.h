#pragma once

#include "ignis/assets/material.h"
#include "ignis/assets/mesh.h"
#include "ignis/assets/texture.h"

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

// Loads and caches assets from disk or from an in-memory pak (see core/pak.h).
// All loaders return nullptr on failure and log the reason.
class AssetManager {
public:
    AssetManager() = default;

    // Raw bytes of any file (disk first, then pak override if set).
    bool read_file(const std::string& path, std::vector<u8>& out_bytes);
    bool read_text(const std::string& path, std::string& out_text);

    std::shared_ptr<Model> load_model(const std::string& path);    // .gltf/.glb
    std::shared_ptr<Texture> load_texture(const std::string& path); // .png/.jpg/.hdr/...

    // Optional pak overlay: when set, read_file prefers pak entries.
    void set_pak(class PakReader* pak) { pak_ = pak; }

    void clear_cache() { cache_.clear(); }
    u64 cache_size() const { return cache_.size(); }

    // Test/helper hook: inject a decoded texture directly.
    void cache_texture(const std::string& path, std::shared_ptr<Texture> tex);

private:
    std::shared_ptr<Model> load_model_impl(const std::string& path);
    std::shared_ptr<Texture> load_texture_impl(const std::string& path);

    std::unordered_map<std::string, std::shared_ptr<void>> cache_;
    class PakReader* pak_ = nullptr;
};

} // namespace ignis
