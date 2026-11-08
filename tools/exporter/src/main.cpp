// ignis-export: packages a game for distribution.
//
// Usage:
//   ignis-export --scene scenes/my_game.scene.json
//                --include assets/ --include scripts/
//                --runtime build/release/ignis_runtime
//                --out dist/
//
// Produces:
//   dist/game.ignispak   - scene + all included files (see core/pak.h)
//   dist/manifest.json   - human-readable index
//   dist/ignis_runtime   - copied runtime executable (if --runtime given)

#include "ignis/core/log.h"
#include "ignis/core/pak.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace ignis;

namespace fs = std::filesystem;

namespace {

void collect_files(const fs::path& dir, const fs::path& base, std::vector<std::pair<std::string, fs::path>>& out) {
    if (!fs::exists(dir)) return;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            const std::string rel = fs::relative(entry.path(), base).generic_string();
            out.emplace_back(rel, entry.path());
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string scene_path, out_dir, runtime_path;
    std::vector<std::string> includes;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--scene") scene_path = next();
        else if (a == "--out") out_dir = next();
        else if (a == "--runtime") runtime_path = next();
        else if (a == "--include") includes.push_back(next());
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }
    if (scene_path.empty() || out_dir.empty()) {
        std::fprintf(stderr, "usage: ignis-export --scene S --out DIR [--include DIR]... [--runtime EXE]\n");
        return 2;
    }

    PakWriter writer;

    // Scene goes in as "scene.json".
    writer.add_file_from_disk("scene.json", scene_path);

    // Includes: preserve their path relative to CWD.
    for (const auto& inc : includes) {
        std::vector<std::pair<std::string, fs::path>> files;
        collect_files(inc, fs::current_path(), files);
        for (const auto& [rel, path] : files) {
            writer.add_file_from_disk(rel, path.string());
        }
    }

    fs::create_directories(out_dir);
    const std::string pak_path = (fs::path(out_dir) / "game.ignispak").string();
    if (!writer.write(pak_path)) {
        IGNIS_ERROR("failed to write {}", pak_path);
        return 1;
    }

    // Manifest.
    nlohmann::json manifest;
    manifest["name"] = fs::path(scene_path).stem().string();
    manifest["pak"] = "game.ignispak";
    manifest["entry_scene"] = "scene.json";
    if (!runtime_path.empty()) {
        const fs::path dst = fs::path(out_dir) / fs::path(runtime_path).filename();
        fs::copy_file(runtime_path, dst, fs::copy_options::overwrite_existing);
        manifest["runtime"] = dst.filename().string();
    }
    {
        std::ofstream f(fs::path(out_dir) / "manifest.json");
        f << manifest.dump(2);
    }

    // Verify: read the pak back.
    PakReader reader;
    if (!reader.open(pak_path)) {
        IGNIS_ERROR("pak verification failed: cannot reopen {}", pak_path);
        return 1;
    }
    for (const auto& name : reader.list()) {
        std::vector<u8> data;
        if (!reader.read(name, data)) {
            IGNIS_ERROR("pak verification failed for '{}'", name);
            return 1;
        }
    }
    IGNIS_INFO("exported {} file(s) -> {}", reader.list().size(), pak_path);
    return 0;
}
