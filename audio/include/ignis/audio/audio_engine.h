#pragma once

#include "ignis/core/result.h"
#include "ignis/core/types.h"
#include "ignis/math/math.h"

#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;
struct ma_engine;
struct ma_sound;
struct ma_decoder;
struct ma_context;

namespace ignis {

class AssetManager;

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

    Result<void> init(bool null_device);
    void shutdown();
    bool is_initialized() const { return initialized_; }
    bool is_null_device() const { return null_device_; }

    // Returns the new sound id (> 0) on success, or the failure reason.
    // When an AssetManager is set (see set_asset_manager), the file bytes are
    // resolved through it first, so sounds also play from inside an exported
    // .ignispak.
    Result<u32> play(const std::string& path, f32 volume = 1.f, bool loop = false);
    // Play from an in-memory encoded audio blob (wav/mp3/...).
    Result<u32> play_memory(const void* data, size_t size, f32 volume = 1.f, bool loop = false);
    void stop(u32 sound_id);
    void stop_all();
    void set_master_volume(f32 v);

    // Per-sound control (no-op on unknown ids).
    bool is_playing(u32 sound_id) const;
    void set_sound_volume(u32 sound_id, f32 v);
    void set_sound_pitch(u32 sound_id, f32 pitch);
    void set_sound_position(u32 sound_id, math::Vec3 pos);
    void set_sound_spatial(u32 sound_id, bool enabled, f32 min_distance, f32 max_distance);

    // 3D listener (defaults to the engine's listener 0).
    void set_listener(math::Vec3 pos, math::Vec3 forward);

    // Optional asset source for pak-aware playback.
    void set_asset_manager(AssetManager* assets) { assets_ = assets; }

    u32 active_sound_count() const { return static_cast<u32>(sounds_.size()); }

    // Lua bindings: ignis.audio.play(path[, volume[, loop]]) -> id,
    //               ignis.audio.stop(id), ignis.audio.set_volume(v)
    static void register_lua_bindings(lua_State* L);
    static AudioEngine* active() { return active_; }

private:
    struct SoundEntry {
        ma_sound* sound = nullptr;
        ma_decoder* decoder = nullptr; // owned, only for play_memory()
        std::vector<u8> bytes;         // owned encoded data the decoder reads from
    };

    ma_engine* engine_ = nullptr;
    ma_context* own_context_ = nullptr; // used when forcing the null backend
    bool initialized_ = false;
    bool null_device_ = false;
    AssetManager* assets_ = nullptr;
    Result<u32> play_memory_owned(std::vector<u8> bytes, f32 volume, bool loop);

    std::unordered_map<u32, SoundEntry> sounds_;
    u32 next_id_ = 1;
    static AudioEngine* active_;
};

} // namespace ignis
