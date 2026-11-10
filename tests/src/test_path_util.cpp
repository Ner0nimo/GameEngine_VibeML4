#include <gtest/gtest.h>

#include "ignis/core/path_util.h"

#include <filesystem>
#include <fstream>
#include <system_error>

using namespace ignis;

namespace fs = std::filesystem;

TEST(PathUtil, UnsafeResourcePathsRejected) {
    EXPECT_TRUE(is_unsafe_resource_path(""));
    EXPECT_TRUE(is_unsafe_resource_path("/etc/passwd"));
    EXPECT_TRUE(is_unsafe_resource_path("//server/share"));
    EXPECT_TRUE(is_unsafe_resource_path("../secret.txt"));
    EXPECT_TRUE(is_unsafe_resource_path("a/../../b"));
    EXPECT_TRUE(is_unsafe_resource_path(".."));
    EXPECT_TRUE(is_unsafe_resource_path("a/.."));
    EXPECT_TRUE(is_unsafe_resource_path("dir\\file.txt"));
    EXPECT_TRUE(is_unsafe_resource_path("C:\\Windows\\system32"));
}

TEST(PathUtil, SafeResourcePathsAccepted) {
    EXPECT_FALSE(is_unsafe_resource_path("texture.png"));
    EXPECT_FALSE(is_unsafe_resource_path("assets/models/cube.gltf"));
    EXPECT_FALSE(is_unsafe_resource_path("./local.txt"));
    EXPECT_FALSE(is_unsafe_resource_path("dir/"));
}

TEST(PathUtil, WithinRootChecks) {
    const fs::path tmp = fs::temp_directory_path() / "ignis_pathutil_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    const fs::path root = tmp / "proj";
    fs::create_directories(root / "sub", ec);
    { std::ofstream(root / "file.txt") << "x"; }
    { std::ofstream(tmp / "secret.txt") << "secret"; }
    fs::create_symlink(tmp / "secret.txt", root / "link.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    EXPECT_TRUE(is_path_within_root(root, root));
    EXPECT_TRUE(is_path_within_root(root, root / "file.txt"));
    EXPECT_TRUE(is_path_within_root(root, root / "sub" / ".." / "file.txt"));
    // Non-existent leaf inside root is still inside (e.g. a save target).
    EXPECT_TRUE(is_path_within_root(root, root / "missing.txt"));
    EXPECT_FALSE(is_path_within_root(root, tmp / "secret.txt"));
    EXPECT_FALSE(is_path_within_root(root, root / ".." / "secret.txt"));
    // A symlink inside root pointing outside resolves outside -> rejected.
    EXPECT_FALSE(is_path_within_root(root, root / "link.txt"));
    EXPECT_FALSE(is_path_within_root(root, fs::path("/etc/passwd")));

    fs::remove_all(tmp, ec);
}
