#include <gtest/gtest.h>

#include "ignis/assets/asset_manager.h"
#include "ignis/core/pak.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace ignis;

namespace {

// Minimal valid .hdr (Radiance RGBE, flat scanlines, width < 8 so no RLE needed):
// 4x4 pixels, all RGBE = (128,128,128,129) -> rgb = 0.5 each.
std::vector<u8> make_hdr_4x4() {
    std::vector<u8> out;
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 4\n";
    out.insert(out.end(), header.begin(), header.end());
    for (int i = 0; i < 16; ++i) {
        out.push_back(128);
        out.push_back(128);
        out.push_back(128);
        out.push_back(136); // exponent 136 -> stb value = byte * 2^(136-136) = 128
    }
    return out;
}

// Minimal .gltf with a data-URI buffer: one triangle (3 positions + 3 indices).
std::string make_gltf_triangle() {
    // positions: 3 * vec3 float32; indices: 3 * uint16
    const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint16_t idx[3] = {0, 1, 2};
    std::vector<u8> buf(sizeof(pos) + sizeof(idx));
    std::memcpy(buf.data(), pos, sizeof(pos));
    std::memcpy(buf.data() + sizeof(pos), idx, sizeof(idx));
    // base64
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    for (size_t i = 0; i < buf.size(); i += 3) {
        u32 v = static_cast<u32>(buf[i]) << 16;
        if (i + 1 < buf.size()) v |= static_cast<u32>(buf[i + 1]) << 8;
        if (i + 2 < buf.size()) v |= static_cast<u32>(buf[i + 2]);
        enc.push_back(b64[(v >> 18) & 63]);
        enc.push_back(b64[(v >> 12) & 63]);
        enc.push_back(i + 1 < buf.size() ? b64[(v >> 6) & 63] : '=');
        enc.push_back(i + 2 < buf.size() ? b64[v & 63] : '=');
    }
    return std::string(
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,") + enc + "\",\"byteLength\":" +
        std::to_string(buf.size()) + "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,\"target\":34963}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0.5,0.25,1],\"metallicFactor\":0.2,\"roughnessFactor\":0.8}}],"
        "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
}

} // namespace

TEST(Assets, GltfTriangle) {
    const auto path = std::filesystem::temp_directory_path() / "ignis_triangle.gltf";
    {
        std::ofstream f(path);
        f << make_gltf_triangle();
    }
    AssetManager am;
    auto model = am.load_model(path.string());
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->mesh->primitives.size(), 1u);
    const auto& prim = model->mesh->primitives[0];
    EXPECT_EQ(prim.positions.size(), 3u);
    EXPECT_EQ(prim.indices.size(), 3u);
    EXPECT_EQ(prim.indices[2], 2u);
    EXPECT_NEAR(prim.positions[1].x, 1.f, 1e-6f);
    ASSERT_EQ(model->materials.size(), 1u);
    EXPECT_NEAR(model->materials[0].base_color.g, 0.5f, 1e-6f);
    EXPECT_NEAR(model->materials[0].metallic, 0.2f, 1e-6f);
    // Cached on second load.
    EXPECT_EQ(am.load_model(path.string()).get(), model.get());
    std::filesystem::remove(path);
}

TEST(Assets, HdrTexture) {
    const auto path = std::filesystem::temp_directory_path() / "ignis_test.hdr";
    {
        const auto bytes = make_hdr_4x4();
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    AssetManager am;
    auto tex = am.load_texture(path.string());
    ASSERT_NE(tex, nullptr);
    EXPECT_TRUE(tex->hdr);
    EXPECT_EQ(tex->width, 4);
    EXPECT_EQ(tex->height, 4);
    ASSERT_EQ(tex->hdr_pixels.size(), 4u * 4u * 4u);
    // stb_image HDR convention: float = byte * 2^(exponent-136) -> 128 * 1 = 128
    EXPECT_NEAR(tex->hdr_pixels[0], 128.f, 1e-4f);
    std::filesystem::remove(path);
}

TEST(Assets, MissingFileReturnsNull) {
    AssetManager am;
    EXPECT_EQ(am.load_model("/nonexistent/model.gltf"), nullptr);
    EXPECT_EQ(am.load_texture("/nonexistent/tex.png"), nullptr);
}

namespace {

namespace fs = std::filesystem;

struct AssetSandbox {
    fs::path tmp;
    fs::path proj;
    AssetSandbox() {
        tmp = fs::temp_directory_path() / "ignis_assets_sec";
        fs::remove_all(tmp);
        proj = tmp / "proj";
        fs::create_directories(proj);
        std::ofstream(proj / "data.txt") << "hello";
        std::ofstream(tmp / "secret.txt") << "secret";
    }
    ~AssetSandbox() { std::error_code ec; fs::remove_all(tmp, ec); }
};

} // namespace

TEST(AssetsSecurity, ProjectModeAcceptsRelativePathWithinRoot) {
    AssetSandbox sb;
    AssetManager am;
    am.set_access_mode(AssetAccessMode::ProjectFilesystem);
    am.set_project_root(sb.proj.string());
    auto r = am.read_file("data.txt");
    ASSERT_TRUE(r.ok()) << r.error();
    EXPECT_EQ(std::string(r.value().begin(), r.value().end()), "hello");
}

TEST(AssetsSecurity, ProjectModeRejectsTraversalAbsoluteAndSymlinkEscape) {
    AssetSandbox sb;
    std::error_code ec;
    fs::create_symlink(sb.tmp / "secret.txt", sb.proj / "link.txt", ec);
    ASSERT_FALSE(ec) << ec.message();
    AssetManager am;
    am.set_access_mode(AssetAccessMode::ProjectFilesystem);
    am.set_project_root(sb.proj.string());
    EXPECT_FALSE(am.read_file("../secret.txt").ok());   // traversal (file exists!)
    EXPECT_FALSE(am.read_file(sb.tmp.string() + "/secret.txt").ok()); // absolute
    EXPECT_FALSE(am.read_file("link.txt").ok());         // symlink escaping root
    EXPECT_TRUE(am.read_file("nonexistent.txt").ok() == false); // controlled failure
}

TEST(AssetsSecurity, PackageOnlyNeverFallsBackToDisk) {
    AssetSandbox sb;
    const auto pak_path = sb.tmp / "p.ignispak";
    PakWriter w;
    ASSERT_TRUE(w.add_file("assets/a.bin", {'x'}).ok());
    ASSERT_TRUE(w.write(pak_path.string()).ok());
    PakReader reader;
    ASSERT_TRUE(reader.open(pak_path.string()).ok());
    AssetManager am;
    am.set_pak(&reader);
    am.set_access_mode(AssetAccessMode::PackageOnly);
    auto from_pak = am.read_file("assets/a.bin");
    ASSERT_TRUE(from_pak.ok());
    // data.txt exists on disk under the project, but PackageOnly must not read it.
    EXPECT_FALSE(am.read_file("data.txt").ok());
    EXPECT_FALSE(am.read_file("../secret.txt").ok());
}

TEST(AssetsSecurity, PackageOnlyWithoutPakFails) {
    AssetManager am;
    am.set_access_mode(AssetAccessMode::PackageOnly);
    EXPECT_FALSE(am.read_file("anything.txt").ok());
}

TEST(AssetsSecurity, EditorModeStillAllowsAbsolutePaths) {
    AssetManager am; // default EditorFilesystem
    const auto path = std::filesystem::temp_directory_path() / "ignis_editor_abs.txt";
    std::ofstream(path) << "ok";
    EXPECT_TRUE(am.read_file(path.string()).ok());
    std::filesystem::remove(path);
}
