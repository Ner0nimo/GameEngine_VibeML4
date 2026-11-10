// libFuzzer target for the agent JSON-command interface. Each input is treated
// as a .jsonl batch (one command per line). Host filesystem access is disabled
// so fuzzed path commands fail safely at the boundary; everything else must
// be handled without crashes, escaping exceptions or unbounded work.

#include "ignis/agent/commands.h"
#include "ignis/assets/asset_manager.h"
#include "ignis/core/json_parse.h"
#include "ignis/ecs/components.h"
#include "ignis/scene/scene.h"

#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 1u * 1024u * 1024u) return 0;
    static const bool registered = [] {
        ignis::World w;
        ignis::register_builtin_components(w);
        return true;
    }();
    (void)registered;

    ignis::Scene scene("fuzz");
    ignis::AssetManager assets;
    ignis::CommandProcessor proc(scene, assets);
    proc.set_host_paths_allowed(false); // no host FS access from fuzzed commands

    const std::string text(reinterpret_cast<const char*>(data), size);
    size_t pos = 0;
    ignis::u32 lines = 0;
    while (pos <= text.size() && lines < 64) {
        const size_t nl = text.find('\n', pos);
        const std::string line =
            text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        if (line.empty()) continue;
        ++lines;
        auto parsed = ignis::parse_json_bounded(line);
        if (!parsed.ok()) continue;
        try {
            (void)proc.execute(parsed.value());
        } catch (...) {
            return 1; // commands must report failures, never throw
        }
    }
    return 0;
}
