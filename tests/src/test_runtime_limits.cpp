#include <gtest/gtest.h>

#include "ignis/core/pak.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/wait.h>

#ifndef IGNIS_RUNTIME_EXE
#define IGNIS_RUNTIME_EXE ""
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

std::string q(const std::string& p) { return "'" + p + "'"; }

std::string test_scene() { return std::string(IGNIS_SCENES_DIR) + "/test_scene.scene.json"; }

} // namespace

TEST(RuntimeLimits, InvalidExecutionParametersRejected) {
    if (std::string(IGNIS_RUNTIME_EXE).empty()) GTEST_SKIP() << "runtime not built";
    const std::string exe = IGNIS_RUNTIME_EXE;
    const std::string scene = q(test_scene());
    // Negative / non-numeric / out-of-range frame counts.
    EXPECT_NE(run(exe + " --headless --frames -1 " + scene), 0);
    EXPECT_NE(run(exe + " --headless --frames abc " + scene), 0);
    EXPECT_NE(run(exe + " --headless --frames 99999999999 " + scene), 0); // stoi out_of_range
    EXPECT_NE(run(exe + " --headless --frames 20000000 " + scene), 0);     // above max
    // Non-finite, non-positive or huge time steps.
    EXPECT_NE(run(exe + " --headless --dt nan " + scene), 0);
    EXPECT_NE(run(exe + " --headless --dt inf " + scene), 0);
    EXPECT_NE(run(exe + " --headless --dt 0 " + scene), 0);
    EXPECT_NE(run(exe + " --headless --dt -1 " + scene), 0);
    EXPECT_NE(run(exe + " --headless --dt abc " + scene), 0);
    EXPECT_NE(run(exe + " --headless --dt 1e30 " + scene), 0);
}

TEST(RuntimeLimits, ValidParametersStillRun) {
    if (std::string(IGNIS_RUNTIME_EXE).empty()) GTEST_SKIP() << "runtime not built";
    const int rc = run(std::string(IGNIS_RUNTIME_EXE) +
                       " --headless --frames 2 --dt 0.016 " + q(test_scene()) + " > /dev/null 2>&1");
    EXPECT_EQ(rc, 0);
}

TEST(RuntimeLimits, MalformedPakRejectedAtStartup) {
    if (std::string(IGNIS_RUNTIME_EXE).empty()) GTEST_SKIP() << "runtime not built";
    const auto path = fs::temp_directory_path() / "ignis_bad_runtime.ignispak";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write("IGPK", 4);
        const u32 huge = 0xFFFFFFFFu;
        f.write(reinterpret_cast<const char*>(&huge), 4);
    }
    const int rc = run(std::string(IGNIS_RUNTIME_EXE) + " --headless --pak " + q(path.string()));
    EXPECT_NE(rc, 0);
    fs::remove(path);
}
