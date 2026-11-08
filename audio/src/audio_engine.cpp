#include "ignis/audio/audio_engine.h"

#include "ignis/core/log.h"

#include "lua_all.h"

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

bool AudioEngine::init(bool null_device, std::string& err) {
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
        err = "ma_engine_init failed: " + std::to_string(static_cast<int>(res));
        delete engine_;
        engine_ = nullptr;
        return false;
    }
    initialized_ = true;
    active_ = this;
    IGNIS_INFO("AudioEngine initialized ({})", null_device_ ? "null device" : "audio device");
    return true;
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

u32 AudioEngine::play(const std::string& path, f32 volume, bool loop) {
    if (!initialized_ || !engine_) return 0;
    auto* sound = new ma_sound();
    const ma_result res = ma_sound_init_from_file(engine_, path.c_str(), 0, nullptr, nullptr, sound);
    if (res != MA_SUCCESS) {
        IGNIS_WARN("AudioEngine: cannot play '{}' (ma_result {})", path, static_cast<int>(res));
        delete sound;
        return 0;
    }
    ma_sound_set_volume(sound, volume);
    ma_sound_set_looping(sound, loop ? MA_TRUE : MA_FALSE);
    const u32 id = next_id_++;
    sounds_[id] = sound;
    return id;
}

void AudioEngine::stop(u32 sound_id) {
    auto it = sounds_.find(sound_id);
    if (it == sounds_.end()) return;
    ma_sound_stop(it->second);
    ma_sound_uninit(it->second);
    sounds_.erase(it);
}

void AudioEngine::stop_all() {
    for (auto& [id, sound] : sounds_) {
        ma_sound_stop(sound);
        ma_sound_uninit(sound);
    }
    sounds_.clear();
}

void AudioEngine::set_master_volume(f32 v) {
    if (engine_) ma_engine_set_volume(engine_, v);
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
    lua_pushinteger(L, e ? static_cast<lua_Integer>(e->play(path, volume, loop)) : 0);
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
