// libFuzzer target for scene JSON deserialization. Arbitrary JSON must never
// destabilize the engine: bounded parse depth, resource budgets, controlled
// errors.

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
    const std::string text(reinterpret_cast<const char*>(data), size);
    auto parsed = ignis::parse_json_bounded(text);
    if (!parsed.ok()) return 0;
    try {
        ignis::Scene scene = ignis::Scene::deserialize(parsed.value());
        (void)scene.serialize();
    } catch (...) {
        // Expected rejections (limits, malformed structure) are normal outcomes.
    }
    return 0;
}
