#include <gtest/gtest.h>

#include "ignis/renderer/ibl.h"
#include "ignis/renderer/renderer.h"

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
