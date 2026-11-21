#include "ignis/assets/asset_types.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace ignis {

const std::vector<AssetTypeInfo>& asset_types() {
    static const std::vector<AssetTypeInfo> kTypes = {
        {".gltf", "M", "Model", "MeshRenderer", true, false},
        {".glb", "M", "Model", "MeshRenderer", true, false},
        {".wav", "A", "Audio", "AudioSource", true, false},
        {".mp3", "A", "Audio", "AudioSource", true, false},
        {".ogg", "A", "Audio", "AudioSource", true, false},
        {".flac", "A", "Audio", "AudioSource", true, false},
        {".lua", "L", "Script", "Script", true, false},
        {".png", "T", "Texture", "", false, false},
        {".jpg", "T", "Texture", "", false, false},
        {".jpeg", "T", "Texture", "", false, false},
        {".hdr", "T", "Texture", "", false, false},
        {".json", "J", "JSON", "", false, false},
        {".scene.json", "S", "Scene", "", false, true},
    };
    return kTypes;
}

const AssetTypeInfo* find_asset_type(const std::string& extension) {
    for (const auto& t : asset_types())
        if (t.extension == extension) return &t;
    return nullptr;
}

const AssetTypeInfo* find_asset_type_for_file(const std::string& filename) {
    constexpr std::string_view kSceneSuffix = ".scene.json";
    if (filename.size() >= kSceneSuffix.size() &&
        filename.compare(filename.size() - kSceneSuffix.size(), kSceneSuffix.size(), kSceneSuffix) == 0)
        return find_asset_type(".scene.json");
    const auto pos = filename.find_last_of('.');
    if (pos == std::string::npos) return nullptr;
    return find_asset_type(filename.substr(pos));
}

std::vector<std::string> find_asset_references(const std::string& dir, const std::string& asset_rel_path) {
    std::vector<std::string> out;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        std::ifstream f(entry.path());
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        if (ss.str().find(asset_rel_path) != std::string::npos)
            out.push_back(entry.path().generic_string());
    }
    return out;
}

} // namespace ignis
