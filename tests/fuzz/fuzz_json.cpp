// libFuzzer target for the bounded JSON parser. Arbitrary bytes must never
// crash or overflow the parser stack: the depth cap and error containment are
// what is being fuzzed.

#include "ignis/core/json_parse.h"

#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 1u * 1024u * 1024u) return 0;
    const std::string text(reinterpret_cast<const char*>(data), size);
    (void)ignis::parse_json_bounded(text);       // default depth cap (64)
    (void)ignis::parse_json_bounded(text, 4);    // small custom cap path
    return 0;
}
