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
#include "ignis/core/path_util.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

using namespace ignis;

namespace fs = std::filesystem;

namespace {

// Collects regular files under dir, keeping only files that resolve (following
// symlinks) inside root. Symlinked entries are never followed, so nothing
// outside the export root can enter the package through path resolution.
void collect_files(const fs::path& dir, const fs::path& root, std::vector<std::pair<std::string, fs::path>>& out) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    for (const auto& entry : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) return;
        if (entry.is_symlink()) {
            IGNIS_WARN("export: skipping symlink '{}'", entry.path().string());
            continue;
        }
        if (!entry.is_regular_file()) continue;
        const fs::path canon = fs::weakly_canonical(entry.path(), ec);
        if (ec || !is_path_within_root(root, canon)) {
            IGNIS_WARN("export: skipping file outside export root '{}'", entry.path().string());
            continue;
        }
        const std::string rel = fs::relative(entry.path(), root).generic_string();
        out.emplace_back(rel, entry.path());
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string scene_path, out_dir, runtime_path, root_arg;
    std::vector<std::string> includes;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--scene") scene_path = next("--scene");
        else if (a == "--out") out_dir = next("--out");
        else if (a == "--runtime") runtime_path = next("--runtime");
        else if (a == "--include") includes.push_back(next("--include"));
        else if (a == "--root") root_arg = next("--root");
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }
    if (scene_path.empty() || out_dir.empty()) {
        std::fprintf(stderr, "usage: ignis-export --scene S --out DIR [--root DIR] [--include DIR]... [--runtime EXE]\n");
        return 2;
    }

    // Export root: everything packaged must resolve inside it (default: CWD).
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(
        root_arg.empty() ? fs::current_path() : fs::path(root_arg), ec);
    if (ec) {
        IGNIS_ERROR("cannot resolve export root: {}", ec.message());
        return 1;
    }

    PakWriter writer;

    // Scene goes in as "scene.json".
    if (auto r = writer.add_file_from_disk("scene.json", scene_path); !r.ok()) {
        IGNIS_ERROR("{}", r.error());
        return 1;
    }

    // Includes: keep their path relative to the export root.
    for (const auto& inc : includes) {
        std::vector<std::pair<std::string, fs::path>> files;
        collect_files(inc, root, files);
        for (const auto& [rel, path] : files) {
            if (auto r = writer.add_file_from_disk(rel, path.string()); !r.ok()) {
                IGNIS_ERROR("{}", r.error());
                return 1;
            }
        }
    }

    fs::create_directories(out_dir);
    const std::string pak_path = (fs::path(out_dir) / "game.ignispak").string();
    if (auto r = writer.write(pak_path); !r.ok()) {
        IGNIS_ERROR("{}", r.error());
        return 1;
    }

    // Verify: read the pak back before declaring success.
    PakReader reader;
    if (auto r = reader.open(pak_path); !r.ok()) {
        IGNIS_ERROR("pak verification failed: {}", r.error());
        return 1;
    }
    for (const auto& name : reader.list()) {
        if (auto r = reader.read(name); !r.ok()) {
            IGNIS_ERROR("pak verification failed for '{}': {}", name, r.error());
            return 1;
        }
    }

    // Manifest: human-readable list of every included file.
    nlohmann::json manifest;
    manifest["name"] = fs::path(scene_path).stem().string();
    manifest["pak"] = "game.ignispak";
    manifest["entry_scene"] = "scene.json";
    manifest["files"] = nlohmann::json::array();
    for (const auto& name : reader.list()) manifest["files"].push_back(name);
    if (!runtime_path.empty()) {
        const fs::path dst = fs::path(out_dir) / fs::path(runtime_path).filename();
        fs::copy_file(runtime_path, dst, fs::copy_options::overwrite_existing);
        manifest["runtime"] = dst.filename().string();
    }
    {
        std::ofstream f(fs::path(out_dir) / "manifest.json");
        f << manifest.dump(2);
    }
    IGNIS_INFO("exported {} file(s) -> {}", reader.list().size(), pak_path);
    return 0;
}
