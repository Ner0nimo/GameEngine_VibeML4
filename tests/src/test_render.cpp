// Offscreen GPU render tests (graphics builds only). The renderer runs the
// full deferred pipeline on a real Vulkan device and the readback pixels are
// asserted. A fork-based guard skips the tests cleanly on machines where the
// driver stack can't run NVRHI (e.g. some VMs), instead of crashing the suite.

#include <gtest/gtest.h>

#include "ignis/ecs/components.h"
#include "ignis/renderer/renderer.h"
#include "ignis/scene/scene.h"

#include <algorithm>
#include <cmath>

#ifdef IGNIS_RENDERER_VULKAN
#include "ignis/physics/physics_world.h"
#include "ignis/renderer/vulkan_renderer.h"
#include "ignis/window/window.h"
#include <vulkan/vulkan.h>
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ignis;

#ifdef IGNIS_RENDERER_VULKAN

namespace {

// Build the small test scene used by the render tests.
Scene make_render_scene() {
    Scene scene("render_test");
    register_builtin_components(scene.world());

    EntityId cube = scene.world().create_entity("cube");
    scene.world().emplace<TransformComponent>(cube);
    auto& mr = scene.world().emplace<MeshRendererComponent>(cube);
    mr.model_path = "assets/cube.gltf";

    EntityId cam = scene.world().create_entity("cam");
    auto& ct = scene.world().emplace<TransformComponent>(cam);
    ct.position = {0.f, 0.f, 3.f};
    auto& cc = scene.world().emplace<CameraComponent>(cam);
    cc.active = true;
    cc.clear_color = {0.02f, 0.02f, 0.03f, 1.f};

    EntityId sun = scene.world().create_entity("sun");
    auto& dl = scene.world().emplace<DirectionalLightComponent>(sun);
    dl.direction = {0.f, -1.f, -0.5f};
    dl.intensity = 3.f;
    return scene;
}

// Returns true if an offscreen NVRHI render works in this environment.
// Runs the actual probe in a forked child so a driver crash cannot take down
// the test process.
bool gpu_render_works() {
    const pid_t pid = fork();
    if (pid == 0) {
        // Child: try a minimal offscreen render; exit code tells the parent.
        Scene scene = make_render_scene();
        Camera camera;
        camera.position = {0.f, 0.f, 3.f};
        camera.fov_y_deg = 60.f;
        VulkanRenderer renderer;
        if (!renderer.init_offscreen(64, 64).ok()) _exit(2);
        renderer.render(scene, camera, 1.f / 60.f);
        auto px = renderer.readback();
        if (!px.ok()) _exit(3);
        if (px.value().size() != 64u * 64u * 4u) _exit(4);
        renderer.shutdown();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool render_cube(std::vector<u8>& px) {
    Scene scene = make_render_scene();
    Camera camera;
    camera.position = {0.f, 0.f, 3.f};
    camera.fov_y_deg = 60.f;
    VulkanRenderer renderer;
    if (!renderer.init_offscreen(64, 64).ok()) return false;
    renderer.render(scene, camera, 1.f / 60.f);
    auto rb = renderer.readback();
    const bool ok = rb.ok();
    if (ok) px = std::move(rb.value());
    renderer.shutdown();
    return ok;
}

} // namespace

#ifdef IGNIS_RENDERER_VULKAN
// Raw Vulkan sanity check (linked libvulkan, no dispatcher): instance, device,
// properties, timeline semaphore. The NVRHI dispatcher lives in the NVRHI
// shared library now and is exercised by the fork-guarded render tests below.
TEST(Render, RawVulkanSanity) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstance inst = VK_NULL_HANDLE;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    // A Vulkan loader can be present while no driver is usable (GPU-less CI
    // runners: VK_ERROR_INCOMPATIBLE_DRIVER, zero devices) - skip, don't fail.
    const VkResult irc = vkCreateInstance(&ici, nullptr, &inst);
    if (irc != VK_SUCCESS)
        GTEST_SKIP() << "no usable Vulkan driver (vkCreateInstance=" << static_cast<int>(irc) << ")";
    u32 n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::fprintf(stderr, "ENUM count=%u\n", n);
    if (n == 0) {
        vkDestroyInstance(inst, nullptr);
        GTEST_SKIP() << "Vulkan loader present but no physical devices (no GPU/driver)";
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    std::fprintf(stderr, "ENUM wrote=%u first=%p\n", n, static_cast<void*>(devs[0]));
    VkPhysicalDevice phys = devs[0];
    ASSERT_NE(phys, VK_NULL_HANDLE);

    // Software rasterizers (lavapipe/SwiftShader, e.g. mesa-vulkan-drivers in
    // CI) enumerate fine but crash on some raw API paths used below; the
    // offscreen NVRHI tests cover rendering there via the fork guard instead.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    const std::string devName(props.deviceName);
    if (devName.find("llvmpipe") != std::string::npos ||
        devName.find("SwiftShader") != std::string::npos) {
        vkDestroyInstance(inst, nullptr);
        GTEST_SKIP() << "software Vulkan driver '" << devName << "' skipped (covered by fork-guarded NVRHI tests)";
    }

    // 1) RAW call through the linked libvulkan symbol.
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    vkGetPhysicalDeviceProperties2(phys, &p2);
    std::fprintf(stderr, "RAW-CALL OK: %s\n", p2.properties.deviceName);

    // Create a real device so the dispatcher gets a valid VkDevice.
    u32 qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    u32 qf = 0;
    for (u32 i = 0; i < qn; ++i) {
        VkQueueFamilyProperties qp{};
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &i, &qp);
        if (qp.queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }
    }
    const float pri = 1.f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &pri;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateDevice(phys, &dci, nullptr, &dev), VK_SUCCESS);

    // 2) Raw call with pNext chain and timeline semaphore (raw C API).
    VkPhysicalDeviceSubgroupProperties sub{};
    sub.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 p4{};
    p4.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p4.pNext = &sub;
    vkGetPhysicalDeviceProperties2(phys, &p4);
    std::fprintf(stderr, "RAW-CALL-PNEXT OK: %s subgroupSize=%u\n",
                 p4.properties.deviceName, sub.subgroupSize);

    VkSemaphoreTypeCreateInfo st{};
    st.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sc{};
    sc.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sc.pNext = &st;
    VkSemaphore sem = VK_NULL_HANDLE;
    VkResult sr = vkCreateSemaphore(dev, &sc, nullptr, &sem);
    std::fprintf(stderr, "RAW-SEMAPHORE-TIMELINE result=%d\n", static_cast<int>(sr));
    EXPECT_EQ(sr, VK_SUCCESS);
    if (sr == VK_SUCCESS) vkDestroySemaphore(dev, sem, nullptr);
}
#endif

#ifdef IGNIS_RENDERER_VULKAN
TEST(Render, OffscreenInitInProcess) {
    if (!gpu_render_works()) GTEST_SKIP() << "Vulkan/NVRHI device not usable in this environment";
    std::vector<u8> px;
    ASSERT_TRUE(render_cube(px));
    std::fprintf(stderr, "render_cube ok, %zu bytes\n", px.size());
    // Center pixel must be the lit cube, corner the clear color.
    auto pix = [&](u32 x, u32 y) -> u32 {
        const size_t i = (static_cast<size_t>(y) * 64 + x) * 4;
        return static_cast<u32>(px[i]) + px[i + 1] + px[i + 2];
    };
    const u32 center = pix(32, 32);
    const u32 corner = pix(2, 2);
    std::fprintf(stderr, "center=%u corner=%u\n", center, corner);
    EXPECT_GT(center, corner + 40u);
}
#endif

TEST(Render, OffscreenCubeLit) {
    if (!gpu_render_works()) GTEST_SKIP() << "Vulkan/NVRHI device not usable in this environment";
    std::vector<u8> px;
    ASSERT_TRUE(render_cube(px));
    ASSERT_EQ(px.size(), 64u * 64u * 4u);

    auto pix = [&](u32 x, u32 y) -> u32 {
        const size_t i = (static_cast<size_t>(y) * 64 + x) * 4;
        return static_cast<u32>(px[i]) + px[i + 1] + px[i + 2];
    };
    const u32 center = pix(32, 32); // cube front face, lit
    const u32 corner = pix(2, 2);   // sky = clear color
    EXPECT_GT(center, corner + 40u) << "center=" << center << " corner=" << corner;
    // Red-dominant cube material (baseColor 0.8, 0.2, 0.2).
    {
        const size_t i = (32u * 64u + 32u) * 4;
        EXPECT_GT(px[i], px[i + 1]);
        EXPECT_GT(px[i], px[i + 2]);
    }
}

// A point light (with the directional light disabled) must brighten the cube
// compared to the same scene with the point light at zero intensity.
TEST(Render, PointLightLightsCube) {
    if (!gpu_render_works()) GTEST_SKIP() << "Vulkan/NVRHI device not usable in this environment";
    auto render_with_point = [](f32 point_intensity) {
        Scene scene = make_render_scene();
        for (EntityId e : scene.world().view<DirectionalLightComponent>()) {
            if (auto* l = scene.world().get<DirectionalLightComponent>(e)) {
                l->intensity = 0.f;
                l->cast_shadow = false;
            }
        }
        EntityId lamp = scene.world().create_entity("lamp");
        auto& lt = scene.world().emplace<TransformComponent>(lamp);
        // Behind the cube: the camera-visible face at the frame center has
        // normal (0,0,-1) (back-face culling), so the light must sit behind.
        lt.position = {0.f, 0.5f, -1.5f};
        auto& pl = scene.world().emplace<PointLightComponent>(lamp);
        pl.color = {1.f, 1.f, 1.f};
        pl.intensity = point_intensity;
        pl.range = 5.f;
        Camera camera;
        camera.position = {0.f, 0.f, 3.f};
        camera.fov_y_deg = 60.f;
        VulkanRenderer renderer;
        if (!renderer.init_offscreen(64, 64).ok()) return std::vector<u8>{};
        renderer.render(scene, camera, 1.f / 60.f);
        auto rb = renderer.readback();
        renderer.shutdown();
        return rb.ok() ? std::move(rb.value()) : std::vector<u8>{};
    };
    const std::vector<u8> withLight = render_with_point(200.f);
    const std::vector<u8> without = render_with_point(0.f);
    ASSERT_EQ(withLight.size(), 64u * 64u * 4u);
    ASSERT_EQ(without.size(), withLight.size());
    auto total = [](const std::vector<u8>& px) {
        u64 sum = 0;
        for (size_t i = 0; i + 2 < px.size(); i += 4)
            sum += static_cast<u64>(px[i]) + static_cast<u64>(px[i + 1]) + static_cast<u64>(px[i + 2]);
        return sum;
    };
    const u64 lit = total(withLight);
    const u64 dark = total(without);
    std::fprintf(stderr, "point light: total with=%llu without=%llu\n",
                 static_cast<unsigned long long>(lit), static_cast<unsigned long long>(dark));
    // Whole-frame sum is robust to per-pixel jitter on software rasterizers.
    EXPECT_GT(lit, dark + 2000u) << "point light must brighten the frame";
}

// A far occluder (beyond the range a single camera-centered shadow box covers)
// must cast a shadow on the ground when 4 cascades are active; the 1-cascade
// fallback must still render.
TEST(Render, CSMShadowsFarGeometry) {
    if (!gpu_render_works()) GTEST_SKIP() << "Vulkan/NVRHI device not usable in this environment";
    auto render = [](u32 cascades) {
        Scene scene("csm");
        register_builtin_components(scene.world());
        EntityId ground = scene.world().create_entity("ground");
        auto& gt = scene.world().emplace<TransformComponent>(ground);
        gt.position = {0.f, -2.f, -40.f};
        gt.scale = {60.f, 1.f, 60.f};
        scene.world().emplace<MeshRendererComponent>(ground).model_path = "assets/cube.gltf";
        EntityId blocker = scene.world().create_entity("blocker");
        auto& bt = scene.world().emplace<TransformComponent>(blocker);
        bt.position = {0.f, 4.f, -45.f};
        bt.scale = {2.f, 2.f, 2.f};
        scene.world().emplace<MeshRendererComponent>(blocker).model_path = "assets/cube.gltf";
        EntityId sun = scene.world().create_entity("sun");
        auto& dl = scene.world().emplace<DirectionalLightComponent>(sun);
        dl.direction = {0.f, -1.f, -0.5f};
        dl.intensity = 3.f;
        dl.cast_shadow = true;
        Camera camera;
        camera.position = {0.f, 0.f, 3.f};
        camera.fov_y_deg = 60.f;
        VulkanRenderer renderer;
        renderer.set_shadow_cascade_count(cascades);
        if (!renderer.init_offscreen(64, 64).ok()) return std::vector<u8>{};
        renderer.render(scene, camera, 1.f / 60.f);
        auto rb = renderer.readback();
        renderer.shutdown();
        return rb.ok() ? std::move(rb.value()) : std::vector<u8>{};
    };
    const std::vector<u8> px = render(4);
    ASSERT_EQ(px.size(), 64u * 64u * 4u);
    const std::vector<u8> px1 = render(1); // single-map fallback must still render
    ASSERT_EQ(px1.size(), px.size());
    auto lum = [](const std::vector<u8>& p, u32 x, u32 y) {
        const size_t i = (static_cast<size_t>(y) * 64u + x) * 4u;
        return static_cast<u32>(p[i]) + p[i + 1] + p[i + 2];
    };
    u32 shadowMin = ~0u, litMax = 0;
    for (u32 y = 31; y <= 36; ++y)
        for (u32 x = 28; x <= 36; ++x) shadowMin = std::min(shadowMin, lum(px, x, y));
    for (u32 y = 39; y <= 44; ++y)
        for (u32 x = 28; x <= 36; ++x) litMax = std::max(litMax, lum(px, x, y));
    std::fprintf(stderr, "csm: shadowMin=%u litMax=%u\n", shadowMin, litMax);
    EXPECT_GT(litMax, 100u); // ground is lit, not sky
    EXPECT_LT(shadowMin, litMax - 15u) << "far occluder must cast a shadow with 4 cascades";
}

// Bloom must add a measurable halo around the bright cube compared to bloom off.
TEST(Render, BloomAddsHalo) {
    if (!gpu_render_works()) GTEST_SKIP() << "Vulkan/NVRHI device not usable in this environment";
    auto render = [](bool bloom) {
        Scene scene = make_render_scene();
        Camera camera;
        camera.position = {0.f, 0.f, 3.f};
        camera.fov_y_deg = 60.f;
        VulkanRenderer renderer;
        if (!renderer.init_offscreen(64, 64).ok()) return std::vector<u8>{};
        // after init: impl exists. threshold 0.3 so the sun-lit cube (HDR ~1) blooms.
        renderer.set_bloom(bloom, 1.0f, 0.3f, 0.5f);
        renderer.render(scene, camera, 1.f / 60.f);
        auto rb = renderer.readback();
        renderer.shutdown();
        return rb.ok() ? std::move(rb.value()) : std::vector<u8>{};
    };
    const std::vector<u8> on = render(true);
    const std::vector<u8> off = render(false);
    ASSERT_EQ(on.size(), 64u * 64u * 4u);
    ASSERT_EQ(off.size(), on.size());
    // Average luminance in a ring just outside the cube (cube spans ~center 14..50).
    auto ringAvg = [](const std::vector<u8>& px) {
        u64 sum = 0;
        u32 n = 0;
        for (u32 y = 0; y < 64; ++y)
            for (u32 x = 0; x < 64; ++x) {
                const u32 dx = x > 32 ? x - 32 : 32 - x;
                const u32 dy = y > 32 ? y - 32 : 32 - y;
                const u32 d = std::max(dx, dy);
                if (d >= 20 && d <= 24) {
                    const size_t i = (static_cast<size_t>(y) * 64u + x) * 4u;
                    sum += static_cast<u64>(px[i]) + static_cast<u64>(px[i + 1]) +
                           static_cast<u64>(px[i + 2]);
                    ++n;
                }
            }
        return n == 0 ? 0u : static_cast<u32>(sum / n);
    };
    const u32 ringOn = ringAvg(on);
    const u32 ringOff = ringAvg(off);
    std::fprintf(stderr, "bloom ring: on=%u off=%u\n", ringOn, ringOff);
    EXPECT_GT(ringOn, ringOff + 2u) << "bloom must brighten the ring around the cube";
}

// SSR must add the cube's reflected light to the ground (lower half of the
// frame) compared to SSR off (IBL-only reflections).
TEST(Render, SSRReflectsScene) {
    if (!gpu_render_works()) GTEST_SKIP() << "Vulkan/NVRHI device not usable in this environment";
    auto render = [](bool ssr) {
        Scene scene("ssr");
        register_builtin_components(scene.world());
        EntityId ground = scene.world().create_entity("ground");
        auto& gt = scene.world().emplace<TransformComponent>(ground);
        gt.position = {0.f, -1.f, 0.f};
        gt.scale = {40.f, 1.f, 40.f};
        scene.world().emplace<MeshRendererComponent>(ground).model_path = "assets/cube.gltf";
        EntityId cube = scene.world().create_entity("cube");
        auto& ct = scene.world().emplace<TransformComponent>(cube);
        ct.position = {0.f, 0.5f, -2.f}; // sitting on the ground
        scene.world().emplace<MeshRendererComponent>(cube).model_path = "assets/cube.gltf";
        EntityId sun = scene.world().create_entity("sun");
        auto& dl = scene.world().emplace<DirectionalLightComponent>(sun);
        dl.direction = {0.f, -1.f, -0.5f};
        dl.intensity = 3.f;
        Camera camera;
        camera.position = {0.f, 2.f, 4.f};
        camera.fov_y_deg = 60.f;
        // Pitch down so the cube's ground reflection is inside the frame.
        const math::Mat4 lv = math::look_at(camera.position, math::Vec3(0.f, -1.f, -2.f),
                                            math::Vec3(0.f, 1.f, 0.f));
        camera.rotation = glm::quat_cast(glm::transpose(glm::mat3(lv)));
        VulkanRenderer renderer;
        if (!renderer.init_offscreen(64, 64).ok()) return std::vector<u8>{};
        renderer.set_ssr(ssr, 2.0f);
        // Two frames: SSR samples the previous frame's lit HDR.
        renderer.render(scene, camera, 1.f / 60.f);
        renderer.render(scene, camera, 1.f / 60.f);
        auto rb = renderer.readback();
        renderer.shutdown();
        return rb.ok() ? std::move(rb.value()) : std::vector<u8>{};
    };
    const std::vector<u8> on = render(true);
    const std::vector<u8> off = render(false);
    ASSERT_EQ(on.size(), 64u * 64u * 4u);
    ASSERT_EQ(off.size(), on.size());
    auto half = [](const std::vector<u8>& px, bool lower) {
        u64 sum = 0;
        for (u32 y = lower ? 32 : 0; y < (lower ? 64 : 32); ++y)
            for (u32 x = 0; x < 64; ++x) {
                const size_t i = (static_cast<size_t>(y) * 64u + x) * 4u;
                sum += static_cast<u64>(px[i]) + static_cast<u64>(px[i + 1]) + static_cast<u64>(px[i + 2]);
            }
        return sum;
    };
    const u64 withSSR = half(on, true);
    const u64 without = half(off, true);
    const u64 upperOn = half(on, false);
    const u64 upperOff = half(off, false);
    std::fprintf(stderr, "ssr: lower with=%llu without=%llu upper with=%llu without=%llu\n",
                 static_cast<unsigned long long>(withSSR), static_cast<unsigned long long>(without),
                 static_cast<unsigned long long>(upperOn), static_cast<unsigned long long>(upperOff));
    EXPECT_GT(withSSR, without + 300u) << "SSR must brighten the ground via the cube's reflection";
    // The upper half (sky + cube) should be roughly unchanged: reflections land on the ground.
    const i64 upperDiff = static_cast<i64>(upperOn) - static_cast<i64>(upperOff);
    EXPECT_LT(std::abs(upperDiff), 2000);
}

TEST(Render, DebugLinesDrawn) {
    if (!gpu_render_works()) GTEST_SKIP() << "Vulkan/NVRHI device not usable in this environment";
    Scene scene("lines");
    register_builtin_components(scene.world());
    EntityId cam = scene.world().create_entity("cam");
    auto& ct = scene.world().emplace<TransformComponent>(cam);
    ct.position = {0.f, 0.f, 3.f};
    auto& cc = scene.world().emplace<CameraComponent>(cam);
    cc.active = true;
    EntityId sun = scene.world().create_entity("sun");
    scene.world().emplace<DirectionalLightComponent>(sun).direction = {0.f, -1.f, -0.5f};

    Camera camera;
    camera.position = {0.f, 0.f, 3.f};

    VulkanRenderer renderer;
    ASSERT_TRUE(renderer.init_offscreen(64, 64).ok());
    renderer.set_debug_lines({{{-1.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {1.f, 1.f, 0.f, 1.f}}});
    renderer.render(scene, camera, 1.f / 60.f);
    auto pxr = renderer.readback();
    ASSERT_TRUE(pxr.ok()) << pxr.error();
    const std::vector<u8>& px = pxr.value();
    bool found = false;
    for (u32 y = 30; y <= 34 && !found; ++y) {
        for (u32 x = 8; x < 56 && !found; ++x) {
            const size_t i = (static_cast<size_t>(y) * 64u + x) * 4;
            if (px[i] > 200 && px[i + 1] > 200 && px[i + 2] < 100) found = true;
        }
    }
    EXPECT_TRUE(found);
    renderer.shutdown();
}

// The real test scene (scenes/test_scene.scene.json) must render visible
// geometry offscreen, not just the sky/clear color - guards against a black
// window in the windowed runtime.
TEST(Render, TestSceneOffscreenRenders) {
    if (!gpu_render_works()) GTEST_SKIP() << "Vulkan/NVRHI device not usable in this environment";
    Scene scene;
    register_builtin_components(scene.world());
    PhysicsSystem::register_components(scene.world());
    ASSERT_TRUE(scene.load_file(std::string(IGNIS_SCENES_DIR) + "/test_scene.scene.json").ok());

    Camera camera;
    bool haveCam = false;
    for (EntityId e : scene.world().view<CameraComponent, TransformComponent>()) {
        const auto* cc = scene.world().get<CameraComponent>(e);
        const auto* tc = scene.world().get<TransformComponent>(e);
        if (cc && cc->active && tc) {
            camera.position = tc->position;
            camera.rotation = tc->rotation;
            camera.fov_y_deg = cc->fov_y_deg;
            camera.z_near = cc->z_near;
            camera.z_far = cc->z_far;
            haveCam = true;
        }
    }
    ASSERT_TRUE(haveCam) << "test scene must have an active camera";

    VulkanRenderer renderer;
    ASSERT_TRUE(renderer.init_offscreen(128, 128).ok());
    renderer.render(scene, camera, 1.f / 60.f);
    auto pxr = renderer.readback();
    ASSERT_TRUE(pxr.ok()) << pxr.error();
    const std::vector<u8>& px = pxr.value();
    ASSERT_EQ(px.size(), 128u * 128u * 4u);

    // Sky = clear color (0.05, 0.08, 0.16) -> roughly (61, 77, 112) after
    // tonemap+gamma, sum ~250. Lit geometry (red cube ~ (191,101,101)) is
    // clearly brighter. Require a visible amount of non-sky pixels.
    u32 lit = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        if (static_cast<u32>(px[i]) + px[i + 1] + px[i + 2] > 300u) ++lit;
    }
    std::fprintf(stderr, "scene render: lit pixels=%u / %u\n", lit, 128u * 128u);
    EXPECT_GT(lit, 128u * 128u / 50u); // > 2% of the frame is lit geometry
    renderer.shutdown();
}

// Issue #53 regression test: offscreen targets must be recreated on resize,
// and rendering + readback must work at the new size (both up and down).
TEST(Render, OffscreenResize) {
    if (!gpu_render_works()) GTEST_SKIP() << "Vulkan/NVRHI device not usable in this environment";
    Scene scene = make_render_scene();
    Camera camera;
    camera.position = {0.f, 0.f, 3.f};
    camera.fov_y_deg = 60.f;

    VulkanRenderer renderer;
    ASSERT_TRUE(renderer.init_offscreen(64, 64).ok());
    renderer.render(scene, camera, 1.f / 60.f);
    auto rb1 = renderer.readback();
    ASSERT_TRUE(rb1.ok()) << rb1.error();
    ASSERT_EQ(rb1.value().size(), 64u * 64u * 4u);

    // Grow.
    renderer.on_resize(128, 96);
    renderer.render(scene, camera, 1.f / 60.f);
    auto rb2 = renderer.readback();
    ASSERT_TRUE(rb2.ok()) << rb2.error();
    ASSERT_EQ(rb2.value().size(), 128u * 96u * 4u);
    {
        const std::vector<u8>& px = rb2.value();
        const size_t i = (48u * 128u + 64u) * 4; // center of 128x96
        const u32 center = static_cast<u32>(px[i]) + px[i + 1] + px[i + 2];
        EXPECT_GT(center, 200u) << "cube must still be lit after resize";
    }

    // Shrink.
    renderer.on_resize(48, 32);
    renderer.render(scene, camera, 1.f / 60.f);
    auto rb3 = renderer.readback();
    ASSERT_TRUE(rb3.ok()) << rb3.error();
    EXPECT_EQ(rb3.value().size(), 48u * 32u * 4u);
    renderer.shutdown();
}

// Issue #53 windowed path: a framebuffer resize event must lead to a swapchain
// recreation on the next render() (no crash, new extent reported). Fork-guarded
// because it needs a real window + driver.
TEST(Render, WindowedResizeRecreatesSwapchain) {
    if (!gpu_render_works()) GTEST_SKIP() << "Vulkan/NVRHI device not usable in this environment";
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        GTEST_SKIP() << "no display available";
    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        WindowProps props;
        props.title = "ignis-resize-test";
        props.width = 320;
        props.height = 240;
        props.visible = false;
        Window win(props);
        if (!win.init().ok()) _exit(2);
        VulkanRenderer r;
        if (!r.init(&win).ok()) _exit(3);
        win.on_resize = [&](u32 w, u32 h) { r.request_resize(w, h); };
        Scene scene = make_render_scene();
        Camera camera;
        camera.position = {0.f, 0.f, 3.f};
        camera.fov_y_deg = 60.f;
        r.render(scene, camera, 1.f / 60.f);
        r.present();
        const RawVulkanHandles h0 = r.raw_handles();
        if (!h0.windowed) _exit(4);
        // Really resize the window; the framebuffer-size callback fires during
        // poll_events and records a pending resize, which the next render()
        // must process by recreating the swapchain.
        glfwSetWindowSize(win.handle(), static_cast<int>(h0.width / 2),
                          static_cast<int>(h0.height / 2));
        for (int i = 0; i < 10; ++i) win.poll_events();
        r.render(scene, camera, 1.f / 60.f);
        r.present();
        const RawVulkanHandles h1 = r.raw_handles();
        if (!h1.windowed) _exit(5);
        if (h1.width >= h0.width || h1.height >= h0.height) _exit(6); // must have shrunk
        r.shutdown();
        win.shutdown();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "child status=" << status << " (2=no window, 3=no renderer, 4/5=not windowed, 6=extent did not shrink)";
}

#else

TEST(Render, OffscreenCubeLit) {
    GTEST_SKIP() << "requires a graphics build (IGNIS_WITH_GRAPHICS=ON)";
}
TEST(Render, PointLightLightsCube) {
    GTEST_SKIP() << "requires a graphics build (IGNIS_WITH_GRAPHICS=ON)";
}
TEST(Render, CSMShadowsFarGeometry) {
    GTEST_SKIP() << "requires a graphics build (IGNIS_WITH_GRAPHICS=ON)";
}
TEST(Render, BloomAddsHalo) {
    GTEST_SKIP() << "requires a graphics build (IGNIS_WITH_GRAPHICS=ON)";
}
TEST(Render, DebugLinesDrawn) {
    GTEST_SKIP() << "requires a graphics build (IGNIS_WITH_GRAPHICS=ON)";
}
TEST(Render, TestSceneOffscreenRenders) {
    GTEST_SKIP() << "requires a graphics build (IGNIS_WITH_GRAPHICS=ON)";
}
TEST(Render, OffscreenResize) {
    GTEST_SKIP() << "requires a graphics build (IGNIS_WITH_GRAPHICS=ON)";
}
TEST(Render, WindowedResizeRecreatesSwapchain) {
    GTEST_SKIP() << "requires a graphics build (IGNIS_WITH_GRAPHICS=ON)";
}

#endif

// The renderer's light gather must find point lights via
// view<PointLightComponent, TransformComponent> and read the world position.
TEST(Render, PointLightGatherView) {
    Scene scene("t");
    register_builtin_components(scene.world());
    EntityId lamp = scene.world().create_entity("lamp");
    auto& lt = scene.world().emplace<TransformComponent>(lamp);
    lt.position = {0.f, 1.f, 2.f};
    auto& pl = scene.world().emplace<PointLightComponent>(lamp);
    pl.color = {1.f, 1.f, 1.f};
    pl.intensity = 50.f;
    pl.range = 10.f;
    u32 n = 0;
    for (EntityId e : scene.world().view<PointLightComponent, TransformComponent>()) {
        const auto* p = scene.world().get<PointLightComponent>(e);
        ASSERT_NE(p, nullptr);
        const math::Mat4 wm = world_matrix(scene.world(), e);
        EXPECT_NEAR(wm[3].x, 0.f, 1e-6f);
        EXPECT_NEAR(wm[3].y, 1.f, 1e-6f);
        EXPECT_NEAR(wm[3].z, 2.f, 1e-6f);
        EXPECT_NEAR(p->intensity, 50.f, 1e-6f);
        EXPECT_NEAR(p->range, 10.f, 1e-6f);
        ++n;
    }
    EXPECT_EQ(n, 1u);
}

// CSM split math (CPU, always compiled).
TEST(Render, CascadeSplitsShape) {
    const auto s = cascade_splits(0.1f, 100.f, 4);
    ASSERT_EQ(s.size(), 5u);
    EXPECT_NEAR(s.front(), 0.1f, 1e-6f);
    EXPECT_NEAR(s.back(), 100.f, 1e-4f);
    for (u32 i = 1; i < s.size(); ++i) EXPECT_GT(s[i], s[i - 1]);
    for (f32 v : s) {
        EXPECT_GE(v, 0.1f);
        EXPECT_LE(v, 100.f);
    }
    // Invalid input -> empty (documented contract).
    EXPECT_TRUE(cascade_splits(0.f, 100.f, 4).empty());
    EXPECT_TRUE(cascade_splits(10.f, 5.f, 4).empty());
    EXPECT_TRUE(cascade_splits(0.1f, 100.f, 0).empty());
    // lambda = 0 -> uniform splits; lambda = 1 -> pure logarithmic.
    const auto uni = cascade_splits(1.f, 100.f, 3, 0.f);
    ASSERT_EQ(uni.size(), 4u);
    EXPECT_NEAR(uni[1], 1.f + 99.f / 3.f, 1e-3f);
    const auto log = cascade_splits(1.f, 100.f, 3, 1.f);
    ASSERT_EQ(log.size(), 4u);
    EXPECT_NEAR(log[1], std::pow(100.f, 1.f / 3.f), 1e-3f);
}
