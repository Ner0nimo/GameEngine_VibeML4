// libFuzzer target for in-memory audio decoding (miniaudio: wav/mp3/flac/...).
// Malformed or hostile headers (e.g. a FLAC STREAMINFO claiming 2^36 frames)
// must fail cleanly: no crash, no huge allocation, consistent output state.

#include "ignis/audio/audio_engine.h"

#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 4u * 1024u * 1024u) return 0;
    ignis::DecodedAudio out;
    if (ignis::decode_audio_memory(data, size, out)) {
        const size_t expected = static_cast<size_t>(out.frames) * out.channels;
        if (out.samples.size() != expected) return 1; // inconsistent decode
        if (out.channels == 0 || out.sample_rate == 0) return 1;
    }
    return 0;
}
