#pragma once

#include "ignis/core/types.h"

#include <string>
#include <unordered_map>
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

class PakWriter {
public:
    void add_file(const std::string& pak_path, const std::vector<u8>& data);
    void add_file_from_disk(const std::string& pak_path, const std::string& disk_path);
    bool write(const std::string& out_path) const;

private:
    struct Entry { std::string pak_path; std::vector<u8> data; };
    std::vector<Entry> entries_;
};

class PakReader {
public:
    bool open(const std::string& path);
    bool is_open() const { return is_open_; }
    bool has(const std::string& pak_path) const;
    bool read(const std::string& pak_path, std::vector<u8>& out) const;
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
