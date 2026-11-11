#include <gtest/gtest.h>

#include "ignis/agent/commands.h"
#include "ignis/assets/asset_manager.h"
#include "ignis/audio/audio_engine.h"
#include "ignis/audio/audio_system.h"
#include "ignis/core/pak.h"
#include "ignis/ecs/components.h"
#include "ignis/scene/scene.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace ignis;

namespace {

// Hand-crafted 16-bit PCM mono WAV, 8000 Hz, 0.1 s of a 440 Hz sine.
std::vector<u8> make_wav() {
    const u32 sample_rate = 8000;
    const u32 num_samples = 800;
    std::vector<u8> pcm(num_samples * 2);
    for (u32 i = 0; i < num_samples; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(sample_rate);
        const i16 v = static_cast<i16>(std::sin(2.f * 3.14159265f * 440.f * t) * 12000.f);
        std::memcpy(pcm.data() + i * 2, &v, 2);
    }
    const u32 data_size = static_cast<u32>(pcm.size());
    const u32 byte_rate = sample_rate * 2;
    std::vector<u8> wav;
    auto push = [&](const void* p, size_t n) {
        const auto* b = static_cast<const u8*>(p);
        wav.insert(wav.end(), b, b + n);
    };
    auto push_str = [&](const char* s) { push(s, 4); };
    auto push_u32 = [&](u32 v) { push(&v, 4); };
    push_str("RIFF");
    push_u32(36 + data_size);
    push_str("WAVE");
    push_str("fmt ");
    push_u32(16);
    u16 audio_format = 1, channels = 1;
    push(&audio_format, 2);
    push(&channels, 2);
    push_u32(sample_rate);
    push_u32(byte_rate);
    u16 block_align = 2, bits = 16;
    push(&block_align, 2);
    push(&bits, 2);
    push_str("data");
    push_u32(data_size);
    push(pcm.data(), pcm.size());
    return wav;
}

} // namespace

TEST(Audio, DecodeWav) {
    const auto wav = make_wav();
    DecodedAudio audio;
    ASSERT_TRUE(decode_audio_memory(wav.data(), wav.size(), audio));
    EXPECT_EQ(audio.channels, 1u);
    EXPECT_EQ(audio.sample_rate, 8000u);
    EXPECT_EQ(audio.frames, 800u);
    EXPECT_EQ(audio.samples.size(), 800u);
    // First samples of a sine start at 0 and rise.
    EXPECT_NEAR(audio.samples[0], 0.f, 0.01f);
    EXPECT_GT(audio.samples[5], 0.f);
}

TEST(Audio, DecodeWavFromFile) {
    const auto path = std::filesystem::temp_directory_path() / "ignis_test.wav";
    {
        const auto wav = make_wav();
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    }
    DecodedAudio audio;
    ASSERT_TRUE(decode_audio_file(path.string(), audio));
    EXPECT_EQ(audio.frames, 800u);
    std::filesystem::remove(path);
}

TEST(Audio, EngineNullDevice) {
    AudioEngine engine;
    ASSERT_TRUE(engine.init(true).ok()); // null device (headless)
    EXPECT_TRUE(engine.is_null_device());

    // Play the generated wav; with no device the handle must still be valid.
    const auto path = std::filesystem::temp_directory_path() / "ignis_play_test.wav";
    {
        const auto wav = make_wav();
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    }
    auto played = engine.play(path.string(), 0.5f, false);
    ASSERT_TRUE(played.ok()) << played.error();
    const u32 id = played.value();
    EXPECT_GT(id, 0u);
    EXPECT_EQ(engine.active_sound_count(), 1u);
    engine.stop(id);
    EXPECT_EQ(engine.active_sound_count(), 0u);

    // Missing file -> failure with a message.
    auto missing = engine.play("/nonexistent.wav");
    EXPECT_FALSE(missing.ok());
    EXPECT_FALSE(missing.error().empty());

    engine.shutdown();
    EXPECT_FALSE(engine.is_initialized());
    std::filesystem::remove(path);
}

TEST(Audio, PlayMemoryAndPerSoundControl) {
    AudioEngine engine;
    ASSERT_TRUE(engine.init(true).ok());

    const auto wav = make_wav();
    auto started = engine.play_memory(wav.data(), wav.size(), 0.5f, true);
    ASSERT_TRUE(started.ok()) << started.error();
    const u32 id = started.value();
    EXPECT_EQ(engine.active_sound_count(), 1u);
    EXPECT_TRUE(engine.is_playing(id));

    // Per-sound control must not crash and must keep the handle alive.
    engine.set_sound_volume(id, 0.25f);
    engine.set_sound_pitch(id, 1.5f);
    engine.set_sound_position(id, {1.f, 2.f, 3.f});
    engine.set_sound_spatial(id, true, 1.f, 20.f);
    engine.set_listener({0.f, 0.f, 5.f}, {0.f, 0.f, -1.f});
    EXPECT_TRUE(engine.is_playing(id));
    EXPECT_FALSE(engine.is_playing(9999u)); // unknown id

    engine.stop(id);
    EXPECT_EQ(engine.active_sound_count(), 0u);
    EXPECT_FALSE(engine.is_playing(id));

    // Garbage memory -> failure.
    const char junk[] = "not audio at all";
    EXPECT_FALSE(engine.play_memory(junk, sizeof(junk)).ok());
    engine.shutdown();
}

TEST(Audio, PlayFromPakViaAssetManager) {
    // Pack the wav into an IGPK and play it through AssetManager (pak path
    // that does not exist on disk).
    const auto wav = make_wav();
    const auto pak_path = std::filesystem::temp_directory_path() / "ignis_audio_test.ignispak";
    {
        PakWriter w;
        w.add_file("sfx/beep.wav", wav);
        ASSERT_TRUE(w.write(pak_path.string()).ok());
    }
    PakReader pak;
    ASSERT_TRUE(pak.open(pak_path.string()).ok());
    AssetManager assets;
    assets.set_pak(&pak);

    AudioEngine engine;
    ASSERT_TRUE(engine.init(true).ok());
    engine.set_asset_manager(&assets);
    auto played = engine.play("sfx/beep.wav", 1.f, false);
    ASSERT_TRUE(played.ok()) << played.error();
    const u32 id = played.value();
    EXPECT_TRUE(engine.is_playing(id));
    engine.stop(id);
    EXPECT_EQ(engine.active_sound_count(), 0u);
    engine.shutdown();
    std::filesystem::remove(pak_path);
}

TEST(Audio, SourceComponentIORoundtrip) {
    AudioSourceComponent c;
    c.path = "assets/test.wav";
    c.volume = 0.8f;
    c.pitch = 1.25f;
    c.loop = true;
    c.play_on_start = true;
    c.spatial = true;
    c.min_distance = 2.f;
    c.max_distance = 40.f;
    const nlohmann::json j = ComponentIO<AudioSourceComponent>::to_json(c);
    const AudioSourceComponent back = ComponentIO<AudioSourceComponent>::from_json(j);
    EXPECT_EQ(back.path, c.path);
    EXPECT_NEAR(back.volume, c.volume, 1e-6f);
    EXPECT_NEAR(back.pitch, c.pitch, 1e-6f);
    EXPECT_EQ(back.loop, c.loop);
    EXPECT_EQ(back.play_on_start, c.play_on_start);
    EXPECT_EQ(back.spatial, c.spatial);
    EXPECT_NEAR(back.min_distance, c.min_distance, 1e-6f);
    EXPECT_NEAR(back.max_distance, c.max_distance, 1e-6f);

    // Defaults for a minimal JSON object.
    const auto def = ComponentIO<AudioSourceComponent>::from_json(nlohmann::json::object());
    EXPECT_TRUE(def.path.empty());
    EXPECT_NEAR(def.volume, 1.f, 1e-6f);
    EXPECT_FALSE(def.loop);
    EXPECT_FALSE(def.play_on_start);
    EXPECT_FALSE(def.spatial);
}

TEST(Audio, AudioSystemPlayOnStartAndControl) {
    Scene scene("audio_sys_test");
    register_builtin_components(scene.world());
    AudioSystem::register_components(scene.world());

    AudioEngine engine;
    ASSERT_TRUE(engine.init(true).ok());
    AudioSystem sys(engine, &scene);

    EntityId e = scene.world().create_entity("emitter");
    auto* t = scene.world().emplace<TransformComponent>(e);
    t->position = {2.f, 1.f, 0.f};
    auto* src = scene.world().emplace<AudioSourceComponent>(e);
    src->path = "assets/test.wav";
    src->loop = true;
    src->play_on_start = true;
    src->spatial = true;
    src->min_distance = 1.f;
    src->max_distance = 30.f;

    const f32 dt = 1.f / 60.f;
    sys.update(scene.world(), dt);
    EXPECT_TRUE(sys.source_playing(e));
    EXPECT_EQ(engine.active_sound_count(), 1u);

    // Moving the transform must not break positional sync.
    t->position = {5.f, 1.f, 0.f};
    sys.update(scene.world(), dt);
    EXPECT_TRUE(sys.source_playing(e));

    // Stop / restart on demand.
    sys.stop_source(e, scene.world());
    EXPECT_FALSE(sys.source_playing(e));
    EXPECT_EQ(engine.active_sound_count(), 0u);
    EXPECT_GT(sys.play_source(e, scene.world()), 0u);
    EXPECT_TRUE(sys.source_playing(e));

    // Destroying the entity stops its sound on the next update.
    scene.world().destroy_entity(e);
    sys.update(scene.world(), dt);
    EXPECT_EQ(engine.active_sound_count(), 0u);

    // play_on_start must not fire for a source without the flag.
    EntityId e2 = scene.world().create_entity("quiet");
    scene.world().emplace<AudioSourceComponent>(e2)->path = "assets/test.wav";
    sys.update(scene.world(), dt);
    EXPECT_FALSE(sys.source_playing(e2));
    EXPECT_EQ(engine.active_sound_count(), 0u);

    engine.shutdown();
}

TEST(Audio, AudioSourceViaAgentCommand) {
    Scene scene("audio_cmd_test");
    register_builtin_components(scene.world());
    AudioSystem::register_components(scene.world());
    AssetManager assets;
    CommandProcessor proc(scene, assets);

    const auto created = proc.execute({{"cmd", "create_entity"}, {"name", "Speaker"}});
    ASSERT_TRUE(created.ok) << created.message;
    const EntityId e = created.data["entity"].get<EntityId>();

    const auto r = proc.execute({{"cmd", "add_component"},
                                 {"entity", e},
                                 {"component", "AudioSource"},
                                 {"props", {{"path", "assets/test.wav"},
                                            {"volume", 0.5},
                                            {"loop", true},
                                            {"play_on_start", true},
                                            {"spatial", true},
                                            {"max_distance", 12}}}});
    ASSERT_TRUE(r.ok) << r.message;

    const auto* src = scene.world().get<AudioSourceComponent>(e);
    ASSERT_NE(src, nullptr);
    EXPECT_EQ(src->path, "assets/test.wav");
    EXPECT_NEAR(src->volume, 0.5f, 1e-6f);
    EXPECT_TRUE(src->loop);
    EXPECT_TRUE(src->play_on_start);
    EXPECT_TRUE(src->spatial);
    EXPECT_NEAR(src->max_distance, 12.f, 1e-6f);

    // And it shows up in get_entity serialization.
    const auto g = proc.execute({{"cmd", "get_entity"}, {"entity", e}});
    ASSERT_TRUE(g.ok) << g.message;
    bool found = false;
    for (const auto& c : g.data["components"]) {
        if (c["type"] == "AudioSource") {
            found = true;
            EXPECT_EQ(c["data"]["path"], "assets/test.wav");
        }
    }
    EXPECT_TRUE(found);
}

namespace {

// 42-byte FLAC whose STREAMINFO claims 2^36 total samples: a decode-length
// allocation bomb if the claimed length is trusted.
std::vector<u8> make_flac_bomb() {
    std::vector<u8> f = {'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, 0x22};
    f.insert(f.end(), {0x10, 0x00, 0x10, 0x00, 0, 0, 0, 0, 0, 0}); // block/frame sizes
    const unsigned long long v =
        (44100ull << 44) | (0ull << 41) | (15ull << 36) | ((1ull << 36) - 1);
    for (int i = 7; i >= 0; --i) f.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
    f.insert(f.end(), 16, 0); // md5
    return f;
}

} // namespace

TEST(AudioDecodeSecurity, GarbageMemoryFails) {
    std::vector<u8> garbage(512);
    for (size_t i = 0; i < garbage.size(); ++i) garbage[i] = static_cast<u8>(i * 37 + 11);
    DecodedAudio out;
    EXPECT_FALSE(decode_audio_memory(garbage.data(), garbage.size(), out));
}

TEST(AudioDecodeSecurity, EmptyMemoryFails) {
    const u8 dummy = 0;
    DecodedAudio out;
    EXPECT_FALSE(decode_audio_memory(&dummy, 0, out));
}

TEST(AudioDecodeSecurity, TruncatedWavDecodesConsistently) {
    auto wav = make_wav();
    wav.resize(60); // header claims 800 samples, only ~16 payload bytes exist
    DecodedAudio out;
    const bool ok = decode_audio_memory(wav.data(), wav.size(), out);
    if (ok) {
        EXPECT_GT(out.frames, 0u);
        EXPECT_LE(out.frames, 800u);
        EXPECT_EQ(out.samples.size(), static_cast<size_t>(out.frames) * out.channels);
    }
}

TEST(AudioDecodeSecurity, FlacHugeClaimedLengthRejected) {
    const auto bomb = make_flac_bomb();
    DecodedAudio out;
    EXPECT_FALSE(decode_audio_memory(bomb.data(), bomb.size(), out));
}

TEST(AudioDecodeSecurity, PlayMemoryGarbageFailsCleanly) {
    AudioEngine engine;
    ASSERT_TRUE(engine.init(true).ok());
    const std::vector<u8> garbage(256, 0xCD);
    auto r = engine.play_memory(garbage.data(), garbage.size());
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(r.error().empty());
    EXPECT_EQ(engine.active_sound_count(), 0u);
    // Engine still usable afterwards.
    const auto wav = make_wav();
    auto ok = engine.play_memory(wav.data(), wav.size());
    ASSERT_TRUE(ok.ok()) << ok.error();
    engine.shutdown();
}
