#include <gtest/gtest.h>

#include "ignis/assets/asset_manager.h"
#include "ignis/assets/asset_types.h"
#include "ignis/core/pak.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    auto model_r = am.load_model(path.string());
    ASSERT_TRUE(model_r.ok()) << model_r.error();
    const auto& model = model_r.value();
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
    auto again = am.load_model(path.string());
    ASSERT_TRUE(again.ok()) << again.error();
    EXPECT_EQ(again.value().get(), model.get());
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
    auto tex_r = am.load_texture(path.string());
    ASSERT_TRUE(tex_r.ok()) << tex_r.error();
    const auto& tex = tex_r.value();
    EXPECT_TRUE(tex->hdr);
    EXPECT_EQ(tex->width, 4);
    EXPECT_EQ(tex->height, 4);
    ASSERT_EQ(tex->hdr_pixels.size(), 4u * 4u * 4u);
    // stb_image HDR convention: float = byte * 2^(exponent-136) -> 128 * 1 = 128
    EXPECT_NEAR(tex->hdr_pixels[0], 128.f, 1e-4f);
    std::filesystem::remove(path);
}

TEST(Assets, MissingFileReturnsErrorWithReason) {
    AssetManager am;
    auto model_r = am.load_model("/nonexistent/model.gltf");
    ASSERT_FALSE(model_r.ok());
    EXPECT_FALSE(model_r.error().empty()); // honest failure: the reason travels with it
    auto tex_r = am.load_texture("/nonexistent/tex.png");
    ASSERT_FALSE(tex_r.ok());
    EXPECT_FALSE(tex_r.error().empty());
    EXPECT_EQ(am.cache_size(), 0u); // failures must not be cached
}

// Issue #114: a model referencing a missing image fails the strict load_model
// (with the image error in the message), while load_model_lenient returns the
// partial model with a structured AssetError and a null texture slot.
TEST(Assets, GltfMissingImageStructuredError) {
    std::string json = make_gltf_triangle();
    const std::string marker = "\"materials\":[{\"pbrMetallicRoughness\":{\"";
    const auto pos = json.find(marker);
    ASSERT_NE(pos, std::string::npos);
    json.replace(pos, marker.size(),
                 "\"images\":[{\"uri\":\"missing_tex.png\"}],"
                 "\"textures\":[{\"source\":0}],"
                 "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"");
    const auto path = std::filesystem::temp_directory_path() / "ignis_missing_img.gltf";
    {
        std::ofstream f(path);
        f << json;
    }
    AssetManager am;
    // Strict: failure, reason mentions image 0.
    auto strict_r = am.load_model(path.string());
    ASSERT_FALSE(strict_r.ok());
    EXPECT_NE(strict_r.error().find("image 0"), std::string::npos);
    // Lenient: partial model with structured error.
    auto lenient_r = am.load_model_lenient(path.string());
    ASSERT_TRUE(lenient_r.ok());
    auto& model = lenient_r.value();
    ASSERT_EQ(model->mesh->primitives.size(), 1u) << "geometry survives a missing texture";
    ASSERT_EQ(model->textures.size(), 1u);
    EXPECT_EQ(model->textures[0], nullptr);
    ASSERT_EQ(model->texture_errors.size(), 1u);
    EXPECT_EQ(model->texture_errors[0].kind, AssetErrorKind::MissingDependency);
    EXPECT_EQ(model->texture_errors[0].image_index, 0);
    EXPECT_NE(model->texture_errors[0].source_path.find("missing_tex.png"), std::string::npos);
    EXPECT_FALSE(model->texture_errors[0].message.empty());
    // Same cached load: strict still fails, lenient still returns the model.
    EXPECT_FALSE(am.load_model(path.string()).ok());
    EXPECT_TRUE(am.load_model_lenient(path.string()).ok());
    std::filesystem::remove(path);
}

// A texture file that exists but is garbage -> Decode error (not MissingDependency).
TEST(Assets, GltfMalformedImageFileRecordsDecodeError) {
    const auto dir = std::filesystem::temp_directory_path();
    {
        std::ofstream f(dir / "garbage_tex.png", std::ios::binary);
        f << "this is not an image";
    }
    std::string json = make_gltf_triangle();
    const std::string marker = "\"materials\":[{\"pbrMetallicRoughness\":{\"";
    const auto pos = json.find(marker);
    ASSERT_NE(pos, std::string::npos);
    json.replace(pos, marker.size(),
                 "\"images\":[{\"uri\":\"garbage_tex.png\"}],"
                 "\"textures\":[{\"source\":0}],"
                 "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"");
    const auto path = dir / "ignis_bad_img.gltf";
    {
        std::ofstream f(path);
        f << json;
    }
    AssetManager am;
    auto r = am.load_model_lenient(path.string());
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value()->texture_errors.size(), 1u);
    EXPECT_EQ(r.value()->texture_errors[0].kind, AssetErrorKind::Decode);
    EXPECT_EQ(r.value()->texture_errors[0].image_index, 0);
    std::filesystem::remove(path);
    std::filesystem::remove(dir / "garbage_tex.png");
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

TEST(AssetsSecurity, SymlinkLeafRejectedAtOpenEvenInEditorMode) {
    // IGN-002 (TOCTOU mitigation): the final path component is opened with
    // O_NOFOLLOW on Linux, so a symlinked leaf is rejected at the filesystem
    // boundary even in EditorFilesystem mode (no root check there).
#if defined(__linux__)
    AssetSandbox sb;
    std::error_code ec;
    fs::create_symlink(sb.tmp / "secret.txt", sb.proj / "innocent_link.txt", ec);
    ASSERT_FALSE(ec) << ec.message();
    AssetManager am; // EditorFilesystem default
    EXPECT_FALSE(am.read_file((sb.proj / "innocent_link.txt").string()).ok())
        << "symlink leaf must be rejected at open, not followed";
    // A regular file still reads fine.
    EXPECT_TRUE(am.read_file((sb.proj / "data.txt").string()).ok());
#endif
}

TEST(AssetsSecurity, HugeDimensionImageRejectedBeforeAllocation) {
    // Minimal PNG header claiming 100000x100000 — stb must reject it via the
    // dimension cap instead of attempting a huge allocation.
    std::vector<u8> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const u32 ihdr_len = 13;
    const u32 w = 100000, h = 100000;
    auto be32 = [&](u32 v) { png.push_back(static_cast<u8>(v >> 24)); png.push_back(static_cast<u8>(v >> 16)); png.push_back(static_cast<u8>(v >> 8)); png.push_back(static_cast<u8>(v)); };
    be32(ihdr_len);
    const char ihdr[4] = {'I', 'H', 'D', 'R'};
    png.insert(png.end(), ihdr, ihdr + 4);
    be32(w); be32(h);
    png.push_back(8); png.push_back(2); png.push_back(0); png.push_back(0); png.push_back(0);
    be32(0); // crc (stb does not verify)
    const auto path = std::filesystem::temp_directory_path() / "ignis_huge.png";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }
    AssetManager am;
    EXPECT_FALSE(am.load_texture(path.string()).ok());
    std::filesystem::remove(path);
}

namespace {

std::string b64_encode(const std::vector<u8>& buf) {
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
    return enc;
}

fs::path write_tmp_text(const std::string& name, const std::string& content) {
    const auto p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return p;
}

fs::path write_tmp_bytes(const std::string& name, const std::vector<u8>& bytes) {
    const auto p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return p;
}

// glTF whose single data-URI buffer holds buffer_bytes, but whose bufferView
// claims view_len and whose POSITION accessor reads accessor_count vec3s.
std::string gltf_with_data_buffer(size_t buffer_bytes, size_t view_len, size_t accessor_count) {
    const std::vector<u8> buf(buffer_bytes, 0);
    return std::string("{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,") + b64_encode(buf) +
        "\",\"byteLength\":" + std::to_string(buf.size()) + "}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" + std::to_string(view_len) +
        ",\"target\":34962}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":" +
        std::to_string(accessor_count) + ",\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
}

// glTF with a buffer that has neither a URI nor data (unloaded buffer).
std::string gltf_no_uri_buffer() {
    return "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":42}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
}

// glTF referencing an external buffer file (42 bytes: 3 vec3 positions + 3 u16 indices).
std::string gltf_external_buffer(const std::string& uri) {
    return std::string("{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"") + uri + "\",\"byteLength\":42}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,\"target\":34963}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
}

} // namespace

TEST(AssetsMalformed, GltfTruncatedJsonReturnsError) {
    const auto p = write_tmp_text("ignis_bad_truncated.gltf", "{\"asset\":{\"version\":\"2.0\"}");
    AssetManager am;
    auto r = am.load_model(p.string());
    ASSERT_FALSE(r.ok());
    EXPECT_FALSE(r.error().empty());
    EXPECT_EQ(am.cache_size(), 0u); // failures must not be cached
    fs::remove(p);
}

TEST(AssetsMalformed, GltfBufferViewOutOfBoundsReturnsErrorWithoutCrash) {
    // bufferView claims 12000 bytes, buffer holds 42, accessor reads 1000 vec3:
    // must be rejected by validation before any accessor unpack read.
    const auto p = write_tmp_text("ignis_bad_oob.gltf", gltf_with_data_buffer(42, 12000, 1000));
    AssetManager am;
    EXPECT_NO_THROW(EXPECT_FALSE(am.load_model(p.string()).ok()));
    EXPECT_EQ(am.cache_size(), 0u);
    fs::remove(p);
}

TEST(AssetsMalformed, GltfUnloadedBufferReturnsErrorWithoutThrow) {
    // Buffer with neither uri nor data: unpack yields nothing; the loader must
    // turn that into a controlled error Result, never an escaping exception.
    const auto p = write_tmp_text("ignis_bad_nobuf.gltf", gltf_no_uri_buffer());
    AssetManager am;
    EXPECT_NO_THROW(EXPECT_FALSE(am.load_model(p.string()).ok()));
    fs::remove(p);
}

TEST(AssetsMalformed, GltfMissingExternalBufferReturnsError) {
    const auto p = write_tmp_text("ignis_bad_missing_bin.gltf", gltf_external_buffer("missing.bin"));
    AssetManager am; // EditorFilesystem: external URIs allowed, file absent
    EXPECT_FALSE(am.load_model(p.string()).ok());
    fs::remove(p);
}

TEST(AssetsMalformed, ExternalBufferWorksInEditorMode) {
    // Control: editor mode (trusted process) still loads external .bin buffers
    // resolved relative to the .gltf file.
    const auto bin = write_tmp_bytes("ignis_ext.bin", std::vector<u8>(42, 0));
    const auto p = write_tmp_text("ignis_ext.gltf", gltf_external_buffer("ignis_ext.bin"));
    AssetManager am; // EditorFilesystem (default)
    auto model_r = am.load_model(p.string());
    ASSERT_TRUE(model_r.ok()) << model_r.error();
    const auto& model = model_r.value();
    ASSERT_EQ(model->mesh->primitives.size(), 1u);
    EXPECT_EQ(model->mesh->primitives[0].positions.size(), 3u);
    fs::remove(p);
    fs::remove(bin);
}

TEST(AssetsMalformed, PackageOnlyRejectsExternalBufferTraversal) {
    // The pak boundary must be real: a .gltf inside the pak whose buffer URI
    // escapes the package ("../...") must not be opened on the host FS in
    // PackageOnly (cgltf's own FILE* loading would follow the traversal).
    const auto bin = write_tmp_bytes("ignis_pak_evil.bin", std::vector<u8>(42, 0));
    const auto gltf = gltf_external_buffer("../ignis_pak_evil.bin"); // exists next to CWD on host
    const auto pak_path = fs::temp_directory_path() / "ignis_evil.ignispak";
    PakWriter w;
    ASSERT_TRUE(w.add_file("assets/m.gltf", std::vector<u8>(gltf.begin(), gltf.end())).ok());
    ASSERT_TRUE(w.write(pak_path.string()).ok());
    PakReader reader;
    ASSERT_TRUE(reader.open(pak_path.string()).ok());
    AssetManager am;
    am.set_pak(&reader);
    am.set_access_mode(AssetAccessMode::PackageOnly);
    EXPECT_FALSE(am.load_model("assets/m.gltf").ok());
    fs::remove(pak_path);
    fs::remove(bin);
}

TEST(AssetsMalformed, PackageOnlyLoadsDataUriModelFromPak) {
    // Control: the same loader path works when the buffer is embedded.
    const auto gltf = make_gltf_triangle();
    const auto pak_path = fs::temp_directory_path() / "ignis_good.ignispak";
    PakWriter w;
    ASSERT_TRUE(w.add_file("assets/m.gltf", std::vector<u8>(gltf.begin(), gltf.end())).ok());
    ASSERT_TRUE(w.write(pak_path.string()).ok());
    PakReader reader;
    ASSERT_TRUE(reader.open(pak_path.string()).ok());
    AssetManager am;
    am.set_pak(&reader);
    am.set_access_mode(AssetAccessMode::PackageOnly);
    auto model_r = am.load_model("assets/m.gltf");
    ASSERT_TRUE(model_r.ok()) << model_r.error();
    const auto& model = model_r.value();
    ASSERT_EQ(model->mesh->primitives.size(), 1u);
    EXPECT_EQ(model->mesh->primitives[0].positions.size(), 3u);
    fs::remove(pak_path);
}

TEST(AssetsMalformed, CorruptTexturesReturnError) {
    std::vector<u8> garbage(512, 0xAB);
    const auto p1 = write_tmp_bytes("ignis_garbage.png", garbage);
    const auto p2 = write_tmp_bytes("ignis_empty.png", {});
    const auto p3 = write_tmp_bytes("ignis_trunc.png", {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
    AssetManager am;
    EXPECT_FALSE(am.load_texture(p1.string()).ok());
    EXPECT_FALSE(am.load_texture(p2.string()).ok());
    EXPECT_FALSE(am.load_texture(p3.string()).ok());
    EXPECT_EQ(am.cache_size(), 0u);
    fs::remove(p1);
    fs::remove(p2);
    fs::remove(p3);
}

TEST(AssetsMalformed, GltfTooManyBuffersRejectedBeforeLoading) {
    // 2000 buffer entries: the count limit must reject before per-buffer work.
    std::string gltf = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[";
    for (int i = 0; i < 2000; ++i) {
        if (i != 0) gltf += ",";
        gltf += "{\"byteLength\":0}";
    }
    gltf += "]}";
    const auto p = write_tmp_text("ignis_bad_many_buf.gltf", gltf);
    AssetManager am;
    EXPECT_NO_THROW(EXPECT_FALSE(am.load_model(p.string()).ok()));
    fs::remove(p);
}

TEST(AssetsMalformed, GltfHugeDeclaredBufferRejectedBeforeAllocation) {
    // byteLength claims 4 GiB but the data URI holds 8 bytes: the per-buffer
    // cap must reject before cgltf allocates the declared size.
    const std::string gltf =
        std::string("{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,") +
        b64_encode(std::vector<u8>(8, 0)) +
        "\",\"byteLength\":4294967295}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8,\"target\":34962}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"VEC3\"}]}";
    const auto p = write_tmp_text("ignis_bad_huge_buf.gltf", gltf);
    AssetManager am;
    EXPECT_NO_THROW(EXPECT_FALSE(am.load_model(p.string()).ok()));
    fs::remove(p);
}

TEST(AssetsMalformed, GltfTooManyImagesRejected) {
    std::string gltf = "{\"asset\":{\"version\":\"2.0\"},\"images\":[";
    for (int i = 0; i < 2000; ++i) {
        if (i != 0) gltf += ",";
        gltf += "{\"uri\":\"data:,\"}";
    }
    gltf += "]}";
    const auto p = write_tmp_text("ignis_bad_many_img.gltf", gltf);
    AssetManager am;
    EXPECT_NO_THROW(EXPECT_FALSE(am.load_model(p.string()).ok()));
    fs::remove(p);
}

namespace {

// Minimal GLB (binary glTF): JSON chunk + BIN chunk with one triangle
// (3 vec3 positions + 3 u16 indices in buffers[0], no uri -> BIN chunk).
std::vector<u8> make_glb_triangle(const std::string& extra_buffers_json = "") {
    std::vector<u8> bin(42, 0);
    const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint16_t idx[3] = {0, 1, 2};
    std::memcpy(bin.data(), pos, sizeof(pos));
    std::memcpy(bin.data() + 36, idx, sizeof(idx));

    std::string json = "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":42}" + extra_buffers_json + "],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,\"target\":34963}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    while (json.size() % 4 != 0) json += ' ';
    while (bin.size() % 4 != 0) bin.push_back(0);

    std::vector<u8> glb;
    auto u32le = [&](u32 v) {
        for (int i = 0; i < 4; ++i) glb.push_back(static_cast<u8>(v >> (8 * i)));
    };
    u32le(0x46546C67u); // "glTF"
    u32le(2);
    u32le(0); // total length, patched below
    u32le(static_cast<u32>(json.size()));
    u32le(0x4E4F534Au); // "JSON"
    glb.insert(glb.end(), json.begin(), json.end());
    u32le(static_cast<u32>(bin.size()));
    u32le(0x004E4942u); // "BIN\0"
    glb.insert(glb.end(), bin.begin(), bin.end());
    const u32 total = static_cast<u32>(glb.size());
    for (size_t i = 0; i < 4; ++i)
        glb[static_cast<size_t>(8) + i] = static_cast<u8>(total >> (8 * i));
    return glb;
}

} // namespace

TEST(AssetsMalformed, GlbLoadsFromDiskInEditorMode) {
    const auto glb = make_glb_triangle();
    const auto p = write_tmp_bytes("ignis_tri.glb", glb);
    AssetManager am; // EditorFilesystem
    auto model_r = am.load_model(p.string());
    ASSERT_TRUE(model_r.ok()) << model_r.error();
    const auto& model = model_r.value();
    ASSERT_EQ(model->mesh->primitives.size(), 1u);
    EXPECT_EQ(model->mesh->primitives[0].positions.size(), 3u);
    fs::remove(p);
}

TEST(AssetsMalformed, GlbLoadsFromPakInPackageOnly) {
    // Boundary pin: a GLB served only from the pak must load without any
    // host-FS access (the BIN chunk is served in-memory by cgltf).
    const auto glb = make_glb_triangle();
    const auto pak_path = fs::temp_directory_path() / "ignis_glb.ignispak";
    PakWriter w;
    ASSERT_TRUE(w.add_file("assets/m.glb", glb).ok());
    ASSERT_TRUE(w.write(pak_path.string()).ok());
    PakReader reader;
    ASSERT_TRUE(reader.open(pak_path.string()).ok());
    AssetManager am;
    am.set_pak(&reader);
    am.set_access_mode(AssetAccessMode::PackageOnly);
    auto model_r = am.load_model("assets/m.glb");
    ASSERT_TRUE(model_r.ok()) << model_r.error();
    const auto& model = model_r.value();
    ASSERT_EQ(model->mesh->primitives.size(), 1u);
    EXPECT_EQ(model->mesh->primitives[0].positions.size(), 3u);
    fs::remove(pak_path);
}

TEST(AssetsMalformed, GlbWithExternalBufferUriRejectedInPackageOnly) {
    // A GLB carrying an extra buffer with a traversal URI must be rejected
    // at the pak boundary, not opened from the host FS.
    const auto glb = make_glb_triangle(",{\"uri\":\"../evil.bin\",\"byteLength\":4}");
    const auto pak_path = fs::temp_directory_path() / "ignis_glb_evil.ignispak";
    PakWriter w;
    ASSERT_TRUE(w.add_file("assets/m.glb", glb).ok());
    ASSERT_TRUE(w.write(pak_path.string()).ok());
    PakReader reader;
    ASSERT_TRUE(reader.open(pak_path.string()).ok());
    AssetManager am;
    am.set_pak(&reader);
    am.set_access_mode(AssetAccessMode::PackageOnly);
    EXPECT_FALSE(am.load_model("assets/m.glb").ok());
    fs::remove(pak_path);
}

TEST(AssetsSecurity, HdrHugeDimensionsRejectedBeforeAllocation) {
    // Valid Radiance header claiming 20000x20000 (= 6.4 GB as f32 RGBA):
    // the decoded-dimension cap must reject it before any allocation.
    const auto p = write_tmp_text("ignis_huge.hdr",
                                  "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 20000 +X 20000\n");
    AssetManager am;
    EXPECT_FALSE(am.load_texture(p.string()).ok());
    fs::remove(p);
}

TEST(AssetsSecurity, PngHugeButAllocatableDimensionsRejected) {
    // 20000x20000 PNG = 1.6 GB RGBA: a naive decoder would allocate it (and
    // OOM on smaller machines); the cap rejects the header up front.
    std::vector<u8> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto be32 = [&](u32 v) {
        png.push_back(static_cast<u8>(v >> 24));
        png.push_back(static_cast<u8>(v >> 16));
        png.push_back(static_cast<u8>(v >> 8));
        png.push_back(static_cast<u8>(v));
    };
    be32(13);
    const char ihdr[4] = {'I', 'H', 'D', 'R'};
    png.insert(png.end(), ihdr, ihdr + 4);
    be32(20000);
    be32(20000);
    png.push_back(8);
    png.push_back(2);
    png.push_back(0);
    png.push_back(0);
    png.push_back(0);
    be32(0); // crc (stb does not verify)
    const auto p = write_tmp_bytes("ignis_huge2.png", png);
    AssetManager am;
    EXPECT_FALSE(am.load_texture(p.string()).ok());
    fs::remove(p);
}

// --- asset type registry (issue #15) -------------------------------------------

TEST(AssetTypes, LookupByExtensionAndFile) {
    const auto* gltf = find_asset_type(".gltf");
    ASSERT_NE(gltf, nullptr);
    EXPECT_STREQ(gltf->component, "MeshRenderer");
    EXPECT_TRUE(gltf->can_spawn);
    EXPECT_STREQ(gltf->icon, "M");

    const auto* wav = find_asset_type(".wav");
    ASSERT_NE(wav, nullptr);
    EXPECT_STREQ(wav->component, "AudioSource");

    const auto* lua = find_asset_type(".lua");
    ASSERT_NE(lua, nullptr);
    EXPECT_STREQ(lua->component, "Script");

    // ".scene.json" suffix wins over the plain ".json" extension.
    const auto* scene = find_asset_type_for_file("test_scene.scene.json");
    ASSERT_NE(scene, nullptr);
    EXPECT_TRUE(scene->is_scene);
    EXPECT_EQ(scene->extension, ".scene.json");

    const auto* plain = find_asset_type_for_file("notes.json");
    ASSERT_NE(plain, nullptr);
    EXPECT_FALSE(plain->is_scene);

    EXPECT_EQ(find_asset_type(".xyz"), nullptr);
    EXPECT_EQ(find_asset_type_for_file("noext"), nullptr);
    EXPECT_FALSE(asset_types().empty());
}

TEST(AssetTypes, FindReferencesScansJsonFiles) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ignis_asset_ref_test";
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "a.scene.json");
        f << R"({"entities":[{"components":{"MeshRenderer":{"model_path":"assets/cube.gltf"}}}]})";
    }
    {
        std::ofstream f(dir / "b.json");
        f << R"({"note":"nothing here"})";
    }
    {
        std::ofstream f(dir / "c.txt"); // not json: ignored
        f << "assets/cube.gltf";
    }
    const auto refs = find_asset_references(dir.string(), "assets/cube.gltf");
    EXPECT_EQ(refs.size(), 1u);
    EXPECT_NE(refs[0].find("a.scene.json"), std::string::npos);
    EXPECT_TRUE(find_asset_references(dir.string(), "assets/missing.gltf").empty());
    fs::remove_all(dir);
}
