#include "ignis/audio/audio_engine.h"

#include "ignis/assets/asset_manager.h"
#include "ignis/core/log.h"

#include "lua_all.h"

#include <fstream>
#include <iterator>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

namespace ignis {

AudioEngine* AudioEngine::active_ = nullptr;

// --- decoding ----------------------------------------------------------------

static bool decode_impl(const ma_decoder_config* cfg_hint, const char* path,
                        const void* mem, size_t mem_size, DecodedAudio& out) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (cfg_hint) cfg = *cfg_hint;

    ma_decoder decoder;
    ma_result res;
    if (path) {
        res = ma_decoder_init_file(path, &cfg, &decoder);
    } else {
        res = ma_decoder_init_memory(mem, mem_size, &cfg, &decoder);
    }
    if (res != MA_SUCCESS) return false;

    ma_uint64 total_frames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);
    if (total_frames == 0) {
        ma_decoder_uninit(&decoder);
        return false;
    }
    const size_t total_samples = static_cast<size_t>(total_frames) * static_cast<size_t>(decoder.outputChannels);
    std::vector<f32> buffer(total_samples);
    ma_uint64 read_frames = 0;
    res = ma_decoder_read_pcm_frames(&decoder, buffer.data(), total_frames, &read_frames);
    const u32 out_channels = decoder.outputChannels;
    const u32 out_sample_rate = decoder.outputSampleRate;
    ma_decoder_uninit(&decoder);
    if (res != MA_SUCCESS || read_frames == 0) return false;

    out.channels = out_channels;
    out.sample_rate = out_sample_rate;
    out.frames = read_frames;
    const size_t n = static_cast<size_t>(read_frames) * static_cast<size_t>(out.channels);
    out.samples.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
    return true;
}

bool decode_audio_file(const std::string& path, DecodedAudio& out) {
    return decode_impl(nullptr, path.c_str(), nullptr, 0, out);
}

bool decode_audio_memory(const void* data, size_t size, DecodedAudio& out) {
    return decode_impl(nullptr, nullptr, data, size, out);
}

// --- engine ------------------------------------------------------------------

AudioEngine::~AudioEngine() {
    shutdown();
}

Result<void> AudioEngine::init(bool null_device) {
    shutdown();
    null_device_ = null_device;

    engine_ = new ma_engine();
    ma_engine_config cfg = ma_engine_config_init();
    if (null_device) {
        // Force the null backend via an explicit context so init also works on
        // machines without any audio hardware.
        own_context_ = new ma_context();
        const ma_backend backends[] = { ma_backend_null };
        if (ma_context_init(backends, 1, nullptr, own_context_) == MA_SUCCESS) {
            cfg.pContext = own_context_;
        } else {
            IGNIS_WARN("AudioEngine: null-backend context init failed, trying default");
            delete own_context_;
            own_context_ = nullptr;
        }
    }

    ma_result res = ma_engine_init(&cfg, engine_);
    if (res != MA_SUCCESS) {
        // Fallback: engine without a playback device (decode-only mode).
        IGNIS_WARN("AudioEngine: engine init failed ({}), retrying with noDevice", static_cast<int>(res));
        delete engine_;
        engine_ = new ma_engine();
        ma_engine_config cfg2 = ma_engine_config_init();
        cfg2.noDevice = MA_TRUE;
        if (own_context_) cfg2.pContext = own_context_;
        res = ma_engine_init(&cfg2, engine_);
        null_device_ = true;
    }
    if (res != MA_SUCCESS) {
        std::string err = "ma_engine_init failed: " + std::to_string(static_cast<int>(res));
        delete engine_;
        engine_ = nullptr;
        return Result<void>::failure(err);
    }
    initialized_ = true;
    active_ = this;
    IGNIS_INFO("AudioEngine initialized ({})", null_device_ ? "null device" : "audio device");
    return Result<void>::success();
}

void AudioEngine::shutdown() {
    if (engine_) {
        stop_all();
        ma_engine_uninit(engine_);
        delete engine_;
        engine_ = nullptr;
    }
    if (own_context_) {
        ma_context_uninit(own_context_);
        delete own_context_;
        own_context_ = nullptr;
    }
    initialized_ = false;
    if (active_ == this) active_ = nullptr;
}

Result<u32> AudioEngine::play(const std::string& path, f32 volume, bool loop) {
    if (!initialized_ || !engine_)
        return Result<u32>::failure("audio engine not initialized");
    // All playback goes through an in-memory decoder: this makes sounds work
    // identically from disk and from inside an exported .ignispak (via the
    // AssetManager), and keeps is_playing() reliable on null devices.
    // The encoded bytes are held by the decoder; PCM is decoded on the fly.
    std::vector<u8> bytes;
    if (assets_) {
        auto r = assets_->read_file(path);
        if (!r.ok() || r.value().empty())
            return Result<u32>::failure("cannot read '" + path + "': " + r.error());
        bytes = std::move(r.value());
    } else {
        std::ifstream f(path, std::ios::binary);
        if (!f) return Result<u32>::failure("cannot open '" + path + "'");
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    return play_memory_owned(std::move(bytes), volume, loop);
}

Result<u32> AudioEngine::play_memory(const void* data, size_t size, f32 volume, bool loop) {
    if (!data || size == 0)
        return Result<u32>::failure("empty audio data");
    const auto* begin = static_cast<const u8*>(data);
    return play_memory_owned(std::vector<u8>(begin, begin + size), volume, loop);
}

Result<u32> AudioEngine::play_memory_owned(std::vector<u8> bytes, f32 volume, bool loop) {
    if (!initialized_ || !engine_ || bytes.empty())
        return Result<u32>::failure("audio engine not initialized or empty data");
    // The sound's decoder reads the encoded bytes on the audio thread, so the
    // bytes must stay alive for the sound's whole lifetime: the SoundEntry
    // owns them.
    auto* decoder = new ma_decoder();
    auto* sound = new ma_sound();
    const u32 id = next_id_++;
    SoundEntry entry;
    entry.bytes = std::move(bytes);
    if (ma_decoder_init_memory(entry.bytes.data(), entry.bytes.size(), nullptr, decoder) != MA_SUCCESS) {
        delete decoder;
        delete sound;
        return Result<u32>::failure("cannot decode audio data");
    }
    entry.decoder = decoder;
    if (ma_sound_init_from_data_source(engine_, reinterpret_cast<ma_data_source*>(decoder),
                                       0, nullptr, sound) != MA_SUCCESS) {
        ma_decoder_uninit(decoder);
        delete decoder;
        delete sound;
        return Result<u32>::failure("ma_sound_init_from_data_source failed");
    }
    ma_sound_start(sound);
    ma_sound_set_volume(sound, volume);
    ma_sound_set_looping(sound, loop ? MA_TRUE : MA_FALSE);
    entry.sound = sound;
    sounds_[id] = std::move(entry);
    return Result<u32>::success(id);
}

void AudioEngine::stop(u32 sound_id) {
    auto it = sounds_.find(sound_id);
    if (it == sounds_.end()) return;
    ma_sound_stop(it->second.sound);
    ma_sound_uninit(it->second.sound);
    if (it->second.decoder) {
        ma_decoder_uninit(it->second.decoder);
        delete it->second.decoder;
    }
    delete it->second.sound;
    sounds_.erase(it);
}

void AudioEngine::stop_all() {
    for (auto& [id, entry] : sounds_) {
        ma_sound_stop(entry.sound);
        ma_sound_uninit(entry.sound);
        if (entry.decoder) {
            ma_decoder_uninit(entry.decoder);
            delete entry.decoder;
        }
        delete entry.sound;
    }
    sounds_.clear();
}

void AudioEngine::set_master_volume(f32 v) {
    if (engine_) ma_engine_set_volume(engine_, v);
}

bool AudioEngine::is_playing(u32 sound_id) const {
    auto it = sounds_.find(sound_id);
    if (it == sounds_.end()) return false;
    return ma_sound_is_playing(it->second.sound) == MA_TRUE;
}

void AudioEngine::set_sound_volume(u32 sound_id, f32 v) {
    auto it = sounds_.find(sound_id);
    if (it != sounds_.end()) ma_sound_set_volume(it->second.sound, v);
}

void AudioEngine::set_sound_pitch(u32 sound_id, f32 pitch) {
    auto it = sounds_.find(sound_id);
    if (it != sounds_.end()) ma_sound_set_pitch(it->second.sound, pitch);
}

void AudioEngine::set_sound_position(u32 sound_id, math::Vec3 pos) {
    auto it = sounds_.find(sound_id);
    if (it != sounds_.end()) ma_sound_set_position(it->second.sound, pos.x, pos.y, pos.z);
}

void AudioEngine::set_sound_spatial(u32 sound_id, bool enabled, f32 min_distance, f32 max_distance) {
    auto it = sounds_.find(sound_id);
    if (it == sounds_.end()) return;
    ma_sound_set_spatialization_enabled(it->second.sound, enabled ? MA_TRUE : MA_FALSE);
    ma_sound_set_attenuation_model(it->second.sound, ma_attenuation_model_inverse);
    ma_sound_set_min_distance(it->second.sound, min_distance);
    ma_sound_set_max_distance(it->second.sound, max_distance);
}

void AudioEngine::set_listener(math::Vec3 pos, math::Vec3 forward) {
    if (!engine_) return;
    ma_engine_listener_set_position(engine_, 0, pos.x, pos.y, pos.z);
    ma_engine_listener_set_direction(engine_, 0, forward.x, forward.y, forward.z);
}

// --- Lua bindings --------------------------------------------------------------

namespace {

AudioEngine* engine_from_lua(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "ignis.audio_engine");
    AudioEngine* e = static_cast<AudioEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return e ? e : AudioEngine::active();
}

int l_audio_play(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    f32 volume = static_cast<f32>(luaL_optnumber(L, 2, 1.0));
    bool loop = lua_toboolean(L, 3) != 0;
    AudioEngine* e = engine_from_lua(L);
    if (!e) {
        lua_pushinteger(L, 0);
        return 1;
    }
    auto r = e->play(path, volume, loop);
    lua_pushinteger(L, r.ok() ? static_cast<lua_Integer>(r.value()) : 0);
    return 1;
}

int l_audio_stop(lua_State* L) {
    u32 id = static_cast<u32>(luaL_checkinteger(L, 1));
    if (AudioEngine* e = engine_from_lua(L)) e->stop(id);
    return 0;
}

int l_audio_set_volume(lua_State* L) {
    f32 v = static_cast<f32>(luaL_checknumber(L, 1));
    if (AudioEngine* e = engine_from_lua(L)) e->set_master_volume(v);
    return 0;
}

} // namespace

void AudioEngine::register_lua_bindings(lua_State* L) {
    lua_getglobal(L, "ignis");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "ignis");
    }
    lua_newtable(L); // ignis.audio
    lua_pushcfunction(L, l_audio_play); lua_setfield(L, -2, "play");
    lua_pushcfunction(L, l_audio_stop); lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, l_audio_set_volume); lua_setfield(L, -2, "set_volume");
    lua_setfield(L, -2, "audio");
    lua_pop(L, 1);

    lua_pushlightuserdata(L, active_);
    lua_setfield(L, LUA_REGISTRYINDEX, "ignis.audio_engine");
}

} // namespace ignis
