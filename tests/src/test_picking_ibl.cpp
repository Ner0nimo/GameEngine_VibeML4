#include <gtest/gtest.h>

#include "ignis/renderer/ibl.h"
#include "ignis/renderer/renderer.h"

#include <algorithm>
#include <cmath>

using namespace ignis;

TEST(Picking, ScreenRayCenter) {
    Camera cam;
    cam.position = {1.f, 2.f, 3.f};
    cam.rotation = math::Quat(1.f, 0.f, 0.f, 0.f); // identity: looks down -Z
    const picking::Ray r = picking::screen_ray(cam, 1.f, 0.f, 0.f);
    EXPECT_NEAR(r.origin.x, 1.f, 1e-6f);
    EXPECT_NEAR(r.dir.x, 0.f, 1e-6f);
    EXPECT_NEAR(r.dir.y, 0.f, 1e-6f);
    EXPECT_NEAR(r.dir.z, -1.f, 1e-6f);
}

TEST(Picking, RayAabb) {
    const math::Vec3 o{0.f, 0.f, -5.f};
    const math::Vec3 d{0.f, 0.f, 1.f};
    const auto hit = picking::ray_aabb(o, d, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(*hit, 4.f, 1e-5f);
    EXPECT_FALSE(picking::ray_aabb(o, d, {10.f, 0.f, 0.f}, {1.f, 1.f, 1.f}).has_value());
    // Ray starting inside the box hits at t=0.
    const auto inside = picking::ray_aabb({0.f, 0.f, 0.f}, d, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
    ASSERT_TRUE(inside.has_value());
    EXPECT_NEAR(*inside, 0.f, 1e-6f);
}

TEST(Ibl, SampleEquirect) {
    // 2x1 HDR: left pixel red, right pixel green.
    Texture hdr;
    hdr.hdr = true;
    hdr.width = 2;
    hdr.height = 1;
    hdr.channels = 4;
    hdr.hdr_pixels = {
        1.f, 0.f, 0.f, 1.f,   // pixel (0,0) red
        0.f, 1.f, 0.f, 1.f,   // pixel (1,0) green
    };
    // Direction +X maps to u = 0.75 -> 75% towards the green pixel.
    const math::Vec3 c = sample_equirect(hdr, {1.f, 0.f, 0.f});
    EXPECT_NEAR(c.x, 0.25f, 1e-5f);
    EXPECT_NEAR(c.y, 0.75f, 1e-5f);
    EXPECT_NEAR(c.z, 0.f, 1e-5f);
    // Invalid texture -> black.
    const Texture empty;
    EXPECT_EQ(sample_equirect(empty, {1, 0, 0}), math::Vec3(0.f));
}

TEST(Ibl, IrradianceCubemapShape) {
    Texture hdr;
    hdr.hdr = true;
    hdr.width = 8;
    hdr.height = 4;
    hdr.channels = 4;
    hdr.hdr_pixels.assign(8u * 4u * 4u, 1.f); // uniform white sky
    const Cubemap cube = equirect_to_irradiance(hdr, 8, 16);
    EXPECT_EQ(cube.face_size, 8);
    ASSERT_EQ(cube.faces.size(), 6u * 8u * 8u * 4u);
    for (f32 v : cube.faces) {
        EXPECT_GE(v, 0.f);
        EXPECT_LE(v, 10.f);
    }
    // Uniform white sky -> every texel ~1.
    EXPECT_NEAR(cube.faces[0], 1.f, 0.2f);
}

namespace {

// HDR equirect with a dim background and one bright "sun" spot around +X.
Texture make_spot_sky(int w, int h, f32 spot_value) {
    Texture t;
    t.hdr = true;
    t.width = w;
    t.height = h;
    t.channels = 4;
    t.hdr_pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0.f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(w);
            const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(h);
            // +X maps to u ~0.75, v ~0.5 in sample_equirect's convention.
            const f32 du = u - 0.75f, dv = v - 0.5f;
            const bool spot = du * du + dv * dv < 0.02f * 0.02f;
            const size_t base = (static_cast<size_t>(y) * static_cast<size_t>(w) +
                                 static_cast<size_t>(x)) * 4u;
            const f32 val = spot ? spot_value : 0.02f;
            t.hdr_pixels[base] = val;
            t.hdr_pixels[base + 1] = val;
            t.hdr_pixels[base + 2] = val;
            t.hdr_pixels[base + 3] = 1.f;
        }
    }
    return t;
}

f32 max_channel(const Cubemap& c, int channel) {
    f32 m = 0.f;
    for (size_t i = static_cast<size_t>(channel); i < c.faces.size(); i += 4)
        m = std::max(m, c.faces[i]);
    return m;
}

} // namespace

TEST(Ibl, PrefilterMipShapes) {
    Texture sky = make_spot_sky(64, 32, 10.f);
    const auto levels = equirect_to_prefiltered(sky, 16, 4, 32);
    ASSERT_EQ(levels.size(), 4u);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(levels[static_cast<size_t>(i)].face_size, 16 >> i);
        const size_t n = 6u * static_cast<size_t>(16 >> i) * static_cast<size_t>(16 >> i) * 4u;
        EXPECT_EQ(levels[static_cast<size_t>(i)].faces.size(), n);
    }
}

TEST(Ibl, PrefilterUniformSky) {
    Texture hdr;
    hdr.hdr = true;
    hdr.width = 8;
    hdr.height = 4;
    hdr.channels = 4;
    hdr.hdr_pixels.assign(8u * 4u * 4u, 1.f); // uniform white sky
    const auto levels = equirect_to_prefiltered(hdr, 8, 3, 16);
    ASSERT_EQ(levels.size(), 3u);
    // Uniform sky stays uniform at every roughness.
    for (const Cubemap& lvl : levels)
        for (size_t i = 0; i < lvl.faces.size(); i += 4)
            EXPECT_NEAR(lvl.faces[i], 1.f, 0.05f);
}

TEST(Ibl, PrefilterBlursWithRoughness) {
    // A bright spot must stay sharp in mip 0 (roughness 0) and be averaged
    // away in the last mip (roughness 1).
    Texture sky = make_spot_sky(64, 32, 10.f);
    const auto levels = equirect_to_prefiltered(sky, 32, 4, 256);
    ASSERT_EQ(levels.size(), 4u);
    const f32 sharp = max_channel(levels[0], 0);
    const f32 blurry = max_channel(levels[3], 0);
    EXPECT_GT(sharp, 5.f) << "mip 0 should keep the sun spot bright";
    EXPECT_GT(sharp, 4.f * blurry) << "roughness 1 mip must blur the spot: sharp=" << sharp
                                  << " blurry=" << blurry;
}

TEST(Ibl, BrdfLutShapeAndRange) {
    const int size = 32;
    const auto lut = build_brdf_lut(size, 64);
    ASSERT_EQ(lut.size(), static_cast<size_t>(size) * size * 2u);
    for (f32 v : lut) {
        EXPECT_GE(v, 0.f);
        EXPECT_LE(v, 1.f + 1e-4f);
    }
    // At NdotV ~ 1 (x = size-1) and roughness ~ 0 (y = 0) the integral is
    // (scale=1, bias=0). LUT texel index = (y * size + x) * 2.
    const size_t corner = (static_cast<size_t>(size - 1)) * 2u; // (u=1-ish, v=0)
    EXPECT_NEAR(lut[corner], 1.f, 0.1f);
    EXPECT_NEAR(lut[corner + 1], 0.f, 0.1f);
    // Bias must grow with roughness at normal incidence (grazing Fresnel lobe).
    const size_t rough = (static_cast<size_t>(size - 1) * static_cast<size_t>(size) +
                          static_cast<size_t>(size - 1)) * 2u; // (u=1-ish, v=1-ish)
    EXPECT_GT(lut[rough + 1], lut[corner + 1]);
}
