#include <gtest/gtest.h>

#include "ignis/audio/audio_engine.h"

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
    std::string err;
    ASSERT_TRUE(engine.init(true, err)) << err; // null device (headless)
    EXPECT_TRUE(engine.is_null_device());

    // Play the generated wav; with no device the handle must still be valid.
    const auto path = std::filesystem::temp_directory_path() / "ignis_play_test.wav";
    {
        const auto wav = make_wav();
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    }
    const u32 id = engine.play(path.string(), 0.5f, false);
    EXPECT_GT(id, 0u);
    EXPECT_EQ(engine.active_sound_count(), 1u);
    engine.stop(id);
    EXPECT_EQ(engine.active_sound_count(), 0u);

    // Missing file -> id 0.
    EXPECT_EQ(engine.play("/nonexistent.wav"), 0u);

    engine.shutdown();
    EXPECT_FALSE(engine.is_initialized());
    std::filesystem::remove(path);
}
