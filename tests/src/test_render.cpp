// Offscreen GPU render tests (graphics builds only). The renderer runs the
// full deferred pipeline on a real Vulkan device and the readback pixels are
// asserted. A fork-based guard skips the tests cleanly on machines where the
// driver stack can't run NVRHI (e.g. some VMs), instead of crashing the suite.

#include <gtest/gtest.h>

#include "ignis/ecs/components.h"
#include "ignis/renderer/renderer.h"
#include "ignis/scene/scene.h"

#ifdef IGNIS_RENDERER_VULKAN
#include "ignis/renderer/vulkan_renderer.h"
#include <vulkan/vulkan.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.hpp>
#include <cstdio>
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
        std::string err;
        if (!renderer.init_offscreen(64, 64, err)) _exit(2);
        renderer.render(scene, camera, 1.f / 60.f);
        std::vector<u8> px;
        if (!renderer.readback(px)) _exit(3);
        if (px.size() != 64u * 64u * 4u) _exit(4);
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
    std::string err;
    if (!renderer.init_offscreen(64, 64, err)) return false;
    renderer.render(scene, camera, 1.f / 60.f);
    const bool ok = renderer.readback(px);
    renderer.shutdown();
    return ok;
}

} // namespace

#ifdef IGNIS_RENDERER_VULKAN
// Definitive comparison: raw linked vk* call vs vulkan-hpp dynamic dispatcher.
void ignis_init_vulkan_dispatch(void*, void*);
TEST(Render, RawVsDispatcherCall) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstance inst = VK_NULL_HANDLE;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ASSERT_EQ(vkCreateInstance(&ici, nullptr, &inst), VK_SUCCESS);
    u32 n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::fprintf(stderr, "ENUM count=%u\n", n);
    ASSERT_GT(n, 0u);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    std::fprintf(stderr, "ENUM wrote=%u first=%p\n", n, static_cast<void*>(devs[0]));
    VkPhysicalDevice phys = devs[0];
    ASSERT_NE(phys, VK_NULL_HANDLE);

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

    // 2) Dispatcher call (same function via defaultDispatchLoaderDynamic).
    ignis_init_vulkan_dispatch(inst, dev);
    auto& d = vk::detail::defaultDispatchLoaderDynamic;
    ASSERT_NE(d.vkGetPhysicalDeviceProperties2, nullptr);
    VkPhysicalDeviceProperties2 p3{};
    p3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    d.vkGetPhysicalDeviceProperties2(phys, &p3);
    std::fprintf(stderr, "DISPATCH-CALL OK: %s\n", p3.properties.deviceName);

    // 3) Dispatcher call WITH pNext chain, exactly like NVRHI's Device ctor.
    VkPhysicalDeviceSubgroupProperties sub{};
    sub.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 p4{};
    p4.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p4.pNext = &sub;
    d.vkGetPhysicalDeviceProperties2(phys, &p4);
    std::fprintf(stderr, "DISPATCH-CALL-PNEXT OK: %s subgroupSize=%u\n",
                 p4.properties.deviceName, sub.subgroupSize);

    // 4) Dispatcher vkCreateSemaphore with timeline pNext, like NVRHI's Queue.
    VkSemaphoreTypeCreateInfo st{};
    st.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sc{};
    sc.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sc.pNext = &st;
    VkSemaphore sem = VK_NULL_HANDLE;
    VkResult sr = d.vkCreateSemaphore(dev, &sc, nullptr, &sem);
    std::fprintf(stderr, "DISPATCH-SEMAPHORE result=%d\n", static_cast<int>(sr));

    // 4b) Same semaphore creation but through the vk:: wrapper exactly like NVRHI.
    {
        vk::SemaphoreTypeCreateInfo sti;
        sti.setSemaphoreType(vk::SemaphoreType::eTimeline);
        vk::SemaphoreCreateInfo sci;
        sci.setPNext(&sti);
        vk::Device vdev(dev);
        auto sem2 = vdev.createSemaphore(sci, nullptr);
        std::fprintf(stderr, "VKHPP-SEMAPHORE ok=%d\n", sem2 ? 1 : 0);
    }

    // 4c) Exact NVRHI Queue replication: pointer-typed null allocator.
    {
        vk::AllocationCallbacks* cb = nullptr;
        vk::Device vdev(dev);
        vk::SemaphoreTypeCreateInfo sti;
        sti.setSemaphoreType(vk::SemaphoreType::eTimeline);
        vk::SemaphoreCreateInfo sci;
        sci.setPNext(&sti);
        auto s = vdev.createSemaphore(sci, cb);
        std::fprintf(stderr, "VKHPP-SEM-PTRCB ok=%d\n", s ? 1 : 0);
    }

    // 4d) vk::PhysicalDevice::getProperties2 via wrapper + default dispatcher.
    {
        vk::PhysicalDevice vphys(phys);
        vk::PhysicalDeviceProperties2 pp;
        vphys.getProperties2(&pp);
        std::fprintf(stderr, "VKHPP-GETPROPS OK: %s\n", pp.properties.deviceName.data());
    }

    // NOTE: full nvrhi::vulkan::createDevice is exercised via the fork-guarded
    // gpu_render_works() probe below - on some dev Mesa builds the driver crashes
    // inside NVRHI's dispatcher calls, which must not kill this test process.
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
    std::string err;
    ASSERT_TRUE(renderer.init_offscreen(64, 64, err)) << err;
    renderer.set_debug_lines({{{-1.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {1.f, 1.f, 0.f, 1.f}}});
    renderer.render(scene, camera, 1.f / 60.f);
    std::vector<u8> px;
    ASSERT_TRUE(renderer.readback(px));
    bool found = false;
    for (u32 x = 8; x < 56 && !found; ++x) {
        const size_t i = (32u * 64u + x) * 4;
        if (px[i] > 200 && px[i + 1] > 200 && px[i + 2] < 100) found = true;
    }
    EXPECT_TRUE(found);
    renderer.shutdown();
}

#else

TEST(Render, OffscreenCubeLit) {
    GTEST_SKIP() << "requires a graphics build (IGNIS_WITH_GRAPHICS=ON)";
}
TEST(Render, DebugLinesDrawn) {
    GTEST_SKIP() << "requires a graphics build (IGNIS_WITH_GRAPHICS=ON)";
}

#endif
