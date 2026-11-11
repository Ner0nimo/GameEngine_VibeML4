// libFuzzer target for glTF model loading. The input is written to a temp
// file inside a per-process temp dir that acts as the project root, then
// loaded through AssetManager in ProjectFilesystem mode (the same path the
// runtime uses for pak/project content).
//
// Deliberate exclusion: inputs containing ANY non-data: "uri" (buffer OR
// image URIs) are skipped, so the fuzzer never triggers host-FS reads via
// cgltf's own FILE* buffer loading or via load_texture(dir + img.uri).
// Traversal/absolute rejection for those image paths is covered by the
// AssetsSecurity unit tests instead.

#include "ignis/assets/asset_manager.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

// True if the JSON text contains a "uri" whose value is not a data: URI.
bool has_external_uri(const std::string& text) {
    size_t pos = 0;
    while ((pos = text.find("\"uri\"", pos)) != std::string::npos) {
        const size_t colon = text.find(':', pos + 5);
        if (colon == std::string::npos) return true;
        size_t v = text.find('"', colon + 1);
        if (v == std::string::npos) return true;
        ++v;
        while (v < text.size() && (text[v] == ' ' || text[v] == '\t')) ++v;
        if (v >= text.size() || text[v] != '"') return true;
        ++v;
        if (text.compare(v, 5, "data:") != 0) return true;
        pos = v;
    }
    return false;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 4u * 1024u * 1024u) return 0;
    const std::string text(reinterpret_cast<const char*>(data), size);
    if (has_external_uri(text)) return 0;

    static const std::string dir = [] {
        char tmpl[] = "/tmp/ignis_fuzz_gltf_XXXXXX";
        const char* d = mkdtemp(tmpl);
        return d ? std::string(d) : std::string("/tmp");
    }();
    const std::filesystem::path file = std::filesystem::path(dir) / "model.gltf";
    {
        std::ofstream f(file, std::ios::binary);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    ignis::AssetManager am;
    am.set_access_mode(ignis::AssetAccessMode::ProjectFilesystem);
    am.set_project_root(dir);
    std::shared_ptr<ignis::Model> model;
    try {
        model = am.load_model("model.gltf");
    } catch (...) {
        return 1; // loaders must return nullptr, never throw
    }
    if (model && model->mesh) {
        for (const auto& prim : model->mesh->primitives) {
            if (!prim.indices.empty() && prim.positions.empty()) return 1;
        }
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return 0;
}
