#include <gtest/gtest.h>

#include "ignis/core/pak.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <sys/wait.h>

#ifndef IGNIS_EXPORTER_EXE
#define IGNIS_EXPORTER_EXE ""
#endif

using namespace ignis;

namespace {

namespace fs = std::filesystem;

int run(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    if (rc == -1) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -1;
}

std::string q(const fs::path& p) { return "'" + p.string() + "'"; }

struct ExportSandbox {
    fs::path tmp;
    fs::path proj;
    fs::path dist;
    ExportSandbox() {
        tmp = fs::temp_directory_path() / "ignis_export_sec";
        fs::remove_all(tmp);
        proj = tmp / "proj";
        dist = proj / "dist";
        fs::create_directories(proj / "assets");
        std::ofstream(proj / "scene.json") << "{\"name\":\"t\",\"entities\":[]}";
        std::ofstream(proj / "assets" / "a.txt") << "hello";
        std::ofstream(tmp / "secret.txt") << "secret";
        fs::create_directories(tmp / "outside");
        std::ofstream(tmp / "outside" / "x.txt") << "outside";
    }
    ~ExportSandbox() { std::error_code ec; fs::remove_all(tmp, ec); }
};

} // namespace

TEST(ExportSecurity, SymlinkEscapeAndExternalIncludesExcluded) {
    if (std::string(IGNIS_EXPORTER_EXE).empty()) GTEST_SKIP() << "exporter not built";
    ExportSandbox sb;
    std::error_code ec;
    fs::create_symlink(sb.tmp / "secret.txt", sb.proj / "assets" / "evil_link.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    const int rc = run(std::string(IGNIS_EXPORTER_EXE) +
                       " --scene " + q(sb.proj / "scene.json") +
                       " --root " + q(sb.proj) +
                       " --include " + q(sb.proj / "assets") +
                       " --include " + q(sb.tmp / "outside") +
                       " --out " + q(sb.dist));
    ASSERT_EQ(rc, 0);

    PakReader r;
    ASSERT_TRUE(r.open((sb.dist / "game.ignispak").string()).ok());
    EXPECT_TRUE(r.has("scene.json"));
    EXPECT_TRUE(r.has("assets/a.txt"));
    EXPECT_FALSE(r.has("assets/evil_link.txt")); // symlink target outside root
    EXPECT_FALSE(r.has("x.txt"));                // include outside root
    // Every entry must still read back (integrity).
    for (const auto& name : r.list()) EXPECT_TRUE(r.read(name).ok()) << name;

    // Manifest lists exactly the packaged files.
    std::ifstream mf(sb.dist / "manifest.json");
    const std::string manifest((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
    EXPECT_NE(manifest.find("assets/a.txt"), std::string::npos);
    EXPECT_EQ(manifest.find("evil_link.txt"), std::string::npos);
}

TEST(ExportSecurity, MissingSceneFailsCleanly) {
    if (std::string(IGNIS_EXPORTER_EXE).empty()) GTEST_SKIP() << "exporter not built";
    ExportSandbox sb;
    const int rc = run(std::string(IGNIS_EXPORTER_EXE) +
                       " --scene " + q(sb.proj / "nope.json") +
                       " --out " + q(sb.dist));
    EXPECT_NE(rc, 0);
}

TEST(ExportSecurity, MissingArgumentValueExitsWithUsageError) {
    if (std::string(IGNIS_EXPORTER_EXE).empty()) GTEST_SKIP() << "exporter not built";
    const int rc = run(std::string(IGNIS_EXPORTER_EXE) + " --scene"); // trailing flag, no value
    EXPECT_EQ(rc, 2);
}

TEST(ExportSecurity, SceneRuntimeAndOutMustBeInsideRoot) {
    if (std::string(IGNIS_EXPORTER_EXE).empty()) GTEST_SKIP() << "exporter not built";
    ExportSandbox sb;
    const std::string exe = IGNIS_EXPORTER_EXE;

    // --scene outside the export root: rejected.
    int rc = run(exe + " --scene " + q(sb.tmp / "secret.txt") + " --root " + q(sb.proj) + " --out " +
                 q(sb.dist));
    EXPECT_NE(rc, 0) << "scene outside root must be rejected";

    // --out outside the export root: rejected.
    rc = run(exe + " --scene " + q(sb.proj / "scene.json") + " --root " + q(sb.proj) + " --out " +
             q(sb.tmp / "outside_dist"));
    EXPECT_NE(rc, 0) << "out dir outside root must be rejected";

    // Normal export where the scene is also covered by --include: the
    // duplicate "scene.json" entry must be skipped, not abort the export.
    rc = run(exe + " --scene " + q(sb.proj / "scene.json") + " --root " + q(sb.proj) + " --include " +
             q(sb.proj) + " --out " + q(sb.dist));
    ASSERT_EQ(rc, 0);
    PakReader r;
    ASSERT_TRUE(r.open((sb.dist / "game.ignispak").string()).ok());
    EXPECT_TRUE(r.has("scene.json"));
    EXPECT_TRUE(r.has("assets/a.txt"));
}
