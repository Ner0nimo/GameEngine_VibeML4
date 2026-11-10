#include <gtest/gtest.h>

#include "ignis/core/pak.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

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

namespace {

namespace fs = std::filesystem;

void write_raw(const fs::path& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Builds a raw .ignispak file from an index JSON string and a blob.
void write_pak(const fs::path& path, const std::string& index_json, const std::string& blob) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write("IGPK", 4);
    const u32 sz = static_cast<u32>(index_json.size());
    f.write(reinterpret_cast<const char*>(&sz), 4);
    f.write(index_json.data(), static_cast<std::streamsize>(index_json.size()));
    f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
}

std::string entry_json(const std::string& name, u64 off, u64 size, const std::string& hash) {
    nlohmann::json j;
    j["files"][name] = {{"offset", off}, {"size", size}, {"sha256", hash}};
    return j.dump();
}

std::string hash_of(const std::string& s) {
    return sha256_hex(reinterpret_cast<const u8*>(s.data()), s.size());
}

} // namespace

TEST(PakSecurity, BadMagicRejected) {
    const auto path = fs::temp_directory_path() / "ignis_sec_badmagic.ignispak";
    write_raw(path, std::string("XXXX", 4) + std::string(16, '\0'));
    PakReader r;
    EXPECT_FALSE(r.open(path.string()).ok());
    fs::remove(path);
}

TEST(PakSecurity, TruncatedHeaderRejected) {
    const auto path = fs::temp_directory_path() / "ignis_sec_trunc.ignispak";
    write_raw(path, "IGP"); // 3 bytes only
    PakReader r;
    EXPECT_FALSE(r.open(path.string()).ok());
    fs::remove(path);
}

TEST(PakSecurity, HugeIndexSizeRejectedWithoutAllocation) {
    const auto path = fs::temp_directory_path() / "ignis_sec_hugeidx.ignispak";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write("IGPK", 4);
    const u32 huge = 0xFFFFFFFFu;
    f.write(reinterpret_cast<const char*>(&huge), 4);
    f.close();
    PakReader r;
    EXPECT_FALSE(r.open(path.string()).ok()); // must fail fast, not allocate 4 GiB
    fs::remove(path);
}

TEST(PakSecurity, IndexBeyondEofRejected) {
    const auto path = fs::temp_directory_path() / "ignis_sec_idx_oob.ignispak";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write("IGPK", 4);
    const u32 sz = 1000;
    f.write(reinterpret_cast<const char*>(&sz), 4);
    f.close(); // no index bytes at all
    PakReader r;
    EXPECT_FALSE(r.open(path.string()).ok());
    fs::remove(path);
}

TEST(PakSecurity, EntryOutOfBoundsRejected) {
    const auto path = fs::temp_directory_path() / "ignis_sec_oob.ignispak";
    const std::string blob = "hello";
    const std::string idx = entry_json("a.txt", 1000000ull, 10, std::string(64, 'a'));
    write_pak(path, idx, blob);
    PakReader r;
    EXPECT_FALSE(r.open(path.string()).ok());
    fs::remove(path);
}

TEST(PakSecurity, EntryOverlappingHeaderRejected) {
    const auto path = fs::temp_directory_path() / "ignis_sec_hdr.ignispak";
    const std::string blob = "hello";
    const std::string idx = entry_json("a.txt", 4, 5, std::string(64, 'a')); // inside 8-byte header
    write_pak(path, idx, blob);
    PakReader r;
    EXPECT_FALSE(r.open(path.string()).ok());
    fs::remove(path);
}

TEST(PakSecurity, OverlappingEntriesRejected) {
    const auto path = fs::temp_directory_path() / "ignis_sec_overlap.ignispak";
    const std::string blob = "0123456789";
    nlohmann::json idx;
    const u64 base = 8 + 100; // just needs to be past the (fake) index; use real size below
    (void)base;
    // Build with correct self-referential offset: compute after dump once.
    idx["files"]["a.txt"] = {{"offset", 0}, {"size", 5}, {"sha256", std::string(64, 'a')}};
    idx["files"]["b.txt"] = {{"offset", 2}, {"size", 5}, {"sha256", std::string(64, 'b')}};
    const std::string idx_str = idx.dump();
    const u64 off = 8 + idx_str.size();
    idx["files"]["a.txt"]["offset"] = off;
    idx["files"]["b.txt"]["offset"] = off + 2; // overlaps a.txt
    write_pak(path, idx.dump(), blob);
    PakReader r;
    EXPECT_FALSE(r.open(path.string()).ok());
    fs::remove(path);
}

TEST(PakSecurity, UnsafeEntryPathsRejected) {
    for (const char* name : {"../evil.txt", "/abs/evil.txt", "a/../../evil.txt"}) {
        const auto path = fs::temp_directory_path() / "ignis_sec_path.ignispak";
        const std::string blob = "x";
        nlohmann::json idx;
        idx["files"][name] = {{"offset", 0}, {"size", 1}, {"sha256", hash_of(blob)}};
        const std::string idx_str = idx.dump();
        idx["files"][name]["offset"] = 8 + idx_str.size();
        write_pak(path, idx.dump(), blob);
        PakReader r;
        EXPECT_FALSE(r.open(path.string()).ok()) << "path: " << name;
        fs::remove(path);
    }
}

TEST(PakSecurity, MissingOrMalformedHashRejected) {
    const auto path = fs::temp_directory_path() / "ignis_sec_hash.ignispak";
    const std::string blob = "data";
    // No sha256 at all.
    nlohmann::json idx;
    idx["files"]["a.txt"] = {{"offset", 0}, {"size", 4}};
    std::string idx_str = idx.dump();
    idx["files"]["a.txt"]["offset"] = 8 + idx_str.size();
    write_pak(path, idx.dump(), blob);
    PakReader r;
    EXPECT_FALSE(r.open(path.string()).ok());
    // Malformed sha256.
    idx["files"]["a.txt"]["sha256"] = "not-a-hash";
    idx_str = idx.dump();
    idx["files"]["a.txt"]["offset"] = 8 + idx_str.size();
    write_pak(path, idx.dump(), blob);
    EXPECT_FALSE(r.open(path.string()).ok());
    fs::remove(path);
}

TEST(PakSecurity, DeeplyNestedIndexRejected) {
    const auto path = fs::temp_directory_path() / "ignis_sec_deep.ignispak";
    const std::string idx = std::string(100000, '[') + std::string(100000, ']');
    write_pak(path, idx, "");
    PakReader r;
    EXPECT_FALSE(r.open(path.string()).ok()); // bounded parse: fails, no stack overflow
    fs::remove(path);
}

TEST(PakSecurity, WriterRejectsDuplicateAndUnsafePaths) {
    PakWriter w;
    EXPECT_TRUE(w.add_file("a.txt", {'x'}).ok());
    EXPECT_FALSE(w.add_file("a.txt", {'y'}).ok());   // duplicate
    EXPECT_FALSE(w.add_file("", {'z'}).ok());         // empty
    EXPECT_FALSE(w.add_file("../up.txt", {'z'}).ok()); // traversal
    EXPECT_FALSE(w.add_file("/abs.txt", {'z'}).ok());  // absolute
}

TEST(PakSecurity, ValidPakStillOpens) {
    const auto path = fs::temp_directory_path() / "ignis_sec_valid.ignispak";
    PakWriter w;
    ASSERT_TRUE(w.add_file("a.txt", {'h', 'i'}).ok());
    ASSERT_TRUE(w.write(path.string()).ok());
    PakReader r;
    ASSERT_TRUE(r.open(path.string()).ok());
    auto out = r.read("a.txt");
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(std::string(out.value().begin(), out.value().end()), "hi");
    fs::remove(path);
}

TEST(PakSecurity, NonUnsignedOffsetSizeRejected) {
    // IGN-005: negative, float and string offset/size must be rejected before
    // any u64 conversion (is_number_unsigned), not silently converted.
    const auto path = fs::temp_directory_path() / "ignis_sec_int.ignispak";
    const std::string blob = "hello";
    const std::string hash(64, 'a');
    for (const char* bad : {"-5", "1.5", "\"8\"", "null", "true"}) {
        nlohmann::json idx;
        idx["files"]["a.txt"] = nlohmann::json::parse(
            std::string("{\"offset\":") + bad + ",\"size\":5,\"sha256\":\"" + hash + "\"}");
        write_pak(path, idx.dump(), blob);
        PakReader r;
        EXPECT_FALSE(r.open(path.string()).ok()) << "offset=" << bad << " must be rejected";
    }
    // Same for size.
    for (const char* bad : {"-1", "2.5", "\"3\""}) {
        nlohmann::json idx;
        idx["files"]["a.txt"] = nlohmann::json::parse(
            std::string("{\"offset\":100,\"size\":") + bad + ",\"sha256\":\"" + hash + "\"}");
        write_pak(path, idx.dump(), blob);
        PakReader r;
        EXPECT_FALSE(r.open(path.string()).ok()) << "size=" << bad << " must be rejected";
    }
    fs::remove(path);
}

TEST(PakSecurity, WriterResourceLimitsEnforced) {
    // IGN-003: oversized entries and oversized totals must abort cleanly,
    // and add_file_from_disk must check the file size BEFORE reading it.
    const auto path = fs::temp_directory_path() / "ignis_sec_big.bin";
    { std::ofstream f(path, std::ios::binary); f << std::string(1024, 'x'); }

    PakWriter w;
    w.limits.max_entry_bytes = 512;
    EXPECT_FALSE(w.add_file("big.bin", std::vector<u8>(1024, 'x')).ok()) << "oversized entry";
    EXPECT_FALSE(w.add_file_from_disk("big.bin", path.string()).ok())
        << "oversized disk file must be rejected before reading";

    PakWriter w2;
    w2.limits.max_total_bytes = 100;
    EXPECT_TRUE(w2.add_file("a", std::vector<u8>(60, 'a')).ok());
    EXPECT_FALSE(w2.add_file("b", std::vector<u8>(60, 'b')).ok()) << "total over budget";

    PakWriter w3;
    w3.limits.max_entries = 2;
    EXPECT_TRUE(w3.add_file("a", {'x'}).ok());
    EXPECT_TRUE(w3.add_file("b", {'y'}).ok());
    EXPECT_FALSE(w3.add_file("c", {'z'}).ok()) << "entry count cap";
    fs::remove(path);
}
