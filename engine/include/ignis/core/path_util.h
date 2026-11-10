#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace ignis {

// Lexical safety check for a relative resource path (pak entry names, asset
// paths, project-relative command paths): rejects empty paths, absolute paths,
// parent-directory traversal and Windows-style separators.
inline bool is_unsafe_resource_path(const std::string& p) {
    if (p.empty()) return true;
    if (p.find('\\') != std::string::npos) return true;
    const std::filesystem::path fp(p);
    if (fp.is_absolute()) return true;
    for (const auto& seg : fp) {
        if (seg == "..") return true;
    }
    return false;
}

// True when p resolves (following symlinks, at the filesystem boundary) to a
// location inside root. Uses weakly_canonical so non-existent leaf components
// (e.g. a save target) are still checked against the canonicalized prefix.
inline bool is_path_within_root(const std::filesystem::path& root,
                                const std::filesystem::path& p) {
    std::error_code ec;
    const auto r = std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    const auto c = std::filesystem::weakly_canonical(p, ec);
    if (ec) return false;
    const auto [rend, cend] = std::mismatch(r.begin(), r.end(), c.begin(), c.end());
    (void)cend;
    return rend == r.end();
}

} // namespace ignis
