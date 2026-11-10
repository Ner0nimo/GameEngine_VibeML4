#pragma once

#include "ignis/core/result.h"
#include "ignis/core/types.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ignis {

// Package format "IGPK" used for exported games:
//
//   offset 0:  magic "IGPK"            (4 bytes)
//   offset 4:  u32le index_json_size
//   offset 8:  index JSON:
//              {"files": {"<path>": {"offset": u64, "size": u64, "sha256": "<hex>"}}}
//   then:      blob data, concatenated in index order
//
// All multi-byte integers are little-endian.

// Policy limits enforced by PakReader::open when validating an untrusted
// package index. Values are starting points, not universal requirements.
struct PakLimits {
    u64 max_index_bytes = 16ull << 20;    // 16 MiB
    u64 max_entry_bytes = 256ull << 20;   // 256 MiB per asset
    u64 max_total_bytes = 2ull << 30;      // 2 GiB total unpacked
    u64 max_entries = 100000;
    u64 max_path_bytes = 1024;
};

class PakWriter {
public:
    // IGN-003: resource limits enforced on every added entry (per-file size,
    // total bytes, entry count) so an oversized project aborts cleanly instead
    // of exhausting memory during export.
    PakLimits limits;

    Result<void> add_file(const std::string& pak_path, const std::vector<u8>& data);
    Result<void> add_file_from_disk(const std::string& pak_path, const std::string& disk_path);
    Result<void> write(const std::string& out_path) const;

private:
    struct Entry { std::string pak_path; std::vector<u8> data; };
    std::vector<Entry> entries_;
    std::unordered_set<std::string> paths_;
    u64 total_bytes_ = 0;
};

class PakReader {
public:
    Result<void> open(const std::string& path, const PakLimits& limits = {});
    bool is_open() const { return is_open_; }
    bool has(const std::string& pak_path) const;
    Result<std::vector<u8>> read(const std::string& pak_path) const;
    std::vector<std::string> list() const;

private:
    struct Entry { u64 offset = 0; u64 size = 0; std::string sha256; };
    bool is_open_ = false;
    std::string file_path_;
    std::unordered_map<std::string, Entry> index_;
};

// Compact SHA-256 (hex string, lowercase).
std::string sha256_hex(const u8* data, size_t size);
inline std::string sha256_hex(const std::vector<u8>& v) {
    return sha256_hex(v.data(), v.size());
}

} // namespace ignis
