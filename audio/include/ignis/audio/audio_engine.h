#pragma once

#include "ignis/core/types.h"

#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;
struct ma_engine;
struct ma_sound;
struct ma_context;

namespace ignis {

// Decoded PCM audio (always converted to 32-bit float).
struct DecodedAudio {
    u32 channels = 0;
    u32 sample_rate = 0;
    u64 frames = 0;
    std::vector<f32> samples; // interleaved, frames * channels
};

// Decode helpers (headless-testable, no audio device needed).
bool decode_audio_file(const std::string& path, DecodedAudio& out);
bool decode_audio_memory(const void* data, size_t size, DecodedAudio& out);

// Playback engine. On machines without an audio device (CI, headless tests)
// init with null_device=true: sounds are still "played" into the void and
// handles are valid, but nothing is audible.
class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool init(bool null_device, std::string& err);
    void shutdown();
    bool is_initialized() const { return initialized_; }
    bool is_null_device() const { return null_device_; }

    // Returns a sound id (> 0) or 0 on failure.
    u32 play(const std::string& path, f32 volume = 1.f, bool loop = false);
    void stop(u32 sound_id);
    void stop_all();
    void set_master_volume(f32 v);

    u32 active_sound_count() const { return static_cast<u32>(sounds_.size()); }

    // Lua bindings: ignis.audio.play(path[, volume[, loop]]) -> id,
    //               ignis.audio.stop(id), ignis.audio.set_volume(v)
    static void register_lua_bindings(lua_State* L);
    static AudioEngine* active() { return active_; }

private:
    ma_engine* engine_ = nullptr;
    ma_context* own_context_ = nullptr; // used when forcing the null backend
    bool initialized_ = false;
    bool null_device_ = false;
    std::unordered_map<u32, ma_sound*> sounds_;
    u32 next_id_ = 1;
    static AudioEngine* active_;
};

} // namespace ignis
