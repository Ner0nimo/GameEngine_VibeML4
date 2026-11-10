#include <gtest/gtest.h>

#include "ignis/core/pak.h"

#include <filesystem>
#include <fstream>

using namespace ignis;

TEST(Pak, Sha256KnownVector) {
    // FIPS 180-4 test vector.
    EXPECT_EQ(sha256_hex(reinterpret_cast<const u8*>("abc"), 3),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_hex(reinterpret_cast<const u8*>(""), 0),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Pak, WriteReadRoundtrip) {
    const auto path = std::filesystem::temp_directory_path() / "ignis_test.ignispak";
    PakWriter w;
    w.add_file("scene.json", {'{', '}', '\n'});
    w.add_file("assets/a.bin", std::vector<u8>(1000, 0xAB));
    w.add_file("scripts/x.lua", std::vector<u8>{'p', 'r', 'i', 'n', 't', '1'});
    ASSERT_TRUE(w.write(path.string()).ok());

    PakReader r;
    ASSERT_TRUE(r.open(path.string()).ok());
    EXPECT_TRUE(r.has("scene.json"));
    EXPECT_FALSE(r.has("nope"));
    EXPECT_EQ(r.list().size(), 3u);

    auto out = r.read("scene.json");
    ASSERT_TRUE(out.ok()) << out.error();
    EXPECT_EQ(std::string(out.value().begin(), out.value().end()), "{}\n");
    out = r.read("assets/a.bin");
    ASSERT_TRUE(out.ok()) << out.error();
    EXPECT_EQ(out.value().size(), 1000u);
    EXPECT_EQ(out.value()[0], 0xAB);

    std::filesystem::remove(path);
}

TEST(Pak, CorruptionDetected) {
    const auto path = std::filesystem::temp_directory_path() / "ignis_test_corrupt.ignispak";
    PakWriter w;
    w.add_file("data.bin", std::vector<u8>(64, 0x11));
    ASSERT_TRUE(w.write(path.string()).ok());

    // Flip a byte inside the blob region (after header+index: 8 + index size).
    {
        std::fstream f(path.string(), std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(-1, std::ios::end);
        char c = 0;
        f.read(&c, 1);
        f.seekp(-1, std::ios::end);
        c = static_cast<char>(static_cast<unsigned char>(c) ^ 0xFFu);
        f.write(&c, 1);
    }
    PakReader r;
    ASSERT_TRUE(r.open(path.string()).ok());
    EXPECT_FALSE(r.read("data.bin").ok()); // sha256 mismatch must fail
    std::filesystem::remove(path);
}
