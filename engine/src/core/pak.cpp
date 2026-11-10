#include "ignis/core/pak.h"

#include "ignis/core/json_parse.h"
#include "ignis/core/path_util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace ignis {

namespace {

// --- Compact SHA-256 (FIPS 180-4) ------------------------------------------

constexpr std::array<u32, 64> K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline u32 rotr(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

std::array<u32, 8> sha256_compute(const u8* data, size_t size) {
    std::array<u32, 8> h = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    const size_t total = size + 1 + 63 + 8; // padding worst case
    std::vector<u8> msg(data, data + size);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    const u64 bit_len = static_cast<u64>(size) * 8;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<u8>((bit_len >> (i * 8)) & 0xff));
    (void)total;

    for (size_t off = 0; off < msg.size(); off += 64) {
        std::array<u32, 64> w{};
        for (size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<u32>(msg[off + i * 4]) << 24) |
                   (static_cast<u32>(msg[off + i * 4 + 1]) << 16) |
                   (static_cast<u32>(msg[off + i * 4 + 2]) << 8) |
                   (static_cast<u32>(msg[off + i * 4 + 3]));
        }
        for (size_t i = 16; i < 64; ++i) {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3];
        u32 e = h[4], f = h[5], g = h[6], hh = h[7];
        for (size_t i = 0; i < 64; ++i) {
            const u32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const u32 ch = (e & f) ^ (~e & g);
            const u32 t1 = hh + S1 + ch + K[i] + w[i];
            const u32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const u32 maj = (a & b) ^ (a & c) ^ (b & c);
            const u32 t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    return h;
}

} // namespace

std::string sha256_hex(const u8* data, size_t size) {
    const auto h = sha256_compute(data, size);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (u32 word : h) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out.push_back(hex[(word >> shift) & 0xf]);
        }
    }
    return out;
}

// --- PakWriter ---------------------------------------------------------------

Result<void> PakWriter::add_file(const std::string& pak_path, const std::vector<u8>& data) {
    if (is_unsafe_resource_path(pak_path))
        return Result<void>::failure("PakWriter: invalid pak path '" + pak_path + "'");
    if (paths_.contains(pak_path))
        return Result<void>::failure("PakWriter: duplicate pak path '" + pak_path + "'");
    constexpr u64 kMaxEntries = 100000;
    if (entries_.size() >= kMaxEntries)
        return Result<void>::failure("PakWriter: too many entries (max " +
                                     std::to_string(kMaxEntries) + ")");
    paths_.insert(pak_path);
    entries_.push_back({pak_path, data});
    return Result<void>::success();
}

Result<void> PakWriter::add_file_from_disk(const std::string& pak_path, const std::string& disk_path) {
    std::ifstream f(disk_path, std::ios::binary);
    if (!f) return Result<void>::failure("PakWriter: cannot read '" + disk_path + "'");
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    return add_file(pak_path, std::vector<u8>(s.begin(), s.end()));
}

Result<void> PakWriter::write(const std::string& out_path) const {
    nlohmann::json index = nlohmann::json::object();
    index["files"] = nlohmann::json::object();

    // Compute the index iteratively: stored offsets are absolute file offsets
    // (8-byte header + index), so the index size feeds back into the offsets.
    u64 blob_base = 0;
    for (int pass = 0; pass < 4; ++pass) {
        nlohmann::json files = nlohmann::json::object();
        u64 off = blob_base;
        for (const auto& e : entries_) {
            files[e.pak_path] = {
                {"offset", off},
                {"size", e.data.size()},
                {"sha256", sha256_hex(e.data)},
            };
            off += e.data.size();
        }
        index["files"] = std::move(files);
        const std::string s = index.dump();
        const u64 new_base = 8 + static_cast<u64>(s.size());
        if (new_base == blob_base) break;
        blob_base = new_base;
    }

    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) return Result<void>::failure("PakWriter: cannot write '" + out_path + "'");
    f.write("IGPK", 4);
    const std::string index_str = index.dump();
    const u32 index_size = static_cast<u32>(index_str.size());
    f.write(reinterpret_cast<const char*>(&index_size), 4);
    f.write(index_str.data(), static_cast<std::streamsize>(index_str.size()));
    for (const auto& e : entries_) {
        f.write(reinterpret_cast<const char*>(e.data.data()),
                static_cast<std::streamsize>(e.data.size()));
    }
    if (!f.good()) return Result<void>::failure("PakWriter: write failed for '" + out_path + "'");
    return Result<void>::success();
}

// --- PakReader ---------------------------------------------------------------

namespace {

bool is_hex_sha256(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

} // namespace

Result<void> PakReader::open(const std::string& path, const PakLimits& limits) {
    is_open_ = false;
    index_.clear();
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return Result<void>::failure("cannot open pak '" + path + "'");
    const auto file_size_off = f.tellg();
    if (file_size_off < 0) return Result<void>::failure("cannot stat pak '" + path + "'");
    const u64 file_size = static_cast<u64>(file_size_off);
    if (file_size < 8)
        return Result<void>::failure("truncated pak header in '" + path + "'");
    f.seekg(0, std::ios::beg);
    char magic[4] = {};
    f.read(magic, 4);
    if (!f || std::memcmp(magic, "IGPK", 4) != 0)
        return Result<void>::failure("bad magic in pak '" + path + "' (not IGPK)");
    u32 index_size = 0;
    f.read(reinterpret_cast<char*>(&index_size), 4);
    if (!f) return Result<void>::failure("truncated pak header in '" + path + "'");
    if (static_cast<u64>(index_size) > limits.max_index_bytes)
        return Result<void>::failure("pak index too large in '" + path + "'");
    if (static_cast<u64>(index_size) > file_size - 8)
        return Result<void>::failure("truncated pak index in '" + path + "'");
    std::string index_str(index_size, '\0');
    f.read(index_str.data(), static_cast<std::streamsize>(index_size));
    if (!f) return Result<void>::failure("truncated pak index in '" + path + "'");

    auto parsed = parse_json_bounded(index_str);
    if (!parsed.ok())
        return Result<void>::failure("bad index in '" + path + "': " + parsed.error());
    const nlohmann::json& j = parsed.value();
    if (!j.is_object() || !j.contains("files") || !j["files"].is_object())
        return Result<void>::failure("bad index in '" + path + "': missing 'files' object");
    if (j["files"].size() > limits.max_entries)
        return Result<void>::failure("too many entries in '" + path + "'");

    struct Range { u64 off; u64 size; };
    std::vector<Range> ranges;
    ranges.reserve(j["files"].size());
    u64 total_bytes = 0;
    for (const auto& [name, meta] : j["files"].items()) {
        if (is_unsafe_resource_path(name) || name.size() > limits.max_path_bytes)
            return Result<void>::failure("bad entry path '" + name + "' in '" + path + "'");
        if (!meta.is_object() || !meta.contains("offset") || !meta.contains("size"))
            return Result<void>::failure("entry '" + name + "' missing offset/size in '" + path + "'");
        if (!meta["offset"].is_number() || !meta["size"].is_number())
            return Result<void>::failure("entry '" + name + "' has non-numeric offset/size in '" + path + "'");
        const u64 off = meta["offset"].get<u64>();
        const u64 size = meta["size"].get<u64>();
        if (off < 8 + static_cast<u64>(index_size))
            return Result<void>::failure("entry '" + name + "' overlaps pak header/index in '" + path + "'");
        if (size > limits.max_entry_bytes)
            return Result<void>::failure("entry '" + name + "' too large in '" + path + "'");
        if (off > file_size || size > file_size - off)
            return Result<void>::failure("entry '" + name + "' out of file bounds in '" + path + "'");
        total_bytes += size;
        if (total_bytes > limits.max_total_bytes)
            return Result<void>::failure("pak total payload too large in '" + path + "'");
        const std::string hash = meta.value("sha256", "");
        if (!is_hex_sha256(hash))
            return Result<void>::failure("entry '" + name + "' missing/malformed sha256 in '" + path + "'");
        ranges.push_back({off, size});
        index_[name] = Entry{off, size, hash};
    }
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.off < b.off; });
    for (size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].off < ranges[i - 1].off + ranges[i - 1].size)
            return Result<void>::failure("overlapping entries in '" + path + "'");
    }
    file_path_ = path;
    is_open_ = true;
    return Result<void>::success();
}

bool PakReader::has(const std::string& pak_path) const {
    return is_open_ && index_.contains(pak_path);
}

Result<std::vector<u8>> PakReader::read(const std::string& pak_path) const {
    if (!is_open_) return Result<std::vector<u8>>::failure("pak not open");
    auto it = index_.find(pak_path);
    if (it == index_.end())
        return Result<std::vector<u8>>::failure("no entry '" + pak_path + "' in pak");
    std::ifstream f(file_path_, std::ios::binary);
    if (!f) return Result<std::vector<u8>>::failure("cannot reopen pak file '" + file_path_ + "'");
    f.seekg(static_cast<std::streamoff>(it->second.offset));
    std::vector<u8> out(it->second.size);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!f) return Result<std::vector<u8>>::failure("short read for '" + pak_path + "'");
    if (sha256_hex(out) != it->second.sha256)
        return Result<std::vector<u8>>::failure("sha256 mismatch for '" + pak_path + "'");
    return Result<std::vector<u8>>::success(std::move(out));
}

std::vector<std::string> PakReader::list() const {
    std::vector<std::string> out;
    out.reserve(index_.size());
    for (const auto& [n, _] : index_) out.push_back(n);
    return out;
}

} // namespace ignis
