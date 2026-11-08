// Vulkan/NVRHI renderer - full deferred pipeline.
// The GPU path compiles only in graphics builds (IGNIS_RENDERER_VULKAN);
// headless builds get stub implementations so everything still links.
#include "ignis/renderer/vulkan_renderer.h"

#include "ignis/assets/asset_manager.h"
#include "ignis/core/log.h"
#include "ignis/ecs/components.h"
#include "ignis/renderer/ibl.h"
#include "ignis/scene/scene.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

#ifdef IGNIS_RENDERER_VULKAN

#include "ignis/window/window.h"
#include "shaders_embedded.h"
#include "vulkan_dispatch.h"

#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <nvrhi/vulkan.h>

#endif

namespace ignis {

#ifndef IGNIS_RENDERER_VULKAN

// --- Headless stubs -----------------------------------------------------------

struct VulkanRenderer::Impl {};

VulkanRenderer::VulkanRenderer() = default;
VulkanRenderer::~VulkanRenderer() = default;
bool VulkanRenderer::init(Window*, std::string& err) {
    err = "renderer requires a graphics build (IGNIS_WITH_GRAPHICS=ON)";
    return false;
}
void VulkanRenderer::shutdown() {}
void VulkanRenderer::on_resize(u32, u32) {}
void VulkanRenderer::render(Scene&, const Camera&, f32) {}
void VulkanRenderer::set_debug_lines(const std::vector<DebugLine>& lines) { debug_lines_ = lines; }
bool VulkanRenderer::init_offscreen(u32, u32, std::string& err) {
    err = "renderer requires a graphics build";
    return false;
}
bool VulkanRenderer::readback(std::vector<u8>&) { return false; }
u32 VulkanRenderer::offscreen_width() const { return 0; }
u32 VulkanRenderer::offscreen_height() const { return 0; }
bool VulkanRenderer::set_environment_hdr(const std::string&, std::string& err) {
    err = "renderer requires a graphics build";
    return false;
}

#else // IGNIS_RENDERER_VULKAN

using namespace nvrhi;

namespace {

// --- Push constants / UBOs (must match shaders) -------------------------------

struct GbufferPush {
    alignas(16) math::Mat4 model;
    alignas(16) math::Mat4 viewProj;
    alignas(16) math::Vec4 baseColorFactor;
    alignas(8) math::Vec2 metallicRoughness;
    alignas(16) math::Vec4 emissive; // rgb + strength
    u32 texFlags;
    u32 _pad[3];
};

struct ShadowPush {
    alignas(16) math::Mat4 lightViewProj;
    alignas(16) math::Mat4 model;
};

struct LinePush {
    alignas(16) math::Mat4 viewProj;
};

struct LightUBO {
    alignas(16) math::Vec3 camPos;
    f32 _pad0;
    alignas(16) math::Vec3 lightDir;
    f32 lightIntensity;
    alignas(16) math::Vec3 lightColor;
    f32 _pad1;
    alignas(16) math::Mat4 lightViewProj;
    alignas(8) math::Vec2 shadowMapInvSize;
    f32 shadowBias;
    f32 ambientStrength;
    alignas(16) math::Vec4 clearColor;
};

struct SSAOUBO {
    alignas(16) math::Mat4 proj;
    alignas(16) math::Mat4 invProj;
    alignas(16) math::Vec4 kernel[32];
    alignas(8) math::Vec2 noiseScale;
    f32 radius;
    f32 power;
    alignas(8) math::Vec2 screenSize;
    f32 _pad;
};

struct BlurUBO {
    alignas(8) math::Vec2 texelSize;
    alignas(8) math::Vec2 direction;
    f32 _pad;
};

struct TonemapUBO {
    f32 exposure;
    alignas(16) math::Vec4 clearColor;
    f32 _pad0, _pad1;
};

math::Mat4 ortho_vk(f32 l, f32 r, f32 b, f32 t, f32 znear, f32 zfar) {
    math::Mat4 m(1.f);
    m[0][0] = 2.f / (r - l);
    m[1][1] = 2.f / (t - b);
    m[2][2] = 1.f / (znear - zfar);
    m[3][0] = (r + l) / (l - r);
    m[3][1] = (t + b) / (b - t);
    m[3][2] = znear / (znear - zfar);
    return m;
}

// --- Raw Vulkan bootstrap -----------------------------------------------------

bool create_instance(Window* window, VkInstance& out, std::string& err) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Ignis";
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> exts;
    if (window) {
        u32 count = 0;
        const char** glfwExts = glfwGetRequiredInstanceExtensions(&count);
        if (!glfwExts) {
            err = "glfwGetRequiredInstanceExtensions failed (no display?)";
            return false;
        }
        for (u32 i = 0; i < count; ++i) exts.push_back(glfwExts[i]);
    }

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<u32>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();

    if (vkCreateInstance(&ci, nullptr, &out) != VK_SUCCESS) {
        err = "vkCreateInstance failed";
        return false;
    }
    return true;
}

bool pick_physical(VkInstance instance, Window* window, VkSurfaceKHR surface,
                   VkPhysicalDevice& out, u32& queueFamily, std::string& err) {
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) {
        err = "no Vulkan physical devices";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    for (auto dev : devices) {
        u32 qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, qprops.data());
        for (u32 i = 0; i < qcount; ++i) {
            bool graphics = (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            VkBool32 present = VK_FALSE;
            if (window && surface != VK_NULL_HANDLE) {
                vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &present);
            } else {
                present = VK_TRUE;
            }
            if (graphics && present != VK_FALSE) {
                out = dev;
                queueFamily = i;
                return true;
            }
        }
    }
    err = "no suitable GPU (graphics queue) found";
    return false;
}

bool create_logical(VkPhysicalDevice phys, u32 queueFamily, bool windowed,
                    VkDevice& out, VkQueue& outQueue, std::string& err) {
    const f32 priority = 1.f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    std::vector<const char*> exts;
    if (windowed) exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // NVRHI uses timeline semaphores (Queue ctor).
    exts.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);

    VkPhysicalDeviceFeatures feats{};
    feats.samplerAnisotropy = VK_TRUE;

    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timelineFeats{};
    timelineFeats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR;
    timelineFeats.timelineSemaphore = VK_TRUE;

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = static_cast<u32>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.pEnabledFeatures = &feats;
    ci.pNext = &timelineFeats;

    if (vkCreateDevice(phys, &ci, nullptr, &out) != VK_SUCCESS) {
        err = "vkCreateDevice failed";
        return false;
    }
    vkGetDeviceQueue(out, queueFamily, 0, &outQueue);
    return true;
}

bool create_swapchain(VkPhysicalDevice phys, VkDevice device, VkSurfaceKHR surface,
                      Window* window, VkSwapchainKHR& out, VkFormat& outFormat,
                      VkExtent2D& outExtent, std::vector<VkImage>& outImages, std::string& err) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);

    u32 fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmtCount, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        u32 w = 0, h = 0;
        window->framebuffer_size(w, h);
        extent.width = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    u32 imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device, &ci, nullptr, &out) != VK_SUCCESS) {
        err = "vkCreateSwapchainKHR failed";
        return false;
    }
    outFormat = chosen.format;
    outExtent = extent;

    u32 n = 0;
    vkGetSwapchainImagesKHR(device, out, &n, nullptr);
    outImages.resize(n);
    vkGetSwapchainImagesKHR(device, out, &n, outImages.data());
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct VulkanRenderer::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    u32 queueFamily = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapImages;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapExtent{0, 0};
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;

    DeviceHandle dev;

    bool offscreen = false;
    u32 width = 0, height = 0;
    Window* window = nullptr;
    AssetManager* assets = nullptr;
    AssetManager ownAssets;

    ShaderHandle shFullscreenVert, shGbufferVert, shGbufferFrag, shShadowVert, shShadowFrag,
        shSSAOFrag, shBlurFrag, shLightingFrag, shTonemapFrag, shLineVert, shLineFrag;
    InputLayoutHandle meshInputLayout, lineInputLayout;

    SamplerHandle sampLinear, sampNearest, sampShadow;

    GraphicsPipelineHandle psoGbuffer, psoShadow, psoSSAO, psoBlur, psoLighting, psoTonemap, psoLine;

    BindingLayoutHandle layoutPushGbuffer, layoutMaterial, layoutPushShadow, layoutPushLine,
        layoutSSAO, layoutBlur, layoutLightingTex, layoutLightingUBO, layoutTonemap;
    BindingSetHandle setPushGbuffer, setPushShadow, setPushLine; // empty sets for push-only layouts

    TextureHandle texAlbedo, texNormal, texORM, texEmissive, texWorldPos, texDepth;
    FramebufferHandle fbGbuffer;
    TextureHandle texShadow;
    FramebufferHandle fbShadow;
    TextureHandle texSSAO, texSSAOBlurred;
    FramebufferHandle fbSSAO, fbBlur;
    TextureHandle texHDR;
    FramebufferHandle fbLighting;
    TextureHandle texFinal;
    FramebufferHandle fbTonemap;
    std::vector<TextureHandle> swapTexWrapped;
    std::vector<FramebufferHandle> fbSwap;

    TextureHandle texWhite, texBlack, texNormalDefault, texIrradianceCube;

    BufferHandle cbLight, cbSSAO, cbBlur, cbTonemap;
    BindingSetHandle setSSAO, setBlur, setLightingTex, setLightingUBO, setTonemap;

    struct GpuPrimitive { u32 firstIndex; u32 indexCount; u32 materialIndex; };
    struct GpuMesh { BufferHandle vb, ib; std::vector<GpuPrimitive> prims; u32 indexCount = 0; };
    std::unordered_map<std::string, std::shared_ptr<GpuMesh>> meshCache;
    std::unordered_map<std::string, BindingSetHandle> materialSets;
    std::unordered_map<const Texture*, TextureHandle> texCache;

    BufferHandle lineVB;
    u32 lineVertexCount = 0;

    // --- helpers -------------------------------------------------------------

    ShaderHandle loadShader(const char* name, ShaderType type) {
        size_t words = 0;
        const u32* spv = shaders::find(name, words);
        if (!spv || words == 0) {
            IGNIS_ERROR("shader '{}' not embedded (glslangValidator missing?)", name);
            return nullptr;
        }
        ShaderDesc desc;
        desc.setShaderType(type).setEntryName("main").setDebugName(name);
        return dev->createShader(desc, spv, words * sizeof(u32));
    }

    TextureHandle createTex2D(u32 w, u32 h, Format fmt, bool rt, const char* name) {
        TextureDesc d;
        d.width = w;
        d.height = h;
        d.format = fmt;
        d.dimension = TextureDimension::Texture2D;
        d.isRenderTarget = rt;
        d.isShaderResource = true;
        d.debugName = name;
        d.initialState = rt ? ResourceStates::RenderTarget : ResourceStates::ShaderResource;
        d.keepInitialState = true;
        return dev->createTexture(d);
    }

    void runOneShot(std::function<void(ICommandList*)> fn) {
        auto cl = dev->createCommandList();
        cl->open();
        fn(cl);
        cl->close();
        ICommandList* p = cl;
        dev->executeCommandLists(&p, 1);
        dev->waitForIdle();
    }

    TextureHandle uploadTex2D(const Texture& src) {
        auto it = texCache.find(&src);
        if (it != texCache.end()) return it->second;
        TextureDesc d;
        d.width = static_cast<u32>(src.width);
        d.height = static_cast<u32>(src.height);
        d.format = src.hdr ? Format::RGBA16_FLOAT : Format::RGBA8_UNORM;
        d.dimension = TextureDimension::Texture2D;
        d.isShaderResource = true;
        d.debugName = src.name;
        d.initialState = ResourceStates::ShaderResource;
        d.keepInitialState = true;
        auto tex = dev->createTexture(d);
        runOneShot([&](ICommandList* cl) {
            if (src.hdr) {
                cl->writeTexture(tex, 0, 0, src.hdr_pixels.data(), src.hdr_pixels.size() * sizeof(f32));
            } else {
                cl->writeTexture(tex, 0, 0, src.pixels.data(), src.pixels.size());
            }
        });
        texCache[&src] = tex;
        return tex;
    }

    TextureHandle uploadSolidRGBA8(u8 r, u8 g, u8 b, u8 a, const char* name) {
        TextureDesc d;
        d.width = 1;
        d.height = 1;
        d.format = Format::RGBA8_UNORM;
        d.dimension = TextureDimension::Texture2D;
        d.isShaderResource = true;
        d.debugName = name;
        d.initialState = ResourceStates::ShaderResource;
        d.keepInitialState = true;
        auto tex = dev->createTexture(d);
        const u8 px[4] = {r, g, b, a};
        runOneShot([&](ICommandList* cl) { cl->writeTexture(tex, 0, 0, px, 4); });
        return tex;
    }

    BufferHandle createCB(size_t size, const char* name) {
        BufferDesc d;
        d.byteSize = size;
        d.isConstantBuffer = true;
        d.debugName = name;
        return dev->createBuffer(d);
    }

    BindingLayoutHandle makeLayout(std::vector<BindingLayoutItem> items, ShaderType vis) {
        BindingLayoutDesc d;
        d.visibility = vis;
        d.bindings = std::move(items);
        return dev->createBindingLayout(d);
    }

    BindingSetHandle makeSet(BindingLayoutHandle layout, std::vector<BindingSetItem> items) {
        auto set = dev->createDescriptorTable(layout);
        for (auto& it : items) dev->writeDescriptorTable(set, it);
        return set;
    }

    static std::vector<BindingLayoutItem> texSlots(u32 count, u32 startSlot) {
        std::vector<BindingLayoutItem> v;
        for (u32 i = 0; i < count; ++i) {
            v.push_back(BindingLayoutItem::Texture_SRV(startSlot + i));
            v.push_back(BindingLayoutItem::Sampler(startSlot + i));
        }
        return v;
    }

    GraphicsPipelineHandle makePso(ShaderHandle vs, ShaderHandle ps, InputLayoutHandle layout,
                                   std::vector<BindingLayoutHandle> layouts, const FramebufferInfo& fbinfo,
                                   bool depthWrite, bool depthTest, RasterCullMode cull,
                                   f32 slopeBias, PrimitiveType primType = PrimitiveType::TriangleList) {
        GraphicsPipelineDesc d;
        d.primType = primType;
        d.inputLayout = layout;
        d.VS = vs;
        d.PS = ps;
        for (auto& l : layouts) d.addBindingLayout(l);
        RenderState rs;
        rs.depthStencilState.setDepthTestEnable(depthTest).setDepthWriteEnable(depthWrite)
            .setDepthFunc(ComparisonFunc::LessOrEqual);
        rs.rasterState.cullMode = cull;
        rs.rasterState.slopeScaledDepthBias = slopeBias;
        rs.rasterState.depthBias = slopeBias > 0.f ? 2 : 0;
        d.renderState = rs;
        return dev->createGraphicsPipeline(d, fbinfo);
    }

    void createTargets(u32 w, u32 h) {
        width = w;
        height = h;
        texAlbedo = createTex2D(w, h, Format::RGBA8_UNORM, true, "albedo");
        texNormal = createTex2D(w, h, Format::RGBA16_FLOAT, true, "normal");
        texORM = createTex2D(w, h, Format::RGBA8_UNORM, true, "orm");
        texEmissive = createTex2D(w, h, Format::RGBA16_FLOAT, true, "emissive");
        texWorldPos = createTex2D(w, h, Format::RGBA16_FLOAT, true, "worldPos");
        texDepth = createTex2D(w, h, Format::D32, true, "depth");
        FramebufferDesc fb;
        fb.addColorAttachment(texAlbedo).addColorAttachment(texNormal).addColorAttachment(texORM)
            .addColorAttachment(texEmissive).addColorAttachment(texWorldPos).setDepthAttachment(texDepth);
        fbGbuffer = dev->createFramebuffer(fb);

        texShadow = createTex2D(2048, 2048, Format::D32, true, "shadow");
        FramebufferDesc fbs;
        fbs.setDepthAttachment(texShadow);
        fbShadow = dev->createFramebuffer(fbs);

        texSSAO = createTex2D(w, h, Format::R8_UNORM, true, "ssao");
        texSSAOBlurred = createTex2D(w, h, Format::R8_UNORM, true, "ssaoBlur");
        FramebufferDesc fba;
        fba.addColorAttachment(texSSAO);
        fbSSAO = dev->createFramebuffer(fba);
        FramebufferDesc fbb;
        fbb.addColorAttachment(texSSAOBlurred);
        fbBlur = dev->createFramebuffer(fbb);

        texHDR = createTex2D(w, h, Format::RGBA16_FLOAT, true, "hdr");
        FramebufferDesc fbl;
        fbl.addColorAttachment(texHDR);
        fbLighting = dev->createFramebuffer(fbl);

        if (offscreen) {
            texFinal = createTex2D(w, h, Format::RGBA8_UNORM, true, "final");
            FramebufferDesc fbf;
            fbf.addColorAttachment(texFinal);
            fbTonemap = dev->createFramebuffer(fbf);
        }
    }

    void createPipelines(Format finalFormat) {
        // Shaders.
        shFullscreenVert = loadShader("fullscreen_vert", ShaderType::Vertex);
        shGbufferVert = loadShader("gbuffer_vert", ShaderType::Vertex);
        shGbufferFrag = loadShader("gbuffer_frag", ShaderType::Pixel);
        shShadowVert = loadShader("shadow_vert", ShaderType::Vertex);
        shShadowFrag = loadShader("shadow_frag", ShaderType::Pixel);
        shSSAOFrag = loadShader("ssao_frag", ShaderType::Pixel);
        shBlurFrag = loadShader("blur_frag", ShaderType::Pixel);
        shLightingFrag = loadShader("lighting_frag", ShaderType::Pixel);
        shTonemapFrag = loadShader("tonemap_frag", ShaderType::Pixel);
        shLineVert = loadShader("line_vert", ShaderType::Vertex);
        shLineFrag = loadShader("line_frag", ShaderType::Pixel);

        // Input layouts.
        VertexAttributeDesc meshAttrs[4];
        meshAttrs[0].setName("in_pos").setFormat(Format::RGB32_FLOAT).setBufferIndex(0).setOffset(0).setElementStride(48);
        meshAttrs[1].setName("in_normal").setFormat(Format::RGB32_FLOAT).setBufferIndex(0).setOffset(12).setElementStride(48);
        meshAttrs[2].setName("in_uv").setFormat(Format::RG32_FLOAT).setBufferIndex(0).setOffset(24).setElementStride(48);
        meshAttrs[3].setName("in_tangent").setFormat(Format::RGBA32_FLOAT).setBufferIndex(0).setOffset(32).setElementStride(48);
        meshInputLayout = dev->createInputLayout(meshAttrs, 4, shGbufferVert);

        VertexAttributeDesc lineAttrs[2];
        lineAttrs[0].setName("in_pos").setFormat(Format::RGB32_FLOAT).setBufferIndex(0).setOffset(0).setElementStride(28);
        lineAttrs[1].setName("in_color").setFormat(Format::RGBA32_FLOAT).setBufferIndex(0).setOffset(12).setElementStride(28);
        lineInputLayout = dev->createInputLayout(lineAttrs, 2, shLineVert);

        // Samplers.
        SamplerDesc sd;
        sd.minFilter = true;
        sd.magFilter = true;
        sd.mipFilter = false;
        sd.addressU = SamplerAddressMode::Repeat;
        sd.addressV = SamplerAddressMode::Repeat;
        sampLinear = dev->createSampler(sd);
        SamplerDesc sdClamp = sd;
        sdClamp.addressU = SamplerAddressMode::Clamp;
        sdClamp.addressV = SamplerAddressMode::Clamp;
        sampNearest = dev->createSampler(sdClamp);
        SamplerDesc sdShadow = sdClamp;
        sdShadow.reductionType = SamplerReductionType::Comparison;
        sampShadow = dev->createSampler(sdShadow);

        // Binding layouts. GBuffer: set 0 = push constants only, set 1 = material
        // textures (the shader declares them at set=1, binding=0..4).
        layoutPushGbuffer = makeLayout({BindingLayoutItem::PushConstants(0, sizeof(GbufferPush))},
                                       ShaderType::Vertex | ShaderType::Pixel);
        layoutMaterial = makeLayout(texSlots(5, 0), ShaderType::Pixel);
        layoutPushShadow = makeLayout({BindingLayoutItem::PushConstants(0, sizeof(ShadowPush))},
                                      ShaderType::Vertex);
        layoutPushLine = makeLayout({BindingLayoutItem::PushConstants(0, sizeof(LinePush))},
                                    ShaderType::Vertex);
        layoutSSAO = makeLayout({BindingLayoutItem::Texture_SRV(0), BindingLayoutItem::Sampler(0),
                                 BindingLayoutItem::Texture_SRV(1), BindingLayoutItem::Sampler(1),
                                 BindingLayoutItem::ConstantBuffer(2)},
                                ShaderType::Pixel);
        layoutBlur = makeLayout({BindingLayoutItem::Texture_SRV(0), BindingLayoutItem::Sampler(0),
                                 BindingLayoutItem::ConstantBuffer(1)},
                                ShaderType::Pixel);
        layoutLightingTex = makeLayout(texSlots(8, 0), ShaderType::Pixel);
        layoutLightingUBO = makeLayout({BindingLayoutItem::ConstantBuffer(0)}, ShaderType::Pixel);
        layoutTonemap = makeLayout({BindingLayoutItem::Texture_SRV(0), BindingLayoutItem::Sampler(0),
                                    BindingLayoutItem::ConstantBuffer(1)},
                                   ShaderType::Pixel);

        // Framebuffer infos.
        FramebufferInfo fbGbuf;
        fbGbuf.addColorFormat(Format::RGBA8_UNORM).addColorFormat(Format::RGBA16_FLOAT)
            .addColorFormat(Format::RGBA8_UNORM).addColorFormat(Format::RGBA16_FLOAT)
            .addColorFormat(Format::RGBA16_FLOAT).setDepthFormat(Format::D32);
        FramebufferInfo fbDepthOnly;
        fbDepthOnly.setDepthFormat(Format::D32);
        FramebufferInfo fbR8;
        fbR8.addColorFormat(Format::R8_UNORM);
        FramebufferInfo fbHDR;
        fbHDR.addColorFormat(Format::RGBA16_FLOAT);
        FramebufferInfo fbFinal;
        fbFinal.addColorFormat(finalFormat);

        psoGbuffer = makePso(shGbufferVert, shGbufferFrag, meshInputLayout,
                             {layoutPushGbuffer, layoutMaterial}, fbGbuf, true, true,
                             RasterCullMode::Back, 0.f);
        psoShadow = makePso(shShadowVert, shShadowFrag, meshInputLayout,
                            {layoutPushShadow}, fbDepthOnly, true, true, RasterCullMode::Back, 2.f);
        psoSSAO = makePso(shFullscreenVert, shSSAOFrag, nullptr, {layoutSSAO}, fbR8,
                          false, false, RasterCullMode::None, 0.f);
        psoBlur = makePso(shFullscreenVert, shBlurFrag, nullptr, {layoutBlur}, fbR8,
                          false, false, RasterCullMode::None, 0.f);
        psoLighting = makePso(shFullscreenVert, shLightingFrag, nullptr,
                              {layoutLightingTex, layoutLightingUBO}, fbHDR, false, false,
                              RasterCullMode::None, 0.f);
        psoTonemap = makePso(shFullscreenVert, shTonemapFrag, nullptr, {layoutTonemap}, fbFinal,
                             false, false, RasterCullMode::None, 0.f);
        psoLine = makePso(shLineVert, shLineFrag, lineInputLayout, {layoutPushLine}, fbFinal,
                          false, false, RasterCullMode::None, 0.f, PrimitiveType::LineList);
    }

    void createCommonResources() {
        texWhite = uploadSolidRGBA8(255, 255, 255, 255, "white");
        texBlack = uploadSolidRGBA8(0, 0, 0, 255, "black");
        texNormalDefault = uploadSolidRGBA8(128, 128, 255, 255, "normal");

        // Default irradiance cubemap: uniform gray 2x2x6.
        {
            TextureDesc d;
            d.width = 2;
            d.height = 2;
            d.arraySize = 6;
            d.format = Format::RGBA16_FLOAT;
            d.dimension = TextureDimension::TextureCube;
            d.isShaderResource = true;
            d.debugName = "irradiance";
            d.initialState = ResourceStates::ShaderResource;
            d.keepInitialState = true;
            texIrradianceCube = dev->createTexture(d);
            runOneShot([&](ICommandList* cl) {
                for (u32 face = 0; face < 6; ++face) {
                    const f32 px[2 * 2 * 4] = {0.5f, 0.5f, 0.5f, 1.f, 0.5f, 0.5f, 0.5f, 1.f,
                                               0.5f, 0.5f, 0.5f, 1.f, 0.5f, 0.5f, 0.5f, 1.f};
                    TextureSlice slice;
                    slice.arraySlice = face;
                    slice.width = 2;
                    slice.height = 2;
                    cl->writeTexture(texIrradianceCube, slice.arraySlice, 0, px, sizeof(px));
                }
            });
        }

        cbLight = createCB(sizeof(LightUBO), "cbLight");
        cbSSAO = createCB(sizeof(SSAOUBO), "cbSSAO");
        cbBlur = createCB(sizeof(BlurUBO), "cbBlur");
        cbTonemap = createCB(sizeof(TonemapUBO), "cbTonemap");

        // SSAO kernel (deterministic).
        {
            std::mt19937 rng(42);
            std::uniform_real_distribution<f32> dist(0.f, 1.f);
            SSAOUBO ubo{};
            for (int i = 0; i < 32; ++i) {
                math::Vec3 v(dist(rng) * 2.f - 1.f, dist(rng) * 2.f - 1.f, dist(rng));
                v = glm::normalize(v);
                f32 scale = 0.1f + 0.9f * (static_cast<f32>(i) / 32.f) * (static_cast<f32>(i) / 32.f);
                ubo.kernel[i] = math::Vec4(v * scale, 0.f);
            }
            ubo.radius = 0.5f;
            ubo.power = 1.5f;
            runOneShot([&](ICommandList* cl) { cl->writeBuffer(cbSSAO, &ubo, sizeof(ubo)); });
        }

        // Debug line buffer (dynamic).
        {
            BufferDesc d;
            d.byteSize = 4096 * 2 * 28;
            d.isVertexBuffer = true;
            d.cpuAccess = CpuAccessMode::Write;
            d.debugName = "lineVB";
            lineVB = dev->createBuffer(d);
        }

        // Empty binding sets for push-constants-only layouts.
        setPushGbuffer = makeSet(layoutPushGbuffer, {});
        setPushShadow = makeSet(layoutPushShadow, {});
        setPushLine = makeSet(layoutPushLine, {});
    }

    void updateLightingSets() {
        setLightingTex = makeSet(layoutLightingTex, {
            BindingSetItem::Texture_SRV(0, texAlbedo), BindingSetItem::Sampler(0, sampLinear),
            BindingSetItem::Texture_SRV(1, texNormal), BindingSetItem::Sampler(1, sampLinear),
            BindingSetItem::Texture_SRV(2, texORM), BindingSetItem::Sampler(2, sampLinear),
            BindingSetItem::Texture_SRV(3, texEmissive), BindingSetItem::Sampler(3, sampLinear),
            BindingSetItem::Texture_SRV(4, texWorldPos), BindingSetItem::Sampler(4, sampLinear),
            BindingSetItem::Texture_SRV(5, texSSAOBlurred), BindingSetItem::Sampler(5, sampLinear),
            BindingSetItem::Texture_SRV(6, texShadow), BindingSetItem::Sampler(6, sampShadow),
            BindingSetItem::Texture_SRV(7, texIrradianceCube), BindingSetItem::Sampler(7, sampLinear),
        });
        setLightingUBO = makeSet(layoutLightingUBO, {BindingSetItem::ConstantBuffer(0, cbLight)});
        setSSAO = makeSet(layoutSSAO, {
            BindingSetItem::Texture_SRV(0, texDepth), BindingSetItem::Sampler(0, sampNearest),
            BindingSetItem::Texture_SRV(1, texNormal), BindingSetItem::Sampler(1, sampNearest),
            BindingSetItem::ConstantBuffer(2, cbSSAO),
        });
        setBlur = makeSet(layoutBlur, {
            BindingSetItem::Texture_SRV(0, texSSAO), BindingSetItem::Sampler(0, sampLinear),
            BindingSetItem::ConstantBuffer(1, cbBlur),
        });
        setTonemap = makeSet(layoutTonemap, {
            BindingSetItem::Texture_SRV(0, texHDR), BindingSetItem::Sampler(0, sampLinear),
            BindingSetItem::ConstantBuffer(1, cbTonemap),
        });
    }

    // Upload a model into GPU buffers (cached).
    std::shared_ptr<GpuMesh> gpuMesh(const std::string& path) {
        auto it = meshCache.find(path);
        if (it != meshCache.end()) return it->second;
        AssetManager& am = assets ? *assets : ownAssets;
        auto model = am.load_model(path);
        if (!model || !model->mesh || model->mesh->primitives.empty()) return nullptr;

        std::vector<f32> vertices;
        std::vector<u32> indices;
        auto mesh = std::make_shared<GpuMesh>();
        u32 baseVertex = 0;
        for (const auto& prim : model->mesh->primitives) {
            GpuPrimitive gp;
            gp.firstIndex = static_cast<u32>(indices.size());
            gp.materialIndex = prim.material_index;
            const size_t n = prim.positions.size();
            for (size_t i = 0; i < n; ++i) {
                const auto& p = prim.positions[i];
                vertices.push_back(p.x); vertices.push_back(p.y); vertices.push_back(p.z);
                const auto nm = i < prim.normals.size() ? prim.normals[i] : math::Vec3(0.f, 1.f, 0.f);
                vertices.push_back(nm.x); vertices.push_back(nm.y); vertices.push_back(nm.z);
                const auto uv = i < prim.texcoords.size() ? prim.texcoords[i] : math::Vec2(0.f);
                vertices.push_back(uv.x); vertices.push_back(uv.y);
                const auto tg = i < prim.tangents.size() ? prim.tangents[i] : math::Vec4(1.f, 0.f, 0.f, 1.f);
                vertices.push_back(tg.x); vertices.push_back(tg.y); vertices.push_back(tg.z); vertices.push_back(tg.w);
            }
            for (u32 idx : prim.indices) indices.push_back(baseVertex + idx);
            gp.indexCount = static_cast<u32>(prim.indices.size());
            baseVertex += static_cast<u32>(n);
            mesh->prims.push_back(gp);
        }
        mesh->indexCount = static_cast<u32>(indices.size());

        BufferDesc vbDesc;
        vbDesc.byteSize = vertices.size() * sizeof(f32);
        vbDesc.isVertexBuffer = true;
        vbDesc.debugName = "meshVB";
        mesh->vb = dev->createBuffer(vbDesc);
        BufferDesc ibDesc;
        ibDesc.byteSize = indices.size() * sizeof(u32);
        ibDesc.isIndexBuffer = true;
        ibDesc.debugName = "meshIB";
        mesh->ib = dev->createBuffer(ibDesc);
        runOneShot([&](ICommandList* cl) {
            cl->writeBuffer(mesh->vb, vertices.data(), vertices.size() * sizeof(f32));
            cl->writeBuffer(mesh->ib, indices.data(), indices.size() * sizeof(u32));
        });

        meshCache[path] = mesh;
        return mesh;
    }

    BindingSetHandle materialSetFor(const std::string& modelPath, u32 matIndex) {
        const std::string key = modelPath + "|" + std::to_string(matIndex);
        auto it = materialSets.find(key);
        if (it != materialSets.end()) return it->second;
        AssetManager& am = assets ? *assets : ownAssets;
        auto model = am.load_model(modelPath);
        if (!model) return nullptr;
        const Material* mat = matIndex < model->materials.size() ? &model->materials[matIndex] : nullptr;

        auto texOf = [&](int idx) -> TextureHandle {
            const auto nTex = static_cast<int>(model->textures.size());
            if (mat && idx >= 0 && idx < nTex && model->textures[static_cast<size_t>(idx)] &&
                model->textures[static_cast<size_t>(idx)]->valid()) {
                return uploadTex2D(*model->textures[static_cast<size_t>(idx)]);
            }
            return texWhite;
        };
        TextureHandle tBase = texOf(mat ? mat->base_color_texture : -1);
        TextureHandle tMR = texOf(mat ? mat->metallic_roughness_texture : -1);
        TextureHandle tNormal = texOf(mat ? mat->normal_texture : -1);
        if (tNormal == texWhite) tNormal = texNormalDefault;
        TextureHandle tOcc = texOf(mat ? mat->occlusion_texture : -1);
        TextureHandle tEmis = texOf(mat ? mat->emissive_texture : -1);

        auto set = makeSet(layoutMaterial, {
            BindingSetItem::Texture_SRV(0, tBase), BindingSetItem::Sampler(0, sampLinear),
            BindingSetItem::Texture_SRV(1, tMR), BindingSetItem::Sampler(1, sampLinear),
            BindingSetItem::Texture_SRV(2, tNormal), BindingSetItem::Sampler(2, sampLinear),
            BindingSetItem::Texture_SRV(3, tOcc), BindingSetItem::Sampler(3, sampLinear),
            BindingSetItem::Texture_SRV(4, tEmis), BindingSetItem::Sampler(4, sampLinear),
        });
        materialSets[key] = set;
        return set;
    }
};

// --- VulkanRenderer public API -------------------------------------------------

VulkanRenderer::VulkanRenderer() = default;

VulkanRenderer::~VulkanRenderer() {
    shutdown();
}

bool VulkanRenderer::init(Window* window, std::string& err) {
    impl_ = std::make_unique<Impl>();
    auto& I = *impl_;
    I.window = window;
    I.assets = assets_;

    if (!create_instance(window, I.instance, err)) return false;

    // Surface for windowed mode.
    if (window) {
        void* surf = window->create_vulkan_surface(reinterpret_cast<void*>(I.instance), err);
        if (!surf) return false;
        I.surface = reinterpret_cast<VkSurfaceKHR>(surf);
    }

    if (!pick_physical(I.instance, window, I.surface, I.physical, I.queueFamily, err)) return false;
    if (!create_logical(I.physical, I.queueFamily, window != nullptr, I.device, I.queue, err)) return false;

    // NVRHI's static Vulkan backend uses the vulkan-hpp dynamic dispatcher,
    // which must be initialized with our instance/device before createDevice.
    ignis_init_vulkan_dispatch(reinterpret_cast<void*>(I.instance), reinterpret_cast<void*>(I.device));

    u32 w = 1280, h = 720;
    if (window) window->framebuffer_size(w, h);
    if (window) {
        if (!create_swapchain(I.physical, I.device, I.surface, window, I.swapchain, I.swapFormat,
                              I.swapExtent, I.swapImages, err)) {
            return false;
        }
        w = I.swapExtent.width;
        h = I.swapExtent.height;
    }

    // NVRHI device wrapping the raw Vulkan device.
    struct MsgCB : nvrhi::IMessageCallback {
        void message(nvrhi::MessageSeverity, const char* messageText) override {
            IGNIS_ERROR("NVRHI: {}", messageText);
        }
    };
    static MsgCB msgCB;
    nvrhi::vulkan::DeviceDesc desc;
    desc.errorCB = &msgCB;
    desc.instance = I.instance;
    desc.physicalDevice = I.physical;
    desc.device = I.device;
    desc.graphicsQueue = I.queue;
    desc.graphicsQueueIndex = static_cast<int>(I.queueFamily);
    I.dev = nvrhi::vulkan::createDevice(desc);
    if (!I.dev) {
        err = "nvrhi::vulkan::createDevice failed";
        return false;
    }

    I.createTargets(w, h);

    // Wrap swapchain images.
    if (window) {
        // nvrhi::vulkan::convertFormat only maps nvrhi->Vk; do the reverse manually.
        Format nfmt = Format::RGBA8_UNORM;
        if (I.swapFormat == VK_FORMAT_B8G8R8A8_UNORM || I.swapFormat == VK_FORMAT_B8G8R8A8_SRGB) {
            nfmt = Format::BGRA8_UNORM;
        } else if (I.swapFormat == VK_FORMAT_R8G8B8A8_UNORM || I.swapFormat == VK_FORMAT_R8G8B8A8_SRGB) {
            nfmt = Format::RGBA8_UNORM;
        }
        for (VkImage img : I.swapImages) {
            TextureDesc td;
            td.width = I.swapExtent.width;
            td.height = I.swapExtent.height;
            td.format = nfmt;
            td.dimension = TextureDimension::Texture2D;
            td.isRenderTarget = true;
            td.isShaderResource = false;
            td.initialState = ResourceStates::Present;
            td.keepInitialState = true;
            auto tex = I.dev->createHandleForNativeTexture(ObjectTypes::VK_Image, img, td);
            I.swapTexWrapped.push_back(tex);
            FramebufferDesc fb;
            fb.addColorAttachment(tex);
            I.fbSwap.push_back(I.dev->createFramebuffer(fb));
        }
        I.createPipelines(nfmt);
    } else {
        I.createPipelines(Format::RGBA8_UNORM);
    }
    I.createCommonResources();
    I.updateLightingSets();

    // Sync objects.
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(I.device, &sci, nullptr, &I.imageAvailable);
    vkCreateSemaphore(I.device, &sci, nullptr, &I.renderFinished);
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(I.device, &fci, nullptr, &I.inFlight);

    IGNIS_INFO("VulkanRenderer initialized ({}x{}, {})", w, h, window ? "windowed" : "offscreen");
    return true;
}

bool VulkanRenderer::init_offscreen(u32 w, u32 h, std::string& err) {
    impl_ = std::make_unique<Impl>();
    impl_->offscreen = true;
    impl_->assets = assets_;
    if (!init(nullptr, err)) return false;
    // Re-create targets at the requested offscreen size.
    impl_->dev->waitForIdle();
    impl_->createTargets(w, h);
    impl_->updateLightingSets();
    return true;
}

void VulkanRenderer::shutdown() {
    if (!impl_) return;
    auto& I = *impl_;
    if (I.dev) I.dev->waitForIdle();
    impl_.reset();
    if (I.device) {
        if (I.swapchain) vkDestroySwapchainKHR(I.device, I.swapchain, nullptr);
        if (I.imageAvailable) vkDestroySemaphore(I.device, I.imageAvailable, nullptr);
        if (I.renderFinished) vkDestroySemaphore(I.device, I.renderFinished, nullptr);
        if (I.inFlight) vkDestroyFence(I.device, I.inFlight, nullptr);
        vkDestroyDevice(I.device, nullptr);
    }
    if (I.surface) vkDestroySurfaceKHR(I.instance, I.surface, nullptr);
    if (I.instance) vkDestroyInstance(I.instance, nullptr);
}

void VulkanRenderer::on_resize(u32 w, u32 h) {
    // v1: recreate offscreen targets; windowed swapchain recreation on next init.
    if (impl_ && impl_->offscreen && impl_->dev) {
        impl_->dev->waitForIdle();
        impl_->createTargets(w, h);
        impl_->updateLightingSets();
    }
}

bool VulkanRenderer::set_environment_hdr(const std::string& path, std::string& err) {
    if (!impl_ || !impl_->dev) {
        err = "renderer not initialized";
        return false;
    }
    auto& I = *impl_;
    AssetManager& am = I.assets ? *I.assets : I.ownAssets;
    auto hdr = am.load_texture(path);
    if (!hdr || !hdr->hdr) {
        err = "cannot load HDR equirect '" + path + "'";
        return false;
    }
    const Cubemap cube = equirect_to_irradiance(*hdr, 32, 64);
    TextureDesc d;
    d.width = 32;
    d.height = 32;
    d.arraySize = 6;
    d.format = Format::RGBA16_FLOAT;
    d.dimension = TextureDimension::TextureCube;
    d.isShaderResource = true;
    d.debugName = "irradiance";
    d.initialState = ResourceStates::ShaderResource;
        d.keepInitialState = true;
    I.texIrradianceCube = I.dev->createTexture(d);
    const size_t faceFloats = 32u * 32u * 4u;
    I.runOneShot([&](ICommandList* cl) {
        for (u32 face = 0; face < 6; ++face) {
            cl->writeTexture(I.texIrradianceCube, face, 0,
                             cube.faces.data() + face * faceFloats, faceFloats * sizeof(f32));
        }
    });
    I.updateLightingSets();
    IGNIS_INFO("IBL environment set from {}", path);
    return true;
}

u32 VulkanRenderer::offscreen_width() const { return impl_ ? impl_->width : 0; }
u32 VulkanRenderer::offscreen_height() const { return impl_ ? impl_->height : 0; }

bool VulkanRenderer::readback(std::vector<u8>& out) {
    if (!impl_ || !impl_->dev || !impl_->texFinal) return false;
    auto& I = *impl_;
    TextureDesc d;
    d.width = I.width;
    d.height = I.height;
    d.format = Format::RGBA8_UNORM;
    d.dimension = TextureDimension::Texture2D;
    d.isShaderResource = false;
    d.keepInitialState = true;
    d.initialState = ResourceStates::ShaderResource;
    auto staging = I.dev->createStagingTexture(d, CpuAccessMode::Read);
    I.runOneShot([&](ICommandList* cl) {
        cl->copyTexture(staging, TextureSlice(), I.texFinal, TextureSlice());
    });
    size_t rowPitch = 0;
    void* data = I.dev->mapStagingTexture(staging, TextureSlice(), CpuAccessMode::Read, &rowPitch);
    if (!data) return false;
    out.resize(static_cast<size_t>(I.width) * I.height * 4);
    for (u32 y = 0; y < I.height; ++y) {
        std::memcpy(out.data() + static_cast<size_t>(y) * I.width * 4,
                    static_cast<const u8*>(data) + static_cast<size_t>(y) * rowPitch,
                    static_cast<size_t>(I.width) * 4);
    }
    I.dev->unmapStagingTexture(staging);
    return true;
}

void VulkanRenderer::set_debug_lines(const std::vector<DebugLine>& lines) {
    debug_lines_ = lines;
}

void VulkanRenderer::render(Scene& scene, const Camera& camera, f32 dt) {
    (void)dt;
    if (!impl_ || !impl_->dev) return;
    auto& I = *impl_;

    const u32 w = I.width, h = I.height;
    const f32 aspect = h > 0 ? static_cast<f32>(w) / static_cast<f32>(h) : 1.f;
    const math::Mat4 view = camera.view_matrix();
    const math::Mat4 proj = camera.projection_matrix(aspect);
    const math::Mat4 viewProj = proj * view;
    const math::Mat4 invProj = glm::inverse(proj);

    // Light: first DirectionalLightComponent, else default.
    math::Vec3 lightDir = glm::normalize(math::Vec3(0.3f, -1.f, 0.2f));
    math::Vec3 lightColor(1.f);
    f32 lightIntensity = 3.f;
    bool castShadow = true;
    for (EntityId e : scene.world().view<DirectionalLightComponent>()) {
        const auto* l = scene.world().get<DirectionalLightComponent>(e);
        if (l) {
            lightDir = glm::normalize(l->direction);
            lightColor = l->color;
            lightIntensity = l->intensity;
            castShadow = l->cast_shadow;
        }
        break;
    }
    // Light view-projection: ortho box around origin.
    const math::Vec3 lightPos = -lightDir * 50.f;
    const math::Mat4 lightView = math::look_at(lightPos, math::Vec3(0.f), math::Vec3(0.f, 1.f, 0.f));
    const math::Mat4 lightProj = ortho_vk(-30.f, 30.f, -30.f, 30.f, 0.1f, 200.f);
    const math::Mat4 lightViewProj = lightProj * lightView;

    // Acquire swapchain image (windowed) before recording.
    u32 swapIndex = 0;
    if (!I.offscreen && I.swapchain) {
        vkWaitForFences(I.device, 1, &I.inFlight, VK_TRUE, UINT64_MAX);
        vkResetFences(I.device, 1, &I.inFlight);
        VkResult acq = vkAcquireNextImageKHR(I.device, I.swapchain, UINT64_MAX, I.imageAvailable,
                                             VK_NULL_HANDLE, &swapIndex);
        if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
            IGNIS_WARN("vkAcquireNextImageKHR failed: {}", static_cast<int>(acq));
            return;
        }
    }

    auto cl = I.dev->createCommandList();
    cl->open();

    // --- update uniform buffers -------------------------------------------------
    {
        LightUBO lu{};
        lu.camPos = camera.position;
        lu.lightDir = lightDir;
        lu.lightIntensity = lightIntensity;
        lu.lightColor = lightColor;
        lu.lightViewProj = lightViewProj;
        lu.shadowMapInvSize = math::Vec2(1.f / 2048.f);
        lu.shadowBias = 0.005f;
        lu.ambientStrength = 1.f;
        // clear color from active camera component
        lu.clearColor = math::Vec4(0.02f, 0.02f, 0.03f, 1.f);
        for (EntityId e : scene.world().view<CameraComponent>()) {
            if (const auto* c = scene.world().get<CameraComponent>(e)) {
                if (c->active) lu.clearColor = c->clear_color;
            }
            break;
        }
        cl->writeBuffer(I.cbLight, &lu, sizeof(lu));
    }
    {
        SSAOUBO su{};
        su.proj = proj;
        su.invProj = invProj;
        // kernel regenerated identically to createCommonResources
        std::mt19937 rng(42);
        std::uniform_real_distribution<f32> dist(0.f, 1.f);
        for (int i = 0; i < 32; ++i) {
            math::Vec3 v(dist(rng) * 2.f - 1.f, dist(rng) * 2.f - 1.f, dist(rng));
            v = glm::normalize(v);
            f32 scale = 0.1f + 0.9f * (static_cast<f32>(i) / 32.f) * (static_cast<f32>(i) / 32.f);
            su.kernel[i] = math::Vec4(v * scale, 0.f);
        }
        su.noiseScale = math::Vec2(static_cast<f32>(w) / 4.f, static_cast<f32>(h) / 4.f);
        su.radius = 0.5f;
        su.power = 1.5f;
        su.screenSize = math::Vec2(static_cast<f32>(w), static_cast<f32>(h));
        cl->writeBuffer(I.cbSSAO, &su, sizeof(su));
    }
    {
        BlurUBO bu{};
        bu.texelSize = math::Vec2(1.f / static_cast<f32>(w), 1.f / static_cast<f32>(h));
        bu.direction = math::Vec2(1.f, 0.f);
        cl->writeBuffer(I.cbBlur, &bu, sizeof(bu));
    }
    {
        TonemapUBO tu{};
        tu.exposure = 1.f;
        tu.clearColor = math::Vec4(0.02f, 0.02f, 0.03f, 1.f);
        for (EntityId e : scene.world().view<CameraComponent>()) {
            if (const auto* c = scene.world().get<CameraComponent>(e)) {
                if (c->active) tu.clearColor = c->clear_color;
            }
            break;
        }
        cl->writeBuffer(I.cbTonemap, &tu, sizeof(tu));
    }

    // --- gather mesh entities ----------------------------------------------------
    struct DrawItem {
        std::shared_ptr<VulkanRenderer::Impl::GpuMesh> mesh;
        math::Mat4 model;
        std::string modelPath;
    };
    std::vector<DrawItem> draws;
    for (EntityId e : scene.world().view<MeshRendererComponent, TransformComponent>()) {
        const auto* mr = scene.world().get<MeshRendererComponent>(e);
        const auto* tr = scene.world().get<TransformComponent>(e);
        if (!mr || mr->model_path.empty() || !tr) continue;
        auto mesh = I.gpuMesh(mr->model_path);
        if (mesh) draws.push_back({mesh, world_matrix(scene.world(), e), mr->model_path});
    }

    auto setViewport = [&](GraphicsState& gs) {
        ViewportState vs;
        vs.addViewportAndScissorRect(Viewport(static_cast<f32>(w), static_cast<f32>(h)));
        gs.setViewport(vs);
    };

    // --- shadow pass -------------------------------------------------------------
    if (castShadow) {
        cl->clearDepthStencilTexture(I.texShadow, TextureSubresourceSet(), true, 1.f, false, 0);
        for (const auto& d : draws) {
            ShadowPush push{};
            push.lightViewProj = lightViewProj;
            push.model = d.model;
            GraphicsState gs;
            gs.setPipeline(I.psoShadow);
            gs.setFramebuffer(I.fbShadow);
            gs.addBindingSet(I.setPushShadow);
            gs.addVertexBuffer({d.mesh->vb, 0, 0});
            gs.setIndexBuffer({d.mesh->ib, Format::R32_UINT, 0});
            setViewport(gs);
            cl->setGraphicsState(gs);
            cl->setPushConstants(&push, sizeof(push));
            for (const auto& prim : d.mesh->prims) {
                DrawArguments args;
                args.vertexCount = prim.indexCount; // index count for drawIndexed
                args.startIndexLocation = prim.firstIndex;
                cl->drawIndexed(args);
            }
        }
    }

    // --- gbuffer pass ------------------------------------------------------------
    {
        cl->clearTextureFloat(I.texAlbedo, TextureSubresourceSet(), Color(0.f, 0.f, 0.f, 0.f));
        cl->clearTextureFloat(I.texNormal, TextureSubresourceSet(), Color(0.f, 0.f, 0.f, 1.f));
        cl->clearTextureFloat(I.texORM, TextureSubresourceSet(), Color(1.f, 1.f, 0.f, 0.f));
        cl->clearTextureFloat(I.texEmissive, TextureSubresourceSet(), Color(0.f, 0.f, 0.f, 1.f));
        cl->clearTextureFloat(I.texWorldPos, TextureSubresourceSet(), Color(0.f, 0.f, 0.f, 1.f));
        cl->clearDepthStencilTexture(I.texDepth, TextureSubresourceSet(), true, 1.f, false, 0);

        GraphicsState gs;
        gs.setPipeline(I.psoGbuffer);
        gs.setFramebuffer(I.fbGbuffer);
        setViewport(gs);
        for (const auto& d : draws) {
            for (const auto& prim : d.mesh->prims) {
                auto matSet = I.materialSetFor(d.modelPath, prim.materialIndex);
                if (!matSet) continue;
                // Material params from the model.
                AssetManager& am = I.assets ? *I.assets : I.ownAssets;
                auto model = am.load_model(d.modelPath);
                const Material* mat = prim.materialIndex < model->materials.size()
                                          ? &model->materials[prim.materialIndex] : nullptr;
                GbufferPush push{};
                push.model = d.model;
                push.viewProj = viewProj;
                push.baseColorFactor = mat ? mat->base_color : math::Vec4(1.f);
                push.metallicRoughness = mat ? math::Vec2(mat->metallic, mat->roughness) : math::Vec2(0.f, 0.5f);
                push.emissive = mat ? math::Vec4(mat->emissive * mat->emissive_strength, mat->emissive_strength)
                                    : math::Vec4(0.f, 0.f, 0.f, 0.f);
                push.texFlags = 0;
                if (mat) {
                    if (mat->base_color_texture >= 0) push.texFlags |= 1u;
                    if (mat->metallic_roughness_texture >= 0) push.texFlags |= 2u;
                    if (mat->normal_texture >= 0) push.texFlags |= 4u;
                    if (mat->occlusion_texture >= 0) push.texFlags |= 8u;
                    if (mat->emissive_texture >= 0) push.texFlags |= 16u;
                }
                GraphicsState gs2 = gs;
                gs2.addBindingSet(I.setPushGbuffer); // set 0 (push constants)
                gs2.addBindingSet(matSet);           // set 1 (material textures)
                gs2.addVertexBuffer({d.mesh->vb, 0, 0});
                gs2.setIndexBuffer({d.mesh->ib, Format::R32_UINT, 0});
                cl->setGraphicsState(gs2);
                cl->setPushConstants(&push, sizeof(push));
                DrawArguments args;
                args.vertexCount = prim.indexCount;
                args.startIndexLocation = prim.firstIndex;
                cl->drawIndexed(args);
            }
        }
    }

    // --- SSAO + blur --------------------------------------------------------------
    {
        cl->clearTextureFloat(I.texSSAO, TextureSubresourceSet(), Color(1.f, 1.f, 1.f, 1.f));
        GraphicsState gs;
        gs.setPipeline(I.psoSSAO);
        gs.setFramebuffer(I.fbSSAO);
        gs.addBindingSet(I.setSSAO);
        setViewport(gs);
        cl->setGraphicsState(gs);
        DrawArguments args;
        args.vertexCount = 3;
        cl->draw(args); // fullscreen triangle, no vertex/index buffers

        GraphicsState gs2;
        gs2.setPipeline(I.psoBlur);
        gs2.setFramebuffer(I.fbBlur);
        gs2.addBindingSet(I.setBlur);
        setViewport(gs2);
        cl->setGraphicsState(gs2);
        cl->draw(args);
    }

    // --- lighting (HDR) ------------------------------------------------------------
    {
        GraphicsState gs;
        gs.setPipeline(I.psoLighting);
        gs.setFramebuffer(I.fbLighting);
        gs.addBindingSet(I.setLightingTex);
        gs.addBindingSet(I.setLightingUBO);
        setViewport(gs);
        cl->setGraphicsState(gs);
        DrawArguments args;
        args.vertexCount = 3;
        cl->draw(args);
    }

    // --- tonemap into final target ---------------------------------------------------
    FramebufferHandle fbFinal = I.offscreen ? I.fbTonemap : I.fbSwap[swapIndex];
    {
        GraphicsState gs;
        gs.setPipeline(I.psoTonemap);
        gs.setFramebuffer(fbFinal);
        gs.addBindingSet(I.setTonemap);
        setViewport(gs);
        cl->setGraphicsState(gs);
        DrawArguments args;
        args.vertexCount = 3;
        cl->draw(args);
    }

    // --- debug lines -------------------------------------------------------------------
    if (!debug_lines_.empty()) {
        struct LineVertex { math::Vec3 pos; math::Vec4 color; };
        std::vector<LineVertex> verts;
        verts.reserve(debug_lines_.size() * 2);
        for (const auto& l : debug_lines_) {
            verts.push_back({l.a, l.color});
            verts.push_back({l.b, l.color});
        }
        I.lineVertexCount = static_cast<u32>(verts.size());
        cl->writeBuffer(I.lineVB, verts.data(), verts.size() * sizeof(LineVertex));
        GraphicsState gs;
        gs.setPipeline(I.psoLine);
        gs.setFramebuffer(fbFinal);
        gs.addBindingSet(I.setPushLine);
        gs.addVertexBuffer({I.lineVB, 0, 0});
        setViewport(gs);
        cl->setGraphicsState(gs);
        LinePush push{};
        push.viewProj = viewProj;
        cl->setPushConstants(&push, sizeof(push));
        DrawArguments args;
        args.vertexCount = I.lineVertexCount;
        cl->draw(args); // LineList
    }

    cl->close();
    ICommandList* clp = cl;
    I.dev->executeCommandLists(&clp, 1);

    if (!I.offscreen && I.swapchain) {
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &I.imageAvailable;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        VkCommandBuffer cmdBuf = cl->getNativeObject(ObjectTypes::VK_CommandBuffer);
        submit.pCommandBuffers = &cmdBuf;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &I.renderFinished;
        vkQueueSubmit(I.queue, 1, &submit, I.inFlight);
        vkWaitForFences(I.device, 1, &I.inFlight, VK_TRUE, UINT64_MAX);

        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &I.renderFinished;
        pi.swapchainCount = 1;
        pi.pSwapchains = &I.swapchain;
        pi.pImageIndices = &swapIndex;
        vkQueuePresentKHR(I.queue, &pi);
    } else {
        I.dev->waitForIdle();
    }

    stats_.draw_calls = static_cast<u32>(draws.size());
    stats_.triangles = 0;
    for (const auto& d : draws) {
        for (const auto& p : d.mesh->prims) stats_.triangles += p.indexCount / 3;
    }
}

#endif // IGNIS_RENDERER_VULKAN

} // namespace ignis
