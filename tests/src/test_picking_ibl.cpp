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

TEST(Picking, ScreenRayOrtho) {
    Camera cam;
    cam.orthographic = true;
    cam.ortho_size = 5.f;
    cam.position = {1.f, 2.f, 3.f};
    cam.rotation = math::Quat(1.f, 0.f, 0.f, 0.f); // identity: looks down -Z
    const f32 aspect = 2.f;
    // Center ray: origin at the camera, direction = forward.
    const picking::Ray center = picking::screen_ray(cam, aspect, 0.f, 0.f);
    EXPECT_NEAR(center.origin.x, 1.f, 1e-6f);
    EXPECT_NEAR(center.origin.y, 2.f, 1e-6f);
    EXPECT_NEAR(center.origin.z, 3.f, 1e-6f);
    EXPECT_NEAR(center.dir.z, -1.f, 1e-6f);
    // Corner ray (ndc = 1,1): origin offset by half the view volume, same direction.
    const picking::Ray corner = picking::screen_ray(cam, aspect, 1.f, 1.f);
    EXPECT_NEAR(corner.origin.x, 1.f + 10.f, 1e-5f); // ortho_size * aspect
    EXPECT_NEAR(corner.origin.y, 2.f + 5.f, 1e-5f);  // ortho_size
    EXPECT_NEAR(corner.dir.z, -1.f, 1e-6f);
    EXPECT_NEAR(corner.dir.x, 0.f, 1e-6f);
}

TEST(Camera, OrthoProjectionMatrix) {
    Camera cam;
    cam.orthographic = true;
    cam.ortho_size = 10.f;
    cam.z_near = 0.1f;
    cam.z_far = 100.f;
    const math::Mat4 p = cam.projection_matrix(2.f);
    // Same mapping as math::ortho_vk over the ortho_size-derived volume.
    const glm::vec4 near_clip = p * glm::vec4(0.f, 0.f, -0.1f, 1.f);
    EXPECT_NEAR(near_clip.z / near_clip.w, 0.f, 1e-4f);
    const glm::vec4 far_clip = p * glm::vec4(0.f, 0.f, -100.f, 1.f);
    EXPECT_NEAR(far_clip.z / far_clip.w, 1.f, 1e-3f);
    EXPECT_NEAR((p * glm::vec4(-20.f, 0.f, -1.f, 1.f)).x, -1.f, 1e-5f); // -ortho_size*aspect
    EXPECT_NEAR((p * glm::vec4(0.f, 10.f, -1.f, 1.f)).y, 1.f, 1e-5f);   // +ortho_size
    // Default is perspective.
    const Camera persp;
    EXPECT_FALSE(persp.orthographic);
    const math::Mat4 pp = persp.projection_matrix(1.f);
    EXPECT_NEAR((pp * glm::vec4(0.f, 0.f, -0.1f, 1.f)).z / (pp * glm::vec4(0.f, 0.f, -0.1f, 1.f)).w,
                0.f, 1e-4f);
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

// --- marquee selection helpers (issue #11) -------------------------------------

TEST(Picking, WorldToScreenCenter) {
    Camera cam; // identity at origin, looking down -Z
    const auto px = picking::world_to_screen(cam, 1.f, 100.f, 100.f, math::Vec3(0.f, 0.f, -5.f));
    ASSERT_TRUE(px.has_value());
    EXPECT_NEAR(px->x, 50.f, 0.5f);
    EXPECT_NEAR(px->y, 50.f, 0.5f);
}

TEST(Picking, WorldToScreenBehindCameraIsNullopt) {
    Camera cam;
    EXPECT_FALSE(picking::world_to_screen(cam, 1.f, 100.f, 100.f, math::Vec3(0.f, 0.f, 5.f)).has_value());
}

TEST(Picking, WorldToScreenRoundTripWithRay) {
    Camera cam;
    cam.position = {1.f, 2.f, 3.f};
    cam.rotation = glm::angleAxis(0.3f, math::Vec3(0.f, 1.f, 0.f));
    const math::Vec3 p(1.f, 0.5f, -4.f);
    const auto px = picking::world_to_screen(cam, 16.f / 9.f, 1280.f, 720.f, p);
    ASSERT_TRUE(px.has_value());
    // Convert the pixel back to the NDC the editor feeds into screen_ray and
    // check the ray actually passes through the point's neighborhood.
    const f32 ndc_x = px->x / 1280.f * 2.f - 1.f;
    const f32 ndc_y = -(px->y / 720.f * 2.f - 1.f);
    const picking::Ray ray = picking::screen_ray(cam, 16.f / 9.f, ndc_x, ndc_y);
    const auto t = picking::ray_aabb(ray.origin, ray.dir, p, math::Vec3(0.01f));
    EXPECT_TRUE(t.has_value());
}

TEST(Picking, ProjectAabbCenterAndBehind) {
    Camera cam;
    const auto r = picking::project_aabb(cam, 1.f, 100.f, 100.f, math::Vec3(0.f, 0.f, -5.f),
                                         math::Vec3(1.f));
    EXPECT_TRUE(r.valid);
    EXPECT_LT(r.min_x, 50.f);
    EXPECT_GT(r.max_x, 50.f);
    EXPECT_LT(r.min_y, 50.f);
    EXPECT_GT(r.max_y, 50.f);
    // Box behind the camera -> invalid rect.
    const auto behind = picking::project_aabb(cam, 1.f, 100.f, 100.f, math::Vec3(0.f, 0.f, 5.f),
                                              math::Vec3(1.f));
    EXPECT_FALSE(behind.valid);
}

TEST(Picking, ProjectAabbOffToTheSide) {
    Camera cam;
    const auto r = picking::project_aabb(cam, 1.f, 100.f, 100.f, math::Vec3(3.f, 0.f, -5.f),
                                         math::Vec3(0.5f));
    EXPECT_TRUE(r.valid);
    EXPECT_GT(r.min_x, 50.f); // entirely right of center
}

TEST(Picking, RectsIntersect) {
    const picking::ScreenRect a{10.f, 10.f, 50.f, 50.f, true};
    const picking::ScreenRect b{40.f, 40.f, 80.f, 80.f, true};
    const picking::ScreenRect c{60.f, 60.f, 90.f, 90.f, true};
    const picking::ScreenRect invalid{0.f, 0.f, 0.f, 0.f, false};
    EXPECT_TRUE(picking::rects_intersect(a, b));
    EXPECT_FALSE(picking::rects_intersect(a, c));
    EXPECT_FALSE(picking::rects_intersect(a, invalid));
}
