// Editor application: window + Vulkan renderer + ImGui overlay (menu bar,
// toolbar, hierarchy with filter + drag&drop reparenting, inspector, console
// with level filters + JSON command input, asset browser, stats graph, status
// bar, viewport overlay, transform gizmo with space/snap) + play-mode
// snapshot/restore + undo/redo (SceneHistory transactions) + AI-agent
// command interface (--commands).
// The ImGui pass is recorded on the swapchain image after the scene render
// (render pass with loadOp=LOAD, initial layout PRESENT_SRC_KHR) and before
// present().
//
// This TU compiles everywhere so the target graph stays valid; run() only
// does real work in graphics builds.
#undef IMGUI_IMPL_VULKAN_NO_PROTOTYPES // we link libvulkan directly, want prototypes
#include "ignis/editor/editor_app.h"

#include "ignis/core/log.h"

#ifdef IGNIS_WITH_GRAPHICS
#include <vulkan/vulkan.h>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder API (docking branch), for the default workspace layout
#include <ImGuizmo.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include "ignis/ecs/component_registry.h"
#include "ignis/assets/asset_types.h"
#include "ignis/core/path_util.h"
#include "ignis/ecs/subtree.h"
#include "ignis/physics/debug_draw.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <vector>
#endif

namespace ignis {

EditorApp::EditorApp() = default;
EditorApp::~EditorApp() = default;

#ifndef IGNIS_WITH_GRAPHICS

int EditorApp::run(const std::string& scene_path, const std::string& commands_path) {
    (void)scene_path;
    (void)commands_path;
    IGNIS_ERROR("editor requires a graphics build (IGNIS_WITH_GRAPHICS=ON)");
    return 1;
}

#else // IGNIS_WITH_GRAPHICS

namespace {

// ImGui's Vulkan backend is compiled with IMGUI_IMPL_VULKAN_NO_PROTOTYPES, so
// it needs an explicit loader. vkGetInstanceProcAddr also resolves device
// functions, which is sufficient for the backend.
PFN_vkVoidFunction imgui_vk_loader(const char* name, void* user_data) {
    return vkGetInstanceProcAddr(static_cast<VkInstance>(user_data), name);
}

void check_vk(VkResult r) {
    if (r != VK_SUCCESS) IGNIS_ERROR("ImGui Vulkan: VkResult={}", static_cast<int>(r));
}

} // namespace

int EditorApp::run(const std::string& scene_path, const std::string& commands_path) {
    // Top-level orchestrator: every phase below is one function, one level of
    // abstraction. Failures are reported here, at the topmost level.
    if (!probe_vulkan_device()) {
        IGNIS_ERROR("Vulkan/NVRHI device not usable on this system (driver crash or no GPU). "
                    "Run the runtime with --headless for simulation-only mode.");
        return 1;
    }
    if (auto r = init_window_and_renderer(); !r.ok()) {
        IGNIS_ERROR("{}", r.error());
        return 1;
    }
    if (auto r = init_scene_and_systems(scene_path, commands_path); !r.ok()) {
        IGNIS_ERROR("{}", r.error());
        return 1;
    }
    scene_path_ = scene_path;
    if (auto r = init_imgui_ui(); !r.ok()) {
        IGNIS_ERROR("ImGui UI init failed: {}", r.error());
        return 1;
    }
    main_loop();
    shutdown_ui();
    IGNIS_INFO("editor shut down cleanly");
    return 0;
}

// Probe the Vulkan stack in a forked child first (some driver stacks crash
// inside NVRHI dispatcher calls - fail with a message, not a segfault). This
// is the topmost place where that dishonesty is handled.
bool EditorApp::probe_vulkan_device() {
    const pid_t pid = fork();
    if (pid == 0) {
        VulkanRenderer probeRenderer;
        if (!probeRenderer.init_offscreen(64, 64).ok()) _exit(2);
        probeRenderer.shutdown();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

Result<void> EditorApp::init_window_and_renderer() {
    WindowProps props;
    props.title = "Ignis Editor";
    props.width = 1280;
    props.height = 720;
    props.resizable = true; // swapchain recreation is handled by the renderer (issue #53)
    window_ = std::make_unique<Window>(props);
    if (auto r = window_->init(); !r.ok())
        return Result<void>::failure("window init failed: " + r.error());
    load_window_settings(); // restore size/maximized state from the last session

    assets_ = std::make_unique<AssetManager>();
    renderer_ = std::make_unique<VulkanRenderer>();
    renderer_->set_asset_manager(assets_.get());
    if (auto r = renderer_->init(window_.get()); !r.ok())
        return Result<void>::failure("renderer init failed: " + r.error());
    // Framebuffer resize -> pending resize in the renderer; the next render()
    // recreates the swapchain after waitForIdle (never inside the callback).
    window_->on_resize = [this](u32 w, u32 h) {
        if (renderer_) renderer_->request_resize(w, h);
    };
    return Result<void>::success();
}

Result<void> EditorApp::init_scene_and_systems(const std::string& scene_path,
                                               const std::string& commands_path) {
    scene_ = std::make_unique<Scene>();
    register_builtin_components(scene_->world());
    PhysicsSystem::register_components(scene_->world());
    AudioSystem::register_components(scene_->world());
    if (auto r = scene_->load_file(scene_path); !r.ok())
        return Result<void>::failure(r.error());
    history_ = std::make_unique<SceneHistory>(*scene_);
    mark_saved();

    scripts_ = std::make_unique<ScriptEngine>();
    if (auto r = scripts_->init(scene_.get(), assets_.get()); !r.ok())
        return Result<void>::failure("script engine init failed: " + r.error());
    audio_ = std::make_unique<AudioEngine>();
    if (auto r = audio_->init(false); !r.ok()) // real audio device in the editor
        IGNIS_WARN("audio init failed ({}), continuing silent", r.error());
    audio_->set_asset_manager(assets_.get()); // pak-aware playback
    AudioEngine::register_lua_bindings(scripts_->state());
    audio_sys_ = std::make_unique<AudioSystem>(*audio_, scene_.get());
    AudioSystem::register_lua_bindings(scripts_->state());
    physics_ = std::make_unique<PhysicsSystem>(*scene_);
    if (auto r = physics_->init(scene_->settings().gravity); !r.ok())
        return Result<void>::failure("physics init failed: " + r.error());
    PhysicsSystem::register_lua_bindings(scripts_->state());
    scene_->add_system([&](World&, f32 dt) { scripts_->update(dt); });
    scene_->add_system([&](World& w, f32 dt) { physics_->update(w, dt); });
    scene_->add_system([&](World& w, f32 dt) { audio_sys_->update(w, dt); });

    // Agent commands (AI-controllable).
    commands_ = std::make_unique<CommandProcessor>(*scene_, *assets_);
    // Scope the AI-agent command interface and Lua path bindings to the
    // project root; when driven by --commands (untrusted input), host paths
    // are disallowed entirely.
    const std::string cwd = std::filesystem::current_path().string();
    commands_->set_project_root(cwd);
    scripts_->set_project_root(cwd);
    if (!commands_path.empty()) {
        commands_->set_host_paths_allowed(false);
        scripts_->set_host_paths_allowed(false);
        auto results = commands_->execute_file(commands_path);
        for (const auto& r : results) IGNIS_INFO("cmd: {} ({})", r.message, r.ok ? "ok" : "FAIL");
    }
    return Result<void>::success();
}

Result<void> EditorApp::init_imgui_ui() {
    // ImGui pass on the swapchain image: render pass with loadOp=LOAD,
    // initial layout PRESENT_SRC_KHR (what NVRHI leaves the image in).
    const RawVulkanHandles rh = renderer_->raw_handles();
    const VkInstance instance = static_cast<VkInstance>(rh.instance);
    const VkPhysicalDevice phys = static_cast<VkPhysicalDevice>(rh.physical_device);
    ui_.device = static_cast<VkDevice>(rh.device);
    ui_.queue = static_cast<VkQueue>(rh.queue);
    VkDevice& device = ui_.device;
    VkQueue& queue = ui_.queue;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // #56: dockable panels
    io.IniFilename = "editor_layout.ini"; // dock layout persists across sessions
    apply_default_layout_ = !std::filesystem::exists(io.IniFilename);
    ImGui::StyleColorsDark();
    base_style_ = ImGui::GetStyle(); // pristine copy for DPI re-scaling
    apply_content_scale(window_->content_scale().x);
    ImGui_ImplGlfw_InitForVulkan(window_->handle(), true);
    ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, imgui_vk_loader, instance);

    {
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
            {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000},
        };
        VkDescriptorPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        ci.maxSets = 1000;
        ci.poolSizeCount = static_cast<u32>(sizeof(sizes) / sizeof(sizes[0]));
        ci.pPoolSizes = sizes;
        vkCreateDescriptorPool(device, &ci, nullptr, &ui_.pool);
    }

    // Swapchain-dependent UI resources (render pass, image views, framebuffers).
    if (auto r = create_ui_swapchain_resources(); !r.ok()) return r;

    {
        VkCommandPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = rh.queue_family;
        vkCreateCommandPool(device, &ci, nullptr, &ui_.cmd_pool);
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = ui_.cmd_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &ai, &ui_.cmd_buffer);
    }

    ImGui_ImplVulkan_InitInfo imguiInfo{};
    imguiInfo.ApiVersion = VK_API_VERSION_1_3;
    imguiInfo.Instance = instance;
    imguiInfo.PhysicalDevice = phys;
    imguiInfo.Device = device;
    imguiInfo.QueueFamily = rh.queue_family;
    imguiInfo.Queue = queue;
    imguiInfo.DescriptorPool = ui_.pool;
    imguiInfo.MinImageCount = 2;
    imguiInfo.ImageCount = ui_.image_count;
    imguiInfo.UseDynamicRendering = false;
    imguiInfo.PipelineInfoMain.RenderPass = ui_.render_pass;
    imguiInfo.PipelineInfoMain.Subpass = 0;
    imguiInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    imguiInfo.CheckVkResultFn = check_vk;
    ImGui_ImplVulkan_Init(&imguiInfo); // font texture is created lazily on first NewFrame
    load_editor_state(); // viewport/camera prefs from the previous session
    load_recent_scenes();  // user-level recent scene list (issue #19)
    return Result<void>::success();
}

// --- swapchain-dependent UI resources (issue #53) ------------------------------

Result<void> EditorApp::create_ui_swapchain_resources() {
    const RawVulkanHandles rh = renderer_->raw_handles();
    if (!rh.windowed || rh.swapchain == nullptr)
        return Result<void>::failure("no swapchain (offscreen renderer?)");
    const VkDevice device = ui_.device;
    const VkSwapchainKHR swapchain = static_cast<VkSwapchainKHR>(rh.swapchain);
    const VkFormat swapFormat = static_cast<VkFormat>(rh.swap_format);
    u32 imageCount = rh.swap_image_count;
    std::vector<VkImage> swapImages(imageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapImages.data());

    // UI render pass: loads the scene-rendered swapchain image (NVRHI leaves
    // it in PRESENT_SRC_KHR thanks to keepInitialState) and keeps it there.
    {
        VkAttachmentDescription att{};
        att.format = swapFormat;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments = &att;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        vkCreateRenderPass(device, &ci, nullptr, &ui_.render_pass);
    }

    ui_.views.resize(imageCount);
    ui_.fbs.resize(imageCount);
    for (u32 i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = swapImages[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swapFormat;
        vi.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                         VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &vi, nullptr, &ui_.views[i]);
        VkFramebufferCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = ui_.render_pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &ui_.views[i];
        fi.width = rh.width;
        fi.height = rh.height;
        fi.layers = 1;
        vkCreateFramebuffer(device, &fi, nullptr, &ui_.fbs[i]);
    }
    ui_.image_count = imageCount;
    ui_.fb_width = rh.width;
    ui_.fb_height = rh.height;
    ui_.format = swapFormat;
    return Result<void>::success();
}

void EditorApp::destroy_ui_swapchain_resources() {
    const VkDevice device = ui_.device;
    if (device == VK_NULL_HANDLE) return;
    for (VkFramebuffer fb : ui_.fbs) vkDestroyFramebuffer(device, fb, nullptr);
    for (VkImageView v : ui_.views) vkDestroyImageView(device, v, nullptr);
    ui_.fbs.clear();
    ui_.views.clear();
    if (ui_.render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, ui_.render_pass, nullptr);
        ui_.render_pass = VK_NULL_HANDLE;
    }
    ui_.image_count = 0;
}

void EditorApp::rebuild_ui_after_swapchain_change() {
    // The ImGui Vulkan backend keeps per-image state sized at Init, and the
    // render pass depends on the surface format - so on swapchain recreation
    // the safe lifecycle is: backend down -> resources down -> up -> up.
    const RawVulkanHandles rh = renderer_->raw_handles();
    ImGui_ImplVulkan_Shutdown();
    destroy_ui_swapchain_resources();
    if (auto r = create_ui_swapchain_resources(); !r.ok()) {
        IGNIS_ERROR("UI swapchain resource rebuild failed: {}", r.error());
        return;
    }
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = static_cast<VkInstance>(rh.instance);
    info.PhysicalDevice = static_cast<VkPhysicalDevice>(rh.physical_device);
    info.Device = ui_.device;
    info.QueueFamily = rh.queue_family;
    info.Queue = ui_.queue;
    info.DescriptorPool = ui_.pool;
    info.MinImageCount = 2;
    info.ImageCount = ui_.image_count;
    info.UseDynamicRendering = false;
    info.PipelineInfoMain.RenderPass = ui_.render_pass;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = check_vk;
    ImGui_ImplVulkan_Init(&info);
    IGNIS_INFO("editor UI resources rebuilt for {}x{} ({} swapchain images)", ui_.fb_width,
               ui_.fb_height, ui_.image_count);
}

void EditorApp::apply_content_scale(f32 scale) {
    if (scale <= 0.f) scale = 1.f;
    content_scale_ = scale;
    // Reset to the pristine style first so repeated DPI changes don't accumulate.
    ImGui::GetStyle() = base_style_;
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetIO().FontGlobalScale = scale;
}

void EditorApp::load_window_settings() {
    std::ifstream f("editor_window.json");
    if (!f || !window_ || !window_->handle()) return;
    try {
        const auto j = nlohmann::json::parse(f);
        const int w = j.value("width", 1280);
        const int h = j.value("height", 720);
        const bool maximized = j.value("maximized", false);
        glfwSetWindowSize(window_->handle(), w, h);
        if (maximized) glfwMaximizeWindow(window_->handle());
    } catch (const std::exception& e) {
        IGNIS_WARN("ignoring corrupt editor_window.json: {}", e.what());
    }
}

void EditorApp::save_window_settings() const {
    if (!window_ || !window_->handle()) return;
    int w = 1280, h = 720;
    glfwGetWindowSize(window_->handle(), &w, &h);
    const bool maximized = glfwGetWindowAttrib(window_->handle(), GLFW_MAXIMIZED) == GLFW_TRUE;
    const nlohmann::json j = {{"width", w}, {"height", h}, {"maximized", maximized}};
    std::ofstream f("editor_window.json");
    if (f) f << j.dump(2);
}

// --- per-component inspector editors (one function per component type) -------

namespace {

void edit_transform(TransformComponent& t) {
    ImGui::DragFloat3("position", &t.position.x, 0.05f);
    math::Vec4 q{t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z};
    if (ImGui::DragFloat4("rotation (wxyz)", &q.x, 0.01f))
        t.rotation = glm::normalize(math::Quat(q.x, q.y, q.z, q.w));
    ImGui::DragFloat3("scale", &t.scale.x, 0.05f, 0.01f, 100.f);
}

void edit_camera(CameraComponent& c) {
    ImGui::DragFloat("fov_y_deg", &c.fov_y_deg, 0.5f, 10.f, 170.f);
    ImGui::DragFloat("z_near", &c.z_near, 0.01f, 0.01f, 10.f);
    ImGui::DragFloat("z_far", &c.z_far, 1.f, 1.f, 100000.f);
    ImGui::ColorEdit4("clear_color", &c.clear_color.x);
    ImGui::Checkbox("active", &c.active);
}

void edit_directional_light(DirectionalLightComponent& l) {
    ImGui::DragFloat3("direction", &l.direction.x, 0.01f);
    ImGui::ColorEdit3("color", &l.color.x);
    ImGui::DragFloat("intensity", &l.intensity, 0.05f, 0.f, 100.f);
    ImGui::Checkbox("cast_shadow", &l.cast_shadow);
}

void edit_point_light(PointLightComponent& l) {
    ImGui::ColorEdit3("color", &l.color.x);
    ImGui::DragFloat("intensity", &l.intensity, 0.1f, 0.f, 1000.f);
    ImGui::DragFloat("range", &l.range, 0.1f, 0.1f, 1000.f);
}

void edit_spot_light(SpotLightComponent& l) {
    ImGui::DragFloat3("direction", &l.direction.x, 0.01f);
    ImGui::ColorEdit3("color", &l.color.x);
    ImGui::DragFloat("intensity", &l.intensity, 0.1f, 0.f, 1000.f);
    ImGui::DragFloat("range", &l.range, 0.1f, 0.1f, 1000.f);
    ImGui::DragFloat("inner_angle_deg", &l.inner_angle_deg, 0.5f, 0.f, 90.f);
    ImGui::DragFloat("outer_angle_deg", &l.outer_angle_deg, 0.5f, 0.f, 90.f);
}

void edit_mesh_renderer(MeshRendererComponent& m) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", m.model_path.c_str());
    if (ImGui::InputText("model_path", buf, sizeof(buf))) m.model_path = buf;
}

void edit_script(ScriptComponent& s) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", s.path.c_str());
    if (ImGui::InputText("path", buf, sizeof(buf))) s.path = buf;
    ImGui::Checkbox("enabled", &s.enabled);
}

void edit_rigid_body(RigidBodyComponent& rb) {
    const char* items[] = {"Static", "Kinematic", "Dynamic"};
    int cur = static_cast<int>(rb.type);
    if (ImGui::Combo("type", &cur, items, 3)) rb.type = static_cast<RigidBodyComponent::Type>(cur);
    if (rb.type == RigidBodyComponent::Type::Dynamic)
        ImGui::DragFloat("mass", &rb.mass, 0.1f, 0.01f, 10000.f);
    ImGui::DragFloat("linear_damping", &rb.linear_damping, 0.01f, 0.f, 10.f);
}

void edit_collider(ColliderComponent& c) {
    const char* items[] = {"Box", "Sphere", "Capsule"};
    int cur = static_cast<int>(c.shape);
    if (ImGui::Combo("shape", &cur, items, 3)) c.shape = static_cast<ColliderComponent::Shape>(cur);
    if (c.shape == ColliderComponent::Shape::Box)
        ImGui::DragFloat3("half_extents", &c.half_extents.x, 0.05f, 0.01f, 1000.f);
    else
        ImGui::DragFloat("radius", &c.radius, 0.05f, 0.01f, 1000.f);
    if (c.shape == ColliderComponent::Shape::Capsule)
        ImGui::DragFloat("height", &c.height, 0.05f, 0.01f, 1000.f);
}

ImVec4 log_color(LogLevel l) {
    switch (l) {
        case LogLevel::Error:
        case LogLevel::Fatal: return ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
        case LogLevel::Warn:  return ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
        case LogLevel::Debug:
        case LogLevel::Trace: return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        default:              return ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
    }
}

void item_tooltip(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

const char* const kGameobjectKinds[] = {"Empty", "Cube", "Camera", "Directional Light",
                                        "Point Light", "Audio Source"};
const char* const kGameobjectTips[] = {
    "create an entity with only a Transform component",
    "create an entity with Transform + MeshRenderer (assets/cube.gltf)",
    "create an entity with Transform + Camera",
    "create an entity with Transform + DirectionalLight",
    "create an entity with Transform + PointLight",
    "create an entity with Transform + AudioSource",
};
constexpr u32 kGameobjectKindCount = static_cast<u32>(sizeof(kGameobjectKinds) / sizeof(kGameobjectKinds[0]));

} // namespace

// --- main loop: one frame = input, simulate, UI panels, render ----------------

EntityId EditorApp::find_active_camera() const {
    for (EntityId e : scene_->world().all_entities()) {
        if (const auto* cc = scene_->world().get<CameraComponent>(e))
            if (cc->active) return e;
    }
    return NULL_ENTITY;
}

void EditorApp::update_camera(Camera& camera, EntityId cam_entity) const {
    camera = Camera{};
    if (cam_entity != NULL_ENTITY && scene_->world().alive(cam_entity)) {
        const auto* cc = scene_->world().get<CameraComponent>(cam_entity);
        const auto* tc = scene_->world().get<TransformComponent>(cam_entity);
        if (cc && tc) {
            camera.position = tc->position;
            camera.rotation = tc->rotation;
            camera.fov_y_deg = cc->fov_y_deg;
            camera.z_near = cc->z_near;
            camera.z_far = cc->z_far;
        }
    }
}

void EditorApp::publish_view_camera(Camera& camera, EntityId cam_entity) {
    if (standalone_cam_) {
        camera = editor_cam_;
    } else {
        update_camera(camera, cam_entity);
    }
    // The projection mode is editor (viewport) state, applied on top of
    // whichever camera is rendered; it never touches the scene camera.
    camera.orthographic = editor_ortho_;
    camera.ortho_size = editor_ortho_size_;
}

void EditorApp::set_view_pose(const math::Vec3& pos, const math::Quat& rot, EntityId cam_entity) {
    if (standalone_cam_) {
        editor_cam_.position = pos;
        editor_cam_.rotation = rot;
    } else if (cam_entity != NULL_ENTITY && scene_->world().alive(cam_entity)) {
        if (auto* tc = scene_->world().get<TransformComponent>(cam_entity)) {
            tc->position = pos;
            tc->rotation = rot;
        }
    }
}

void EditorApp::apply_view_offset(const math::Vec3& delta, EntityId cam_entity) {
    if (standalone_cam_) {
        editor_cam_.position += delta;
    } else if (cam_entity != NULL_ENTITY && scene_->world().alive(cam_entity)) {
        if (auto* tc = scene_->world().get<TransformComponent>(cam_entity)) tc->position += delta;
    }
}

void EditorApp::apply_view_rotation(const math::Quat& rot, EntityId cam_entity) {
    if (standalone_cam_) {
        editor_cam_.rotation = rot;
    } else if (cam_entity != NULL_ENTITY && scene_->world().alive(cam_entity)) {
        if (auto* tc = scene_->world().get<TransformComponent>(cam_entity)) tc->rotation = rot;
    }
}

void EditorApp::sync_yaw_pitch_from(const math::Quat& rot) {
    const math::Vec3 fwd = rot * math::Vec3(0.f, 0.f, -1.f);
    cam_yaw_ = std::atan2(-fwd.x, -fwd.z);
    cam_pitch_ = glm::clamp(std::asin(glm::clamp(fwd.y, -1.f, 1.f)), -1.55f, 1.55f);
}

void EditorApp::set_standalone_camera(bool on, EntityId cam_entity) {
    if (on == standalone_cam_) return;
    standalone_cam_ = on;
    if (on) {
        // Start the editor camera from the current view so nothing jumps.
        editor_cam_ = view_cam_;
        sync_yaw_pitch_from(editor_cam_.rotation);
        IGNIS_INFO("standalone editor camera ON (scene camera untouched)");
    } else {
        // Sync the editor camera from the scene camera so the next toggle
        // continues from there; the viewport itself returns to the scene
        // camera because it was never modified while standalone.
        if (cam_entity != NULL_ENTITY && scene_->world().alive(cam_entity)) {
            update_camera(editor_cam_, cam_entity);
            sync_yaw_pitch_from(editor_cam_.rotation);
        }
        IGNIS_INFO("standalone editor camera OFF (view = scene camera)");
    }
    save_editor_state();
}

void EditorApp::load_editor_state() {
    std::ifstream in("editor_state.json");
    if (!in) return; // first run: keep defaults
    try {
        const nlohmann::json j = nlohmann::json::parse(in);
        standalone_cam_ = j.value("standalone_camera", false);
        editor_ortho_ = j.value("ortho", false);
        editor_ortho_size_ = j.value("ortho_size", 10.f);
        cam_speed_ = j.value("fly_speed", 6.f);
        dbg_colliders_ = j.value("debug_colliders", true);
        dbg_selection_ = j.value("debug_selection", true);
        dbg_grid_ = j.value("debug_grid", false);
        dbg_axes_ = j.value("debug_axes", false);
        if (j.contains("editor_cam")) {
            const nlohmann::json& c = j["editor_cam"];
            const auto vec3 = [](const nlohmann::json& a) {
                return math::Vec3(a.at(0).get<f32>(), a.at(1).get<f32>(), a.at(2).get<f32>());
            };
            editor_cam_.position = vec3(c.at("position"));
            const nlohmann::json& r = c.at("rotation"); // w,x,y,z
            editor_cam_.rotation = glm::normalize(math::Quat(r.at(0).get<f32>(), r.at(1).get<f32>(),
                                                             r.at(2).get<f32>(), r.at(3).get<f32>()));
            editor_cam_.fov_y_deg = c.value("fov_y_deg", 60.f);
            editor_cam_.z_near = c.value("z_near", 0.1f);
            editor_cam_.z_far = c.value("z_far", 1000.f);
        }
    } catch (const std::exception& e) {
        IGNIS_WARN("editor_state.json unreadable ({}), using defaults", e.what());
    }
}

void EditorApp::save_editor_state() const {
    nlohmann::json j;
    j["standalone_camera"] = standalone_cam_;
    j["ortho"] = editor_ortho_;
    j["ortho_size"] = editor_ortho_size_;
    j["fly_speed"] = cam_speed_;
    j["debug_colliders"] = dbg_colliders_;
    j["debug_selection"] = dbg_selection_;
    j["debug_grid"] = dbg_grid_;
    j["debug_axes"] = dbg_axes_;
    j["editor_cam"] = {{"position", {editor_cam_.position.x, editor_cam_.position.y, editor_cam_.position.z}},
                       {"rotation", {editor_cam_.rotation.w, editor_cam_.rotation.x, editor_cam_.rotation.y,
                                     editor_cam_.rotation.z}},
                       {"fov_y_deg", editor_cam_.fov_y_deg},
                       {"z_near", editor_cam_.z_near},
                       {"z_far", editor_cam_.z_far}};
    std::ofstream out("editor_state.json");
    if (!out) {
        IGNIS_WARN("could not write editor_state.json");
        return;
    }
    out << j.dump(2);
}

void EditorApp::handle_viewport_input(Camera& camera, EntityId cam_entity, f32 dt) {
    ImGuiIO& io = ImGui::GetIO();
    GLFWwindow* glfw = window_->handle();
    const bool rmb = glfwGetMouseButton(glfw, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(glfw, &mx, &my);
    if (rmb && !io.WantCaptureMouse) {
        if (rmb_down_) {
            cam_yaw_ -= static_cast<f32>(mx - last_mx_) * 0.005f;
            cam_pitch_ -= static_cast<f32>(my - last_my_) * 0.005f;
            cam_pitch_ = glm::clamp(cam_pitch_, -1.55f, 1.55f);
        }
        rmb_down_ = true;
    } else {
        rmb_down_ = false;
        // Pick up external camera changes (inspector edits, scripts, play mode)
        // so the next RMB drag continues from the actual view orientation.
        sync_yaw_pitch_from(camera.rotation);
    }
    last_mx_ = mx;
    last_my_ = my;
    // Fly camera: RMB drag to look, WASDQE to move. The motion drives the
    // standalone editor camera when enabled, otherwise the scene camera
    // entity's transform (so the camera stays editable like any object).
    if (rmb_down_) {
        const math::Quat rot = glm::angleAxis(cam_yaw_, math::Vec3(0.f, 1.f, 0.f)) *
                               glm::angleAxis(cam_pitch_, math::Vec3(1.f, 0.f, 0.f));
        const math::Vec3 fwd = rot * math::Vec3(0.f, 0.f, -1.f);
        const math::Vec3 right = rot * math::Vec3(1.f, 0.f, 0.f);
        const math::Vec3 up = math::Vec3(0.f, 1.f, 0.f);
        const f32 speed = cam_speed_ * (glfwGetKey(glfw, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 4.f : 1.f);
        math::Vec3 delta(0.f);
        if (glfwGetKey(glfw, GLFW_KEY_W) == GLFW_PRESS) delta += fwd;
        if (glfwGetKey(glfw, GLFW_KEY_S) == GLFW_PRESS) delta -= fwd;
        if (glfwGetKey(glfw, GLFW_KEY_D) == GLFW_PRESS) delta += right;
        if (glfwGetKey(glfw, GLFW_KEY_A) == GLFW_PRESS) delta -= right;
        if (glfwGetKey(glfw, GLFW_KEY_E) == GLFW_PRESS) delta += up;
        if (glfwGetKey(glfw, GLFW_KEY_Q) == GLFW_PRESS) delta -= up;
        delta *= speed * dt;
        apply_view_rotation(rot, cam_entity);
        apply_view_offset(delta, cam_entity);
    }
    // Mouse wheel: dolly along the view direction (perspective) or zoom the
    // orthographic view size, whenever the pointer is over the viewport.
    if (scroll_pending_ != 0.0) {
        if (!io.WantCaptureMouse) {
            const f32 y = static_cast<f32>(scroll_pending_);
            if (editor_ortho_) {
                editor_ortho_size_ = glm::clamp(editor_ortho_size_ * std::exp(-y * 0.1f), 0.01f, 100000.f);
            } else {
                const math::Vec3 fwd = camera.rotation * math::Vec3(0.f, 0.f, -1.f);
                apply_view_offset(fwd * (y * cam_speed_ * 0.5f), cam_entity);
            }
        }
        scroll_pending_ = 0.0;
    }
    publish_view_camera(camera, cam_entity);

    // Click / rubber-band marquee selection, unless the click went to the UI.
    // LMB press records the start; a drag past ~5px becomes a marquee; release
    // applies either the marquee (projected-AABB rect intersection) or a
    // single ray pick. Modifiers: none = replace, Ctrl = toggle/add,
    // Ctrl+Shift = remove.
    const bool lmb = glfwGetMouseButton(glfw, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (lmb && !left_clicked_ && !io.WantCaptureMouse) {
        lmb_down_viewport_ = true;
        lmb_press_ = {static_cast<f32>(mx), static_cast<f32>(my)};
        marquee_active_ = false;
        marquee_end_ = lmb_press_;
    }
    if (lmb && lmb_down_viewport_) {
        marquee_end_ = {static_cast<f32>(mx), static_cast<f32>(my)};
        const math::Vec2 d = marquee_end_ - lmb_press_;
        if (glm::length(d) > 5.f) marquee_active_ = true;
    }
    if (!lmb && left_clicked_ && lmb_down_viewport_) {
        const bool add = io.KeyCtrl && !io.KeyShift;
        const bool remove = io.KeyCtrl && io.KeyShift;
        if (marquee_active_) {
            picking::ScreenRect rect;
            rect.min_x = std::min(lmb_press_.x, marquee_end_.x);
            rect.min_y = std::min(lmb_press_.y, marquee_end_.y);
            rect.max_x = std::max(lmb_press_.x, marquee_end_.x);
            rect.max_y = std::max(lmb_press_.y, marquee_end_.y);
            rect.valid = true;
            marquee_select(rect, add, remove);
        } else {
            const FramebufferSize fb = window_->framebuffer_size();
            if (fb.width > 0 && fb.height > 0) {
                const f32 ndcX = (static_cast<f32>(mx) / static_cast<f32>(fb.width)) * 2.f - 1.f;
                const f32 ndcY = (static_cast<f32>(my) / static_cast<f32>(fb.height)) * 2.f - 1.f;
                const picking::Ray ray = picking::screen_ray(
                    camera, static_cast<f32>(fb.width) / static_cast<f32>(fb.height), ndcX, -ndcY);
                f32 bestT = 1e30f;
                EntityId best = NULL_ENTITY;
                for (EntityId e : scene_->world().all_entities()) {
                    if (!scene_->world().get<MeshRendererComponent>(e)) continue;
                    const auto* tc = scene_->world().get<TransformComponent>(e);
                    if (!tc) continue;
                    const math::Mat4 wm = world_matrix(scene_->world(), e);
                    const math::Vec3 center{static_cast<f32>(wm[3][0]), static_cast<f32>(wm[3][1]),
                                            static_cast<f32>(wm[3][2])};
                    const math::Vec3 half = tc->scale * 0.5f;
                    const auto t = picking::ray_aabb(ray.origin, ray.dir, center, half);
                    if (t.has_value() && *t < bestT) {
                        bestT = *t;
                        best = e;
                    }
                }
                if (remove) {
                    if (best != NULL_ENTITY) select_remove(best);
                } else if (add) {
                    if (best != NULL_ENTITY) select_toggle(best);
                } else if (best != NULL_ENTITY) {
                    select_replace(best);
                } else {
                    clear_selection(); // plain click on empty space clears
                }
            }
        }
        lmb_down_viewport_ = false;
        marquee_active_ = false;
    }
    left_clicked_ = lmb;
}

void EditorApp::handle_shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) return;
    const bool has_sel = selected_ != NULL_ENTITY && scene_->world().alive(selected_);
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmo_op_ = GizmoOp::Translate;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmo_op_ = GizmoOp::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmo_op_ = GizmoOp::Scale;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && has_sel) delete_selected();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && has_sel) duplicate_selected();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && has_sel) copy_selection_to_clipboard();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) paste_clipboard_entities();
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        if (io.KeyShift) frame_scene();
        else if (has_sel) frame_selected();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) save_scene();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) prompt_if_dirty(2);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) {
        show_palette_ = !show_palette_;
        if (show_palette_) palette_idx_ = 0;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (io.KeyShift) redo();
        else undo();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        if (playing_) stop_play();
        else start_play();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (show_palette_) show_palette_ = false;
        else request_quit();
    }
}

// --- scene actions (shared by menu bar, toolbar and keyboard shortcuts) ------

void EditorApp::update_window_title() {
    glfwSetWindowTitle(window_->handle(),
                       ("Ignis Editor - " + scene_->name() + (dirty_ ? "*" : "") + "  [" + scene_path_ + "]")
                           .c_str());
}

// The scene is "clean" when it matches the snapshot taken at last save/load.
// Compared by content (not a mutation flag), so modify->undo reports clean.
void EditorApp::mark_saved() {
    saved_snapshot_ = scene_->serialize();
    dirty_ = false;
    update_window_title();
}

bool EditorApp::save_scene() {
    auto r = scene_->save_to_file(scene_path_);
    if (!r.ok()) {
        IGNIS_ERROR("{}", r.error());
        return false;
    }
    IGNIS_INFO("scene saved to {}", scene_path_);
    mark_saved();
    push_recent_scene(scene_path_);
    return true;
}

// --- issue #19: quit/reload/new/open prompt when dirty, recent scenes ---------

void EditorApp::request_quit() {
    if (dirty_) {
        save_prompt_open_ = true;
        save_prompt_action_ = 0;
    } else {
        window_->request_close();
    }
}

void EditorApp::prompt_if_dirty(int action, const std::string& open_path) {
    save_prompt_action_ = action;
    pending_open_path_ = open_path;
    if (dirty_)
        save_prompt_open_ = true;
    else
        perform_pending_action();
}

void EditorApp::perform_pending_action() {
    switch (save_prompt_action_) {
        case 0: window_->request_close(); break;
        case 1: reload_scene(); break;
        case 2: new_scene(); break;
        case 3: open_scene(pending_open_path_); break;
        default: break;
    }
    save_prompt_action_ = -1;
}

void EditorApp::draw_save_prompt() {
    if (!save_prompt_open_) return;
    ImGui::OpenPopup("Unsaved changes");
    if (ImGui::BeginPopupModal("Unsaved changes", &save_prompt_open_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The scene '%s' has unsaved changes.", scene_->name().c_str());
        if (ImGui::Button("Save")) {
            if (save_scene()) {
                save_prompt_open_ = false;
                perform_pending_action();
                ImGui::CloseCurrentPopup();
            }
        }
        item_tooltip("save, then continue");
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            save_prompt_open_ = false;
            perform_pending_action();
            ImGui::CloseCurrentPopup();
        }
        item_tooltip("continue without saving");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            save_prompt_open_ = false;
            save_prompt_action_ = -1;
            ImGui::CloseCurrentPopup();
        }
        item_tooltip("go back to editing");
        ImGui::EndPopup();
    }
}

std::string EditorApp::recent_scenes_path() {
    // User-level config, not the CWD (roadmap review): ~/.ignis_editor_recent.json
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') return std::string(home) + "/.ignis_editor_recent.json";
    return "editor_recent.json";
}

void EditorApp::load_recent_scenes() {
    std::ifstream in(recent_scenes_path());
    if (!in) return;
    try {
        const auto j = nlohmann::json::parse(in);
        if (j.is_array())
            for (const auto& e : j)
                if (e.is_string()) recent_scenes_.push_back(e.get<std::string>());
    } catch (const std::exception& e) {
        IGNIS_WARN("recent scenes unreadable: {}", e.what());
    }
}

void EditorApp::save_recent_scenes() {
    nlohmann::json j = nlohmann::json::array();
    for (const auto& p : recent_scenes_) j.push_back(p);
    std::ofstream out(recent_scenes_path());
    if (out) out << j.dump(2);
}

void EditorApp::push_recent_scene(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string canon = fs::weakly_canonical(path, ec).generic_string();
    const std::string p = ec ? path : canon; // canonicalized absolute path
    std::erase(recent_scenes_, p);
    recent_scenes_.insert(recent_scenes_.begin(), p);
    if (recent_scenes_.size() > 8) recent_scenes_.resize(8);
    save_recent_scenes();
}

// --- issue #19: command palette (Ctrl+K) ---------------------------------------

void EditorApp::run_palette_command(const CommandInfo& ci) {
    nlohmann::json tpl;
    tpl["cmd"] = ci.name;
    for (const auto& p : ci.params) tpl[p] = (p == "entity" || p == "parent") ? 0 : "";
    const std::string text = tpl.dump();
    std::snprintf(console_input_, sizeof(console_input_), "%s", text.c_str());
    show_palette_ = false;
    show_bottom_ = true;
    bottom_tab_ = 0; // console tab
    console_focus_pending_ = true;
    IGNIS_INFO("palette: prepared '{}' in the console input", ci.name);
}

void EditorApp::draw_palette() {
    if (!show_palette_) return;
    ImGui::SetNextWindowSize(ImVec2(560.f, 400.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Command palette", &show_palette_, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowAppearing()) {
        palette_filter_[0] = '\0';
        palette_idx_ = 0;
        ImGui::SetKeyboardFocusHere();
    }
    const bool enter = ImGui::InputTextWithHint("##palette_filter", "type to filter... (Ctrl+K closes)",
                                                palette_filter_, sizeof(palette_filter_),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    struct Item {
        std::string label;
        std::string tip;
        std::function<void()> run;
    };
    const bool has_sel = selected_ != NULL_ENTITY && scene_->world().alive(selected_);
    std::vector<Item> items;
    items.push_back({"Play / Pause", "P - snapshot + simulate, or restore", [this] {
                         if (playing_) stop_play();
                         else start_play();
                     }});
    items.push_back({"Stop (restore pre-play state)", "stop simulation and restore the snapshot", [this] { stop_play(); }});
    items.push_back({"Save", "Ctrl+S", [this] { save_scene(); }});
    items.push_back({"New scene", "Ctrl+N - prompt if unsaved changes", [this] { prompt_if_dirty(2); }});
    items.push_back({"Reload from file", "discard unsaved changes", [this] { prompt_if_dirty(1); }});
    items.push_back({"Undo", "Ctrl+Z", [this] { undo(); }});
    items.push_back({"Redo", "Ctrl+Shift+Z / Ctrl+Y", [this] { redo(); }});
    if (has_sel) {
        items.push_back({"Delete selected", "Del", [this] { delete_selected(); }});
        items.push_back({"Duplicate selected (deep)", "Ctrl+D", [this] { duplicate_selected(); }});
        items.push_back({"Frame selected", "F", [this] { frame_selected(); }});
    }
    items.push_back({"Frame scene", "Shift+F - fit all entities", [this] { frame_scene(); }});
    items.push_back({"Toggle standalone editor camera", "fly without touching the scene camera", [this] {
                         set_standalone_camera(!standalone_cam_, find_active_camera());
                     }});
    items.push_back({"Toggle orthographic projection", "viewport projection mode", [this] {
                         editor_ortho_ = !editor_ortho_;
                         save_editor_state();
                     }});
    items.push_back({"Clear console", "", [this] {
                         std::lock_guard lock(console_mutex_);
                         console_.clear();
                     }});
    for (const auto& ci : CommandProcessor::known_commands()) {
        std::string params;
        for (const auto& p : ci.params) {
            if (!params.empty()) params += ", ";
            params += p;
        }
        items.push_back({"cmd: " + ci.name, ci.description + (params.empty() ? "" : "  [" + params + "]"),
                         [this, ci] { run_palette_command(ci); }});
    }
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string f = lower(palette_filter_);
    std::vector<const Item*> filtered;
    for (const auto& it : items)
        if (f.empty() || lower(it.label).find(f) != std::string::npos) filtered.push_back(&it);
    if (palette_idx_ >= static_cast<int>(filtered.size())) palette_idx_ = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) && !filtered.empty())
        palette_idx_ = (palette_idx_ + static_cast<int>(filtered.size()) - 1) % static_cast<int>(filtered.size());
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && !filtered.empty())
        palette_idx_ = (palette_idx_ + 1) % static_cast<int>(filtered.size());
    const bool run_selected = enter && !filtered.empty();
    ImGui::BeginChild("##palette_list", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < filtered.size(); ++i) {
        const bool sel = static_cast<int>(i) == palette_idx_;
        if (ImGui::Selectable(filtered[i]->label.c_str(), sel)) filtered[i]->run();
        if (sel) ImGui::SetScrollHereY();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", filtered[i]->tip.c_str());
    }
    ImGui::EndChild();
    if (run_selected) filtered[static_cast<size_t>(palette_idx_)]->run();
    ImGui::End();
}

void EditorApp::new_scene() {
    stop_play();
    nlohmann::json empty;
    empty["name"] = "Untitled";
    empty["entities"] = nlohmann::json::array();
    empty["prefabs"] = nlohmann::json::object();
    if (auto r = scene_->load_json(empty); r.ok()) {
        clear_selection();
        prefab_save_target_ = NULL_ENTITY; // no stale modal across scene loads
        rename_target_ = NULL_ENTITY;
        history_->clear();
        scene_path_ = "scenes/untitled.scene.json"; // where Ctrl+S will write
        mark_saved();
        IGNIS_INFO("new empty scene");
    } else {
        IGNIS_ERROR("{}", r.error());
    }
}

void EditorApp::open_scene(const std::string& path) {
    stop_play();
    if (auto r = scene_->load_file(path); r.ok()) {
        clear_selection();
        prefab_save_target_ = NULL_ENTITY; // no stale modal across scene loads
        rename_target_ = NULL_ENTITY;
        history_->clear();
        scene_path_ = path;
        mark_saved();
        push_recent_scene(path);
        IGNIS_INFO("opened scene {}", path);
    } else {
        IGNIS_ERROR("{}", r.error());
    }
}

void EditorApp::reload_scene() {
    stop_play();
    if (auto r = scene_->load_file(scene_path_); r.ok()) {
        clear_selection();
        prefab_save_target_ = NULL_ENTITY; // no stale modal across scene loads
        rename_target_ = NULL_ENTITY;
        history_->clear();
        mark_saved();
        IGNIS_INFO("scene reloaded from {}", scene_path_);
    } else {
        IGNIS_ERROR("{}", r.error());
    }
}

void EditorApp::start_play() {
    if (playing_) return;
    // Snapshot the live scene so stop_play() can restore it (unsaved edits
    // survive a play session; the file on disk is never touched here).
    play_snapshot_ = scene_->serialize();
    has_snapshot_ = true;
    playing_ = true;
    IGNIS_INFO("play: snapshot taken ({} entities)",
               static_cast<unsigned long long>(scene_->world().entity_count()));
}

void EditorApp::stop_play() {
    if (!playing_ && !has_snapshot_) return;
    playing_ = false;
    if (has_snapshot_) {
        if (auto r = scene_->load_json(play_snapshot_); r.ok())
            IGNIS_INFO("stop: restored pre-play scene state");
        else
            IGNIS_ERROR("stop: restore failed: {}", r.error());
        has_snapshot_ = false;
    }
    prune_selection();
}

EntityId EditorApp::create_gameobject(const std::string& kind) {
    World& w = scene_->world();
    history_->begin("create " + kind);
    EntityId e = w.create_entity(kind);
    w.emplace<TransformComponent>(e);
    if (kind == "Cube") {
        w.emplace<MeshRendererComponent>(e)->model_path = "assets/cube.gltf";
    } else if (kind == "Camera") {
        w.emplace<CameraComponent>(e);
    } else if (kind == "Directional Light") {
        w.emplace<DirectionalLightComponent>(e);
    } else if (kind == "Point Light") {
        w.emplace<PointLightComponent>(e);
    } else if (kind == "Audio Source") {
        w.emplace<AudioSourceComponent>(e);
    }
    select_replace(e);
    history_->commit();
    IGNIS_INFO("created '{}' (#%llu)", kind, static_cast<unsigned long long>(e));
    return e;
}

void EditorApp::frame_selected() {
    if (selected_ == NULL_ENTITY || !scene_->world().alive(selected_)) return;
    if (!scene_->world().get<TransformComponent>(selected_)) return;
    const EntityId cam = find_active_camera();
    // Aim the view camera at the selected entity's world position, keeping distance.
    const math::Mat4 wm = world_matrix(scene_->world(), selected_);
    const math::Vec3 target{static_cast<f32>(wm[3][0]), static_cast<f32>(wm[3][1]),
                            static_cast<f32>(wm[3][2])};
    const math::Vec3 to_target = target - view_cam_.position;
    const f32 dist = glm::max(glm::length(to_target), 3.f);
    const math::Vec3 dir = glm::length(to_target) > 1e-5f ? glm::normalize(to_target)
                                                          : math::Vec3(0.f, 0.f, -1.f);
    cam_yaw_ = std::atan2(-dir.x, -dir.z);
    cam_pitch_ = glm::clamp(std::asin(glm::clamp(dir.y, -1.f, 1.f)), -1.55f, 1.55f);
    set_view_pose(target - dir * dist,
                  glm::angleAxis(cam_yaw_, math::Vec3(0.f, 1.f, 0.f)) *
                      glm::angleAxis(cam_pitch_, math::Vec3(1.f, 0.f, 0.f)),
                  cam);
}

// Fit the view frustum around the given bounding sphere, keeping the current
// view direction. In ortho mode this zooms (ortho size) instead of dollying.
void EditorApp::frame_view(const math::Vec3& center, f32 radius, f32 aspect, EntityId cam_entity) {
    const math::Quat rot = glm::angleAxis(cam_yaw_, math::Vec3(0.f, 1.f, 0.f)) *
                           glm::angleAxis(cam_pitch_, math::Vec3(1.f, 0.f, 0.f));
    const math::Vec3 dir = rot * math::Vec3(0.f, 0.f, -1.f);
    f32 dist;
    if (editor_ortho_) {
        editor_ortho_size_ = glm::max(radius * 1.2f, 0.05f);
        dist = glm::max(radius * 2.f, 1.f);
    } else {
        const f32 half_y = math::radians(view_cam_.fov_y_deg) * 0.5f;
        const f32 half_x = std::atan(std::tan(half_y) * aspect);
        dist = glm::max(radius / std::tan(glm::min(half_y, half_x)) * 1.2f, 1.f);
    }
    set_view_pose(center - dir * dist, rot, cam_entity);
}

void EditorApp::frame_scene() {
    // AABB over all entities with a transform (same half-extent convention as
    // viewport picking: local scale / 2 around the world position).
    math::Vec3 mn(std::numeric_limits<f32>::max());
    math::Vec3 mx(std::numeric_limits<f32>::lowest());
    bool any = false;
    for (EntityId e : scene_->world().all_entities()) {
        const auto* tc = scene_->world().get<TransformComponent>(e);
        if (!tc) continue;
        const math::Mat4 wm = world_matrix(scene_->world(), e);
        const math::Vec3 center{static_cast<f32>(wm[3][0]), static_cast<f32>(wm[3][1]),
                                static_cast<f32>(wm[3][2])};
        const math::Vec3 half = glm::max(tc->scale * 0.5f, math::Vec3(0.05f));
        mn = glm::min(mn, center - half);
        mx = glm::max(mx, center + half);
        any = true;
    }
    if (!any) return;
    const FramebufferSize fb = window_->framebuffer_size();
    const f32 aspect = fb.height > 0 ? static_cast<f32>(fb.width) / static_cast<f32>(fb.height) : 1.f;
    frame_view((mn + mx) * 0.5f, glm::max(glm::length(mx - mn) * 0.5f, 0.5f), aspect,
               find_active_camera());
}

// --- multi-selection (editor state; never serialized into the scene) ---------
// selected_ is always the primary = the entity the inspector/gizmo act on.

void EditorApp::select_replace(EntityId e) {
    selection_.clear();
    selection_.push_back(e);
    selected_ = e;
}

void EditorApp::select_add(EntityId e) {
    if (std::find(selection_.begin(), selection_.end(), e) == selection_.end())
        selection_.push_back(e);
    selected_ = e;
}

void EditorApp::select_toggle(EntityId e) {
    const auto it = std::find(selection_.begin(), selection_.end(), e);
    if (it != selection_.end()) {
        selection_.erase(it);
        selected_ = selection_.empty() ? NULL_ENTITY : selection_.back();
    } else {
        selection_.push_back(e);
        selected_ = e;
    }
}

void EditorApp::select_remove(EntityId e) {
    const auto it = std::find(selection_.begin(), selection_.end(), e);
    if (it != selection_.end()) selection_.erase(it);
    if (selected_ == e) selected_ = selection_.empty() ? NULL_ENTITY : selection_.back();
}

void EditorApp::clear_selection() {
    selection_.clear();
    selected_ = NULL_ENTITY;
}

void EditorApp::prune_selection() {
    std::erase_if(selection_, [this](EntityId e) { return !scene_->world().alive(e); });
    if (selected_ != NULL_ENTITY && !scene_->world().alive(selected_))
        selected_ = selection_.empty() ? NULL_ENTITY : selection_.back();
}

// Entities in the exact order the hierarchy tree draws them (roots sorted,
// children in hierarchy order) — used for Shift+click range selection.
std::vector<EntityId> EditorApp::tree_order() const {
    std::vector<EntityId> out;
    std::vector<EntityId> roots;
    for (EntityId e : scene_->world().all_entities())
        if (scene_->world().parent_of(e) == NULL_ENTITY) roots.push_back(e);
    std::sort(roots.begin(), roots.end());
    std::function<void(EntityId)> dfs = [&](EntityId e) {
        out.push_back(e);
        for (EntityId c : scene_->world().children_of(e)) dfs(c);
    };
    for (EntityId r : roots) dfs(r);
    return out;
}

void EditorApp::select_range(EntityId anchor, EntityId e, bool add) {
    const std::vector<EntityId> order = tree_order();
    const auto ia = std::find(order.begin(), order.end(), anchor);
    const auto ib = std::find(order.begin(), order.end(), e);
    if (ia == order.end() || ib == order.end()) return;
    const auto lo = std::min(ia, ib);
    const auto hi = std::max(ia, ib);
    if (!add) selection_.clear();
    for (auto it = lo; it <= hi; ++it)
        if (std::find(selection_.begin(), selection_.end(), *it) == selection_.end())
            selection_.push_back(*it);
    selected_ = e;
}

// Marquee selection: entities whose projected world AABB intersects the drag
// rectangle (8-corner projection, not just corner rays).
void EditorApp::marquee_select(const picking::ScreenRect& rect, bool add, bool remove) {
    const FramebufferSize fb = window_->framebuffer_size();
    if (fb.width == 0 || fb.height == 0) return;
    const Camera& cam = view_cam_;
    const f32 aspect = static_cast<f32>(fb.width) / static_cast<f32>(fb.height);
    std::vector<EntityId> hits;
    World& w = scene_->world();
    for (EntityId e : w.all_entities()) {
        if (!w.get<MeshRendererComponent>(e)) continue;
        const auto* tc = w.get<TransformComponent>(e);
        if (!tc) continue;
        const math::Mat4 wm = world_matrix(w, e);
        const math::Vec3 center{static_cast<f32>(wm[3][0]), static_cast<f32>(wm[3][1]),
                                static_cast<f32>(wm[3][2])};
        const picking::ScreenRect r =
            picking::project_aabb(cam, aspect, static_cast<f32>(fb.width), static_cast<f32>(fb.height),
                                  center, tc->scale * 0.5f);
        if (picking::rects_intersect(rect, r)) hits.push_back(e);
    }
    if (!add && !remove) {
        // Deterministic primary: last hit in tree order (not iteration order).
        const std::vector<EntityId> order = tree_order();
        std::sort(hits.begin(), hits.end(), [&](EntityId a, EntityId b) {
            const auto ia = std::find(order.begin(), order.end(), a);
            const auto ib = std::find(order.begin(), order.end(), b);
            return ia < ib;
        });
        selection_ = hits;
        selected_ = hits.empty() ? NULL_ENTITY : hits.back();
    } else if (add) {
        for (EntityId e : hits) select_add(e);
    } else {
        for (EntityId e : hits) select_remove(e);
    }
}

void EditorApp::delete_entity(EntityId e) {
    if (!scene_->world().alive(e)) return;
    history_->begin("delete '" + scene_->world().name_of(e) + "'");
    scripts_->on_entity_destroyed(e); // fire on_destroy
    scene_->world().destroy_entity(e);
    select_remove(e);
    history_->commit();
    IGNIS_INFO("deleted entity {}", e);
}

// The whole selection is deleted as one undo transaction.
void EditorApp::delete_selected() {
    if (selection_.empty()) return;
    const std::vector<EntityId> targets = selection_;
    history_->begin("delete " + std::to_string(targets.size()) + " entities");
    for (EntityId e : targets) {
        if (!scene_->world().alive(e)) continue;
        scripts_->on_entity_destroyed(e);
        scene_->world().destroy_entity(e);
        select_remove(e);
    }
    history_->commit();
    IGNIS_INFO("deleted {} entities", targets.size());
}

// Deep duplicate: the whole subtree (children included) via the engine's
// serialize_subtree/spawn_subtree, so entity references are remapped to the
// new ids. One undo transaction per call.
void EditorApp::duplicate_entity(EntityId e) {
    if (!scene_->world().alive(e)) return;
    World& w = scene_->world();
    history_->begin("duplicate '" + w.name_of(e) + "' (deep)");
    const nlohmann::json j = serialize_subtree(w, e);
    const EntityId copy = spawn_subtree(w, j, w.parent_of(e));
    if (copy != NULL_ENTITY) {
        w.set_name(copy, w.name_of(e) + " copy");
        if (auto* tc = w.get<TransformComponent>(copy)) tc->position.x += 1.f;
        select_replace(copy);
    }
    history_->commit();
}

// Duplicating the selection is one undo transaction; copies join the
// selection. Only selection roots are duplicated (children of selected
// entities come along inside their parent's subtree).
void EditorApp::duplicate_selected() {
    if (selection_.empty()) return;
    // One semantic for 1..N selected: the copies JOIN the selection and the
    // last copy becomes primary (no special-casing of the single case).
    // Only selection roots are duplicated (children of selected entities come
    // along inside their parent's subtree); deep via serialize/spawn_subtree.
    const std::vector<EntityId> roots = selection_roots();
    if (roots.empty()) return;
    World& w = scene_->world();
    history_->begin("duplicate " + std::to_string(roots.size()) + " entities (deep)");
    for (EntityId e : roots) {
        if (!w.alive(e)) continue;
        const nlohmann::json j = serialize_subtree(w, e);
        const EntityId copy = spawn_subtree(w, j, w.parent_of(e));
        if (copy == NULL_ENTITY) continue;
        w.set_name(copy, w.name_of(e) + " copy");
        if (auto* tc = w.get<TransformComponent>(copy)) tc->position.x += 1.f;
        selection_.push_back(copy);
    }
    if (!selection_.empty()) selected_ = selection_.back();
    history_->commit();
}

// --- clipboard + prefab save (issue #12) --------------------------------------

std::vector<EntityId> EditorApp::selection_roots() const {
    std::vector<EntityId> out;
    World& w = scene_->world();
    for (EntityId e : selection_) {
        const EntityId p = w.parent_of(e);
        const bool parent_selected =
            p != NULL_ENTITY && std::find(selection_.begin(), selection_.end(), p) != selection_.end();
        if (!parent_selected) out.push_back(e);
    }
    return out;
}

void EditorApp::copy_selection_to_clipboard() {
    const std::vector<EntityId> roots = selection_roots();
    if (roots.empty()) {
        IGNIS_WARN("nothing selected to copy");
        return;
    }
    World& w = scene_->world();
    nlohmann::json j;
    j["ignis_clipboard"] = "entities";
    j["entities"] = nlohmann::json::array();
    for (EntityId e : roots) j["entities"].push_back(serialize_subtree(w, e));
    ImGui::SetClipboardText(j.dump().c_str());
    IGNIS_INFO("copied {} entit{} to clipboard", roots.size(), roots.size() == 1 ? "y" : "ies");
}

void EditorApp::paste_clipboard_entities() {
    const char* text = ImGui::GetClipboardText();
    if (!text || text[0] == '\0') {
        IGNIS_WARN("clipboard is empty");
        return;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        IGNIS_WARN("clipboard is not JSON: {}", e.what());
        return;
    }
    if (!j.contains("ignis_clipboard") || j["ignis_clipboard"] != "entities" ||
        !j.contains("entities") || !j["entities"].is_array() || j["entities"].empty()) {
        IGNIS_WARN("clipboard does not hold Ignis entities");
        return;
    }
    World& w = scene_->world();
    history_->begin("paste " + std::to_string(j["entities"].size()) + " entities");
    std::vector<EntityId> pasted;
    for (const auto& sub : j["entities"]) {
        const EntityId e = spawn_subtree(w, sub, NULL_ENTITY); // root level
        if (e == NULL_ENTITY) continue;
        if (auto* tc = w.get<TransformComponent>(e)) tc->position.x += 1.f;
        pasted.push_back(e);
    }
    history_->commit();
    if (!pasted.empty()) {
        selection_ = pasted;
        selected_ = pasted.back();
        IGNIS_INFO("pasted {} entit{}", pasted.size(), pasted.size() == 1 ? "y" : "ies");
    }
}

void EditorApp::copy_component(const ComponentTypeInfo* info) {
    if (selected_ == NULL_ENTITY || !scene_->world().alive(selected_)) return;
    nlohmann::json j;
    j["ignis_component"] = info->name;
    j["data"] = info->serialize(scene_->world(), selected_);
    ImGui::SetClipboardText(j.dump().c_str());
    IGNIS_INFO("copied component '{}'", info->name);
}

void EditorApp::paste_component(const ComponentTypeInfo* info) {
    if (selected_ == NULL_ENTITY || !scene_->world().alive(selected_)) return;
    const char* text = ImGui::GetClipboardText();
    if (!text) return;
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        if (!j.contains("ignis_component") || j["ignis_component"] != info->name ||
            !j.contains("data")) {
            IGNIS_WARN("clipboard does not hold a '{}' component", info->name);
            return;
        }
        history_->begin("paste " + info->name);
        if (!info->deserialize(scene_->world(), selected_, j["data"]))
            IGNIS_WARN("bad component data for '{}'", info->name);
        history_->commit();
        IGNIS_INFO("pasted component '{}'", info->name);
    } catch (const std::exception& e) {
        IGNIS_ERROR("bad clipboard JSON: {}", e.what());
    }
}

void EditorApp::reset_component(const ComponentTypeInfo* info) {
    if (selected_ == NULL_ENTITY || !scene_->world().alive(selected_)) return;
    history_->begin("reset " + info->name);
    info->add_default(scene_->world(), selected_); // emplace overwrites
    history_->commit();
    IGNIS_INFO("reset component '{}' to defaults", info->name);
}

void EditorApp::draw_prefab_modal() {
    if (prefab_save_target_ == NULL_ENTITY || !scene_->world().alive(prefab_save_target_)) return;
    ImGui::OpenPopup("Save as prefab");
    if (ImGui::BeginPopupModal("Save as prefab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("prefab name", prefab_name_buf_, sizeof(prefab_name_buf_));
        if (ImGui::Button("Save")) {
            const std::string name = prefab_name_buf_;
            if (!name.empty()) {
                World& w = scene_->world();
                scene_->add_prefab(name, serialize_subtree(w, prefab_save_target_));
                IGNIS_INFO("saved prefab '{}' (appears under GameObject > Prefab)", name);
            }
            prefab_save_target_ = NULL_ENTITY;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            prefab_save_target_ = NULL_ENTITY;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorApp::undo() {
    if (!history_ || !history_->can_undo()) return;
    const std::string label = history_->undo_label();
    if (history_->undo()) {
        // Entity ids survive load_json; drop dead ids from the selection.
        prune_selection();
        IGNIS_INFO("undo '{}'", label);
    }
}

void EditorApp::redo() {
    if (!history_ || !history_->can_redo()) return;
    const std::string label = history_->redo_label();
    if (history_->redo()) {
        prune_selection();
        IGNIS_INFO("redo '{}'", label);
    }
}

void EditorApp::run_console_command(const std::string& line) {
    if (line.empty()) return;
    // Agent commands are undoable like any other scene mutation.
    history_->begin("cmd: " + (line.size() > 60 ? line.substr(0, 60) + "..." : line));
    try {
        const CommandResult r = commands_->execute(nlohmann::json::parse(line));
        // Results flow into the in-app console through the logger listener.
        if (r.ok) IGNIS_INFO("cmd: {} ({})", r.message, "ok");
        else IGNIS_ERROR("cmd failed: {}", r.message);
    } catch (const std::exception& e) {
        IGNIS_ERROR("bad command JSON: {}", e.what());
    }
    history_->commit();
}

void EditorApp::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New scene", "Ctrl+N")) prompt_if_dirty(2);
        item_tooltip("discard the current scene and start an empty one (asks when unsaved)");
        draw_open_scene_menu();
        if (ImGui::BeginMenu("Recent")) {
            if (recent_scenes_.empty()) {
                ImGui::MenuItem("(none yet)", nullptr, false, false);
            } else {
                for (const auto& p : recent_scenes_) {
                    const bool exists = std::filesystem::exists(p);
                    if (ImGui::MenuItem(p.c_str(), nullptr, false, exists)) prompt_if_dirty(3, p);
                    if (ImGui::IsItemHovered() && !exists)
                        ImGui::SetTooltip("file no longer exists");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear recent")) {
                    recent_scenes_.clear();
                    save_recent_scenes();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S")) save_scene();
        item_tooltip("write the scene to the current file");
        if (ImGui::MenuItem("Save as...")) {
            save_as_open_ = true;
            std::snprintf(save_as_buf_, sizeof(save_as_buf_), "%s", scene_path_.c_str());
        }
        item_tooltip("write the scene to a different file and make it the current one");
        if (ImGui::MenuItem("Reload from file")) prompt_if_dirty(1);
        item_tooltip("reload the scene from disk, discarding unsaved changes (asks when unsaved)");
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Esc")) request_quit();
        item_tooltip("close the editor (asks to save when there are unsaved changes)");
        ImGui::EndMenu();
    }
    draw_gameobject_menu();
    if (ImGui::BeginMenu("Edit")) {
        const bool has_sel = selected_ != NULL_ENTITY && scene_->world().alive(selected_);
        const bool can_undo = history_ && history_->can_undo();
        const bool can_redo = history_ && history_->can_redo();
        const std::string undo_label = "Undo" + (can_undo ? " " + history_->undo_label() : std::string());
        if (ImGui::MenuItem(undo_label.c_str(), "Ctrl+Z", false, can_undo)) undo();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("undo the last scene change");
        const std::string redo_label = "Redo" + (can_redo ? " " + history_->redo_label() : std::string());
        if (ImGui::MenuItem(redo_label.c_str(), "Ctrl+Shift+Z / Ctrl+Y", false, can_redo)) redo();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("redo the last undone change");
        ImGui::Separator();
        if (ImGui::MenuItem("Delete selected", "Del", false, has_sel)) delete_selected();
        item_tooltip("delete all selected entities as one undoable action");
        if (ImGui::MenuItem("Duplicate selected", "Ctrl+D", false, has_sel)) duplicate_selected();
        item_tooltip("deep-copy all selected entities (children included, +1 on X), one undo step");
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, has_sel)) copy_selection_to_clipboard();
        item_tooltip("copy the selected entities (deep, as JSON) to the clipboard");
        if (ImGui::MenuItem("Paste", "Ctrl+V")) paste_clipboard_entities();
        item_tooltip("paste entities from the clipboard at root level (+1 on X), one undo step");
        if (ImGui::MenuItem("Frame selected", "F", false, has_sel)) frame_selected();
        item_tooltip("move the camera to look at the selected entity");
        if (ImGui::MenuItem("Frame scene", "Shift+F")) frame_scene();
        item_tooltip("fit the view around the bounding box of all entities");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Hierarchy", nullptr, &show_hierarchy_);
        item_tooltip("show/hide the entity hierarchy panel");
        ImGui::MenuItem("Inspector", nullptr, &show_inspector_);
        item_tooltip("show/hide the selected entity's component inspector");
        ImGui::MenuItem("Bottom panel", nullptr, &show_bottom_);
        item_tooltip("show/hide the bottom panel (console / assets / stats)");
        ImGui::MenuItem("Status bar", nullptr, &show_statusbar_);
        item_tooltip("show/hide the status bar at the bottom");
        ImGui::MenuItem("Viewport overlay", nullptr, &show_overlay_);
        item_tooltip("show/hide the scene/camera/selection overlay drawn on the viewport");
        if (ImGui::MenuItem("Reset dock layout")) {
            std::filesystem::remove("editor_layout.ini");
            apply_default_layout_ = true; // rebuilt from the default on the next frame
            ImGui::MarkIniSettingsDirty();
        }
        item_tooltip("restore the default docked workspace layout");
        ImGui::Separator();
        const bool t_colliders = ImGui::MenuItem("Colliders", nullptr, &dbg_colliders_);
        item_tooltip("draw collider wireframes (color = rigid-body type: static grey, kinematic yellow, dynamic green)");
        const bool t_selection = ImGui::MenuItem("Selection outline", nullptr, &dbg_selection_);
        item_tooltip("draw the selected entity's world AABB in orange");
        const bool t_grid = ImGui::MenuItem("Ground grid", nullptr, &dbg_grid_);
        item_tooltip("draw a 1m grid on the XZ plane at y=0");
        const bool t_axes = ImGui::MenuItem("World axes", nullptr, &dbg_axes_);
        item_tooltip("draw 1m RGB axes at the world origin");
        if (t_colliders || t_selection || t_grid || t_axes) save_editor_state();
        ImGui::Separator();
        if (ImGui::MenuItem("Standalone editor camera", nullptr, standalone_cam_))
            set_standalone_camera(!standalone_cam_, find_active_camera());
        item_tooltip("fly the viewport with a scene-independent editor camera; "
                     "the scene camera entity is not modified while active");
        if (ImGui::MenuItem("Orthographic projection", nullptr, editor_ortho_)) {
            editor_ortho_ = !editor_ortho_;
            save_editor_state();
        }
        item_tooltip("switch the viewport between perspective and orthographic projection");
        if (editor_ortho_) {
            ImGui::SetNextItemWidth(140.f);
            if (ImGui::DragFloat("ortho size", &editor_ortho_size_, 0.1f, 0.01f, 100000.f))
                save_editor_state();
            item_tooltip("half-height of the orthographic view volume in world units");
        }
        ImGui::SetNextItemWidth(140.f);
        if (ImGui::DragFloat("fly speed", &cam_speed_, 0.5f, 0.1f, 1000.f)) save_editor_state();
        item_tooltip("WASDQE fly speed in units/s (hold Shift for 4x); mouse wheel dollies");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Run")) {
        if (ImGui::MenuItem(playing_ ? "Pause" : "Play", "P")) {
            if (playing_) stop_play();
            else start_play();
        }
        item_tooltip(playing_ ? "pause simulation and restore the pre-play scene state"
                              : "snapshot the scene, then simulate (scripts, physics, audio)");
        if (ImGui::MenuItem("Stop (restore pre-play state)", nullptr, false, playing_)) stop_play();
        item_tooltip("stop simulating and restore the scene as it was before Play");
        ImGui::EndMenu();
    }
    draw_help_menu();
    ImGui::TextDisabled("%s", playing_ ? "[playing]" : "[paused]");
    ImGui::EndMainMenuBar();
}

void EditorApp::draw_open_scene_menu() {
    if (!ImGui::BeginMenu("Open scene")) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> files;
    if (fs::is_directory("scenes", ec))
        for (const auto& entry : fs::directory_iterator("scenes", ec))
            if (entry.path().extension() == ".json") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        ImGui::MenuItem("(no scenes in scenes/)", nullptr, false, false);
    } else {
        for (const auto& f : files) {
            if (ImGui::MenuItem(f.filename().string().c_str())) prompt_if_dirty(3, f.generic_string());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("open %s", f.generic_string().c_str());
        }
    }
    ImGui::EndMenu();
}

void EditorApp::draw_gameobject_menu() {
    if (!ImGui::BeginMenu("GameObject")) return;
    for (u32 i = 0; i < kGameobjectKindCount; ++i) {
        if (ImGui::MenuItem(kGameobjectKinds[i])) create_gameobject(kGameobjectKinds[i]);
        item_tooltip(kGameobjectTips[i]);
    }
    const std::vector<std::string> prefabs = scene_->prefab_names();
    if (!prefabs.empty()) {
        ImGui::Separator();
        for (const auto& p : prefabs) {
            if (ImGui::MenuItem(("Prefab: " + p).c_str())) {
                const EntityId e = scene_->spawn_prefab(p);
                if (e != NULL_ENTITY) {
                    select_replace(e);
                    IGNIS_INFO("spawned prefab '{}' (#%llu)", p, static_cast<unsigned long long>(e));
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("spawn prefab '%s' into the scene", p.c_str());
        }
    }
    ImGui::EndMenu();
}

void EditorApp::draw_help_menu() {
    if (!ImGui::BeginMenu("Help")) return;
    if (ImGui::MenuItem("Shortcuts & about")) show_help_ = true;
    item_tooltip("show all keyboard shortcuts and scene info");
    ImGui::EndMenu();
}

void EditorApp::draw_help_window() {
    if (!show_help_) return;
    if (ImGui::Begin("Help - Ignis Editor", &show_help_)) {
        ImGui::Text("Ignis Editor");
        ImGui::Separator();
        ImGui::Text("Viewport: RMB drag = look, WASDQE = fly, wheel = dolly, LMB = select entity");
        ImGui::Text("          F = frame selected, Shift+F = frame scene, nav gizmo (top-right) snaps view");
        ImGui::Text("Gizmo:    W translate, E rotate, R scale; toolbar = space + snap");
        ImGui::Text("Edit:     Del delete, Ctrl+D duplicate (deep), Ctrl+C/V copy/paste, F frame selected");
        ImGui::Text("History:  Ctrl+Z undo, Ctrl+Shift+Z / Ctrl+Y redo");
        ImGui::Text("File:     Ctrl+N new scene, Ctrl+S save, Esc quit");
        ImGui::Text("Run:      P play/pause; Stop restores the pre-play scene state");
        ImGui::Text("Console:  filter by level/text; Enter runs a JSON agent command");
        ImGui::Text("Assets:   select a file, 'Use on selected' assigns it to the entity");
        ImGui::Separator();
        ImGui::Text("scene: %s  [%s]", scene_->name().c_str(), scene_path_.c_str());
        ImGui::Text("entities: %llu   prefabs: %zu",
                    static_cast<unsigned long long>(scene_->world().entity_count()),
                    scene_->prefab_names().size());
    }
    ImGui::End();
}

void EditorApp::draw_save_as_modal() {
    if (!save_as_open_) return;
    ImGui::OpenPopup("Save scene as");
    if (ImGui::BeginPopupModal("Save scene as", &save_as_open_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("path", save_as_buf_, sizeof(save_as_buf_));
        item_tooltip("scene file path, e.g. scenes/my_scene.scene.json");
        if (ImGui::Button("Save")) {
            if (auto r = scene_->save_to_file(save_as_buf_); r.ok()) {
                scene_path_ = save_as_buf_;
                mark_saved();
                push_recent_scene(scene_path_);
                IGNIS_INFO("scene saved to {}", scene_path_);
                save_as_open_ = false;
                ImGui::CloseCurrentPopup();
            } else {
                IGNIS_ERROR("{}", r.error());
            }
        }
        item_tooltip("write the scene to the path above and make it the current file");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            save_as_open_ = false;
            ImGui::CloseCurrentPopup();
        }
        item_tooltip("close without saving");
        ImGui::EndPopup();
    }
}

void EditorApp::draw_toolbar() {
    ImGui::SetNextWindowPos(ImVec2(0.f, ImGui::GetFrameHeight()), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetMainViewport()->WorkSize.x, 0.f));
    ImGui::Begin("##toolbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
    auto mode_button = [&](const char* label, const char* tip, GizmoOp op) {
        const bool active = gizmo_op_ == op;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label)) gizmo_op_ = op;
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    mode_button("T", "Translate gizmo (W)", GizmoOp::Translate);
    ImGui::SameLine();
    mode_button("R", "Rotate gizmo (E)", GizmoOp::Rotate);
    ImGui::SameLine();
    mode_button("S", "Scale gizmo (R)", GizmoOp::Scale);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::SmallButton(gizmo_world_space_ ? "World" : "Local")) gizmo_world_space_ = !gizmo_world_space_;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Gizmo coordinate space (local axes vs world axes)");
    ImGui::SameLine();
    if (ImGui::SmallButton(gizmo_snap_ ? "Snap: on" : "Snap: off")) gizmo_snap_ = !gizmo_snap_;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snap gizmo drags to the step below");
    ImGui::SameLine();
    if (gizmo_snap_) {
        f32* step = gizmo_op_ == GizmoOp::Translate ? &snap_translate_
                  : gizmo_op_ == GizmoOp::Rotate ? &snap_rotate_deg_
                  : &snap_scale_;
        ImGui::SetNextItemWidth(80.f);
        ImGui::DragFloat("##snapstep", step, 0.01f, 0.001f, 10000.f,
                         gizmo_op_ == GizmoOp::Rotate ? "%.1f deg" : "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Snap step for the current gizmo op (degrees for rotate)");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button(playing_ ? "||" : ">")) {
        if (playing_) stop_play();
        else start_play();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (P)", playing_ ? "Pause" : "Play");
    ImGui::SameLine();
    if (ImGui::Button("[]")) stop_play();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop and restore the pre-play scene state");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    const bool can_undo = history_ && history_->can_undo();
    const bool can_redo = history_ && history_->can_redo();
    ImGui::BeginDisabled(!can_undo);
    if (ImGui::SmallButton("Undo")) undo();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && can_undo)
        ImGui::SetTooltip("Undo '%s' (Ctrl+Z)", history_->undo_label().c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_redo);
    if (ImGui::SmallButton("Redo")) redo();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && can_redo)
        ImGui::SetTooltip("Redo '%s' (Ctrl+Shift+Z / Ctrl+Y)", history_->redo_label().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (selected_ != NULL_ENTITY && scene_->world().alive(selected_)) {
        if (selection_.size() > 1)
            ImGui::Text("Selected: %zu entities (%s #%llu primary)", selection_.size(),
                        scene_->world().name_of(selected_).c_str(),
                        static_cast<unsigned long long>(selected_));
        else
            ImGui::Text("Selected: %s (#%llu)", scene_->world().name_of(selected_).c_str(),
                        static_cast<unsigned long long>(selected_));
    } else {
        ImGui::TextDisabled("Selected: none (click/drag in the viewport; Ctrl adds, Ctrl+Shift removes)");
    }
    ImGui::End();
}

void EditorApp::draw_hierarchy_node(EntityId e) {
    World& w = scene_->world();
    const auto& children = w.children_of(e);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                ImGuiTreeNodeFlags_DefaultOpen;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (std::find(selection_.begin(), selection_.end(), e) != selection_.end())
        flags |= ImGuiTreeNodeFlags_Selected;
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(e)), flags,
                                        "%s", w.name_of(e).c_str());
    if (ImGui::IsItemHovered()) {
        std::string comps;
        for (const auto& [type, storage] : w.storages()) {
            if (!storage->has(e)) continue;
            if (const ComponentTypeInfo* info = w.registry().find_by_type(type)) {
                if (!comps.empty()) comps += ", ";
                comps += info->name;
            }
        }
        ImGui::SetTooltip("#%llu %s\ncomponents: %s  |  click = select, drag onto another node = reparent",
                          static_cast<unsigned long long>(e), w.name_of(e).c_str(), comps.c_str());
    }
    // Drag & drop reparenting: World::set_parent rejects cycles, so a drop on
    // a descendant is a no-op rather than corruption.
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        ImGui::SetDragDropPayload("IGNIS_ENTITY", &e, sizeof(e));
        ImGui::Text("%s", w.name_of(e).c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("IGNIS_ENTITY")) {
            const EntityId child = *static_cast<const EntityId*>(payload->Data);
            if (child != e) {
                history_->begin("reparent '" + w.name_of(child) + "' under '" + w.name_of(e) + "'");
                if (w.set_parent(child, e)) // rejects cycles: commit() then discards the no-op entry
                    IGNIS_INFO("reparented '{}' under '{}'", w.name_of(child).c_str(), w.name_of(e).c_str());
                history_->commit();
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemClicked()) {
        ImGuiIO& io_h = ImGui::GetIO();
        if (io_h.KeyShift)
            select_range(selected_ != NULL_ENTITY ? selected_ : e, e, io_h.KeyCtrl);
        else if (io_h.KeyCtrl)
            select_toggle(e);
        else
            select_replace(e);
    }
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Create child")) {
            EntityId c = w.create_entity("New Entity");
            w.set_parent(c, e);
            select_replace(c);
        }
        item_tooltip("create an empty entity as a child of this one");
        if (ImGui::MenuItem("Rename")) {
            rename_target_ = e;
            std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", w.name_of(e).c_str());
        }
        item_tooltip("rename this entity");
        if (ImGui::MenuItem("Duplicate (deep)")) duplicate_entity(e);
        item_tooltip("copy this entity and its whole subtree (+1 on X), one undo step");
        if (ImGui::MenuItem("Save as prefab...")) {
            prefab_save_target_ = e;
            std::snprintf(prefab_name_buf_, sizeof(prefab_name_buf_), "%s", w.name_of(e).c_str());
        }
        item_tooltip("save this entity's subtree as a named prefab (GameObject > Prefab)");
        if (ImGui::MenuItem("Delete")) delete_entity(e);
        item_tooltip("delete this entity and its children");
        ImGui::EndPopup();
    }
    if (open) {
        for (EntityId c : children) draw_hierarchy_node(c);
        ImGui::TreePop();
    }
}

void EditorApp::draw_hierarchy() {
    const f32 top = ImGui::GetFrameHeight() * 2.f; // menu bar + toolbar
    ImGui::SetNextWindowPos(ImVec2(0.f, top), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280.f, 420.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Hierarchy", &show_hierarchy_)) {
        ImGui::End();
        return;
    }
    World& w = scene_->world();
    if (ImGui::BeginPopupContextWindow("hierarchy_bg")) {
        if (ImGui::BeginMenu("Create")) {
            for (u32 i = 0; i < kGameobjectKindCount; ++i) {
                if (ImGui::MenuItem(kGameobjectKinds[i])) create_gameobject(kGameobjectKinds[i]);
                item_tooltip(kGameobjectTips[i]);
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::InputTextWithHint("##hier_filter", "filter...", hierarchy_filter_, sizeof(hierarchy_filter_));
    item_tooltip("filter entities by name (case-insensitive); matching entities are listed flat");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) hierarchy_filter_[0] = '\0';
    item_tooltip("clear the filter and show the tree again");
    const std::string filter = hierarchy_filter_;
    if (!filter.empty()) {
        // Flat list of case-insensitive name matches.
        auto lower = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string f = lower(filter);
        for (EntityId e : w.all_entities()) {
            const std::string name = w.name_of(e);
            if (lower(name).find(f) == std::string::npos) continue;
            const bool in_sel = std::find(selection_.begin(), selection_.end(), e) != selection_.end();
            if (ImGui::Selectable(name.c_str(), in_sel)) {
                ImGuiIO& io_f = ImGui::GetIO();
                if (io_f.KeyCtrl) select_toggle(e);
                else select_replace(e);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("#%llu - click to select, Ctrl+click to toggle", static_cast<unsigned long long>(e));
        }
    } else {
        std::vector<EntityId> roots;
        for (EntityId e : w.all_entities())
            if (w.parent_of(e) == NULL_ENTITY) roots.push_back(e);
        std::sort(roots.begin(), roots.end());
        for (EntityId e : roots) draw_hierarchy_node(e);
    }

    // Rename modal.
    if (rename_target_ != NULL_ENTITY && w.alive(rename_target_)) {
        ImGui::OpenPopup("Rename entity");
        if (ImGui::BeginPopupModal("Rename entity", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::InputText("name", rename_buf_, sizeof(rename_buf_));
            item_tooltip("new name for the entity");
            if (ImGui::Button("OK")) {
                history_->begin("rename entity");
                w.set_name(rename_target_, rename_buf_);
                history_->commit();
                rename_target_ = NULL_ENTITY;
                ImGui::CloseCurrentPopup();
            }
            item_tooltip("apply the new name");
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                rename_target_ = NULL_ENTITY;
                ImGui::CloseCurrentPopup();
            }
            item_tooltip("keep the old name");
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

void EditorApp::draw_inspector() {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkSize.x - 300.f,
                                   ImGui::GetFrameHeight() * 2.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300.f, 420.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector", &show_inspector_)) {
        ImGui::End();
        return;
    }
    World& w = scene_->world();
    if (selected_ == NULL_ENTITY || !w.alive(selected_)) {
        ImGui::TextDisabled("no entity selected (click one in the viewport)");
        ImGui::End();
        return;
    }
    ImGui::Text("Entity #%llu", static_cast<unsigned long long>(selected_));
    {
        char name_buf[128];
        std::snprintf(name_buf, sizeof(name_buf), "%s", w.name_of(selected_).c_str());
        history_->begin("edit name"); // coalesced per drag/typing session by SceneHistory
        if (ImGui::InputText("name", name_buf, sizeof(name_buf))) w.set_name(selected_, name_buf);
        item_tooltip("entity name (used by scripts, hierarchy and serialization)");
    }
    const EntityId parent = w.parent_of(selected_);
    ImGui::TextDisabled("parent: %s", parent == NULL_ENTITY ? "(root)" : w.name_of(parent).c_str());
    ImGui::Separator();

    for (const auto& [type, storage] : w.storages()) {
        if (!storage->has(selected_)) continue;
        const ComponentTypeInfo* info = w.registry().find_by_type(type);
        if (!info) continue;
        ImGui::PushID(info->name.c_str());
        const bool open = ImGui::CollapsingHeader(info->name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetWindowWidth() - 62.f);
        if (ImGui::SmallButton("...")) ImGui::OpenPopup("comp_ops");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("component actions: copy / paste / reset to default");
        if (ImGui::BeginPopup("comp_ops")) {
            if (ImGui::MenuItem("Copy component")) copy_component(info);
            if (ImGui::MenuItem("Paste component")) paste_component(info);
            if (ImGui::MenuItem("Reset to default")) reset_component(info);
            ImGui::EndPopup();
        }
        ImGui::SameLine(ImGui::GetWindowWidth() - 30.f);
        if (ImGui::SmallButton("x")) {
            history_->begin("remove " + info->name);
            info->remove(w, selected_);
            history_->commit();
            ImGui::PopID();
            continue;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("remove the %s component from this entity", info->name.c_str());
        if (open) {
            history_->begin("edit " + info->name); // one undo entry per drag, not per frame
            if (info->name == "Transform") edit_transform(*w.get<TransformComponent>(selected_));
            else if (info->name == "Camera") edit_camera(*w.get<CameraComponent>(selected_));
            else if (info->name == "DirectionalLight") edit_directional_light(*w.get<DirectionalLightComponent>(selected_));
            else if (info->name == "PointLight") edit_point_light(*w.get<PointLightComponent>(selected_));
            else if (info->name == "SpotLight") edit_spot_light(*w.get<SpotLightComponent>(selected_));
            else if (info->name == "MeshRenderer") edit_mesh_renderer(*w.get<MeshRendererComponent>(selected_));
            else if (info->name == "Script") edit_script(*w.get<ScriptComponent>(selected_));
            else if (info->name == "RigidBody") edit_rigid_body(*w.get<RigidBodyComponent>(selected_));
            else if (info->name == "Collider") edit_collider(*w.get<ColliderComponent>(selected_));
            else if (info->name == "AudioSource") {
                auto& a = *w.get<AudioSourceComponent>(selected_);
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", a.path.c_str());
                if (ImGui::InputText("path", buf, sizeof(buf))) a.path = buf;
                ImGui::DragFloat("volume", &a.volume, 0.01f, 0.f, 1.f);
                ImGui::DragFloat("pitch", &a.pitch, 0.01f, 0.1f, 4.f);
                ImGui::Checkbox("loop", &a.loop);
                ImGui::Checkbox("play_on_start", &a.play_on_start);
                ImGui::Checkbox("spatial", &a.spatial);
                if (a.spatial) {
                    ImGui::DragFloat("min_distance", &a.min_distance, 0.1f, 0.1f, 1000.f);
                    ImGui::DragFloat("max_distance", &a.max_distance, 0.5f, 0.1f, 10000.f);
                }
                if (ImGui::Button("Play")) audio_sys_->play_source(selected_, w);
                item_tooltip("start playing this audio source now");
                ImGui::SameLine();
                if (ImGui::Button("Stop")) audio_sys_->stop_source(selected_, w);
                item_tooltip("stop playing this audio source");
                ImGui::SameLine();
                ImGui::TextDisabled(audio_sys_->source_playing(selected_) ? "playing" : "stopped");
            }
        }
        ImGui::PopID();
    }

    // Add Component combo: every registered type the entity doesn't have yet.
    if (ImGui::BeginCombo("Add Component", "+ add...")) {
        item_tooltip("add a component to the selected entity");
        for (const std::string& name : w.registry().names()) {
            const ComponentTypeInfo* info = w.registry().find_by_name(name);
            if (!info) continue;
            bool has = false;
            for (const auto& [type, storage] : w.storages())
                if (type == info->type) { has = storage->has(selected_); break; }
            if (has) continue;
            if (ImGui::Selectable(name.c_str())) {
                history_->begin("add " + name);
                info->add_default(w, selected_);
                history_->commit();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("add %s with default values", name.c_str());
        }
        ImGui::EndCombo();
    }
    ImGui::End();
}

void EditorApp::draw_console() {
    // Per-level toggle buttons with live counts.
    u32 counts[6] = {0, 0, 0, 0, 0, 0};
    {
        std::lock_guard lock(console_mutex_);
        for (const auto& [level, msg] : console_) ++counts[static_cast<u8>(level)];
    }
    for (u8 i = 0; i < 6; ++i) {
        const LogLevel level = static_cast<LogLevel>(i);
        if (i > 0) ImGui::SameLine();
        char label[40];
        std::snprintf(label, sizeof(label), "%s %u", to_string(level), counts[i]);
        const bool on = log_show_[i];
        if (!on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        if (ImGui::SmallButton(label)) log_show_[i] = !log_show_[i];
        if (!on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("show/hide %s messages", to_string(level));
    }
    ImGui::SameLine();
    ImGui::InputText("filter", console_filter_, sizeof(console_filter_));
    item_tooltip("only show messages containing this text");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        std::lock_guard lock(console_mutex_);
        console_.clear();
    }
    item_tooltip("clear all console messages");
    ImGui::SameLine();
    ImGui::Checkbox("auto-scroll", &console_autoscroll_);
    item_tooltip("scroll to the newest message automatically");
    const std::string filter = console_filter_;
    ImGui::BeginChild("##console_scroll", ImVec2(0.f, -30.f), ImGuiChildFlags_Borders);
    std::lock_guard lock(console_mutex_);
    for (const auto& [level, msg] : console_) {
        const u8 idx = static_cast<u8>(level);
        if (idx < 6 && !log_show_[idx]) continue;
        if (!filter.empty() && msg.find(filter) == std::string::npos) continue;
        draw_console_line(level, msg);
    }
    if (console_autoscroll_) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    draw_console_input();
}

// One console line; alive "#<id>" tokens (complete tokens only, validated
// against the world) get a click-to-select button at the end of the line.
void EditorApp::draw_console_line(LogLevel level, const std::string& msg) {
    ImGui::TextColored(log_color(level), "[%s] %s", to_string(level), msg.c_str());
    std::vector<EntityId> refs;
    for (size_t i = 0; i + 1 < msg.size(); ++i) {
        if (msg[i] != '#') continue;
        const bool left_ok =
            i == 0 || (!std::isdigit(static_cast<unsigned char>(msg[i - 1])) && msg[i - 1] != '#');
        size_t d = i + 1;
        while (d < msg.size() && std::isdigit(static_cast<unsigned char>(msg[d]))) ++d;
        const bool right_ok = d >= msg.size() || !std::isdigit(static_cast<unsigned char>(msg[d]));
        if (left_ok && right_ok && d > i + 1) {
            const EntityId id = static_cast<EntityId>(std::stoull(msg.substr(i + 1, d - i - 1)));
            if (scene_->world().alive(id)) refs.push_back(id);
            i = d - 1;
        }
    }
    for (EntityId id : refs) {
        ImGui::SameLine();
        ImGui::PushID(static_cast<int>(id));
        char label[24];
        std::snprintf(label, sizeof(label), "#%llu", static_cast<unsigned long long>(id));
        if (ImGui::SmallButton(label)) select_replace(id);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("select '%s'", scene_->world().name_of(id).c_str());
        ImGui::PopID();
    }
}

// InputText callback: Up/Down walk the command history; Tab completes the
// "cmd": "..." value against CommandProcessor::known_commands() (the shared
// registry — no separate hard-coded list).
int EditorApp::console_input_cb(ImGuiInputTextCallbackData* d) {
    auto* self = static_cast<EditorApp*>(d->UserData);
    if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (d->EventKey == ImGuiKey_UpArrow && !self->console_history_.empty()) {
            if (self->console_hist_idx_ < 0)
                self->console_hist_idx_ = static_cast<int>(self->console_history_.size()) - 1;
            else if (self->console_hist_idx_ > 0)
                --self->console_hist_idx_;
            d->DeleteChars(0, d->BufTextLen);
            d->InsertChars(0, self->console_history_[static_cast<size_t>(self->console_hist_idx_)].c_str());
        } else if (d->EventKey == ImGuiKey_DownArrow && self->console_hist_idx_ >= 0) {
            ++self->console_hist_idx_;
            if (self->console_hist_idx_ >= static_cast<int>(self->console_history_.size()))
                self->console_hist_idx_ = -1; // past the newest: empty line
            d->DeleteChars(0, d->BufTextLen);
            if (self->console_hist_idx_ >= 0)
                d->InsertChars(0, self->console_history_[static_cast<size_t>(self->console_hist_idx_)].c_str());
        }
    } else if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion && d->EventKey == ImGuiKey_Tab) {
        const std::string text(d->Buf, static_cast<size_t>(d->BufTextLen));
        const auto key = text.find("\"cmd\"");
        if (key == std::string::npos) return 0;
        const auto q1 = text.find('"', key + 5);
        if (q1 == std::string::npos) return 0;
        const auto q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) return 0;
        const std::string partial = text.substr(q1 + 1, q2 - q1 - 1);
        for (const auto& ci : CommandProcessor::known_commands()) {
            if (ci.name.compare(0, partial.size(), partial) == 0) {
                d->DeleteChars(static_cast<int>(q1) + 1, static_cast<int>(q2 - q1 - 1));
                d->InsertChars(static_cast<int>(q1) + 1, ci.name.c_str());
                break;
            }
        }
    }
    return 0;
}

void EditorApp::draw_console_input() {
    if (console_focus_pending_) {
        console_focus_pending_ = false;
        ImGui::SetKeyboardFocusHere(-1); // palette dropped a command template here
    }
    ImGui::PushItemWidth(-1.f);
    const bool run = ImGui::InputTextWithHint(
        "##cmd", "{\"cmd\": \"create_entity\", \"name\": \"Bot\"}  (Enter = run agent command)",
        console_input_, sizeof(console_input_),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
            ImGuiInputTextFlags_CallbackCompletion,
        &EditorApp::console_input_cb, this);
    item_tooltip("run a JSON agent command on Enter; Tab completes the cmd name from the command "
                 "registry, Up/Down recall history; #id tokens in the log are clickable");
    ImGui::PopItemWidth();
    if (run) {
        const std::string line = console_input_;
        if (!line.empty() && (console_history_.empty() || console_history_.back() != line))
            console_history_.push_back(line);
        if (console_history_.size() > 100) console_history_.erase(console_history_.begin());
        console_hist_idx_ = -1;
        run_console_command(line);
        console_input_[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1); // keep focus in the input for the next command
    }
}

// Icon color per asset type, from the central registry (issue #15).
namespace {
ImVec4 asset_icon_color(const AssetTypeInfo* t) {
    if (!t) return ImVec4(0.6f, 0.6f, 0.6f, 1.f);
    switch (t->icon[0]) {
        case 'M': return ImVec4(0.4f, 0.9f, 0.5f, 1.f);   // model
        case 'A': return ImVec4(1.0f, 0.7f, 0.3f, 1.f);   // audio
        case 'L': return ImVec4(0.4f, 0.6f, 1.0f, 1.f);   // script
        case 'T': return ImVec4(0.9f, 0.5f, 0.9f, 1.f);   // texture
        case 'S': return ImVec4(1.0f, 0.5f, 0.8f, 1.f);   // scene
        case 'J': return ImVec4(0.7f, 0.7f, 0.4f, 1.f);   // json
        default:  return ImVec4(0.6f, 0.6f, 0.6f, 1.f);
    }
}
} // namespace

void EditorApp::draw_asset_browser() {
    namespace fs = std::filesystem;
    ImGui::InputTextWithHint("##asset_filter", "filter...", asset_filter_, sizeof(asset_filter_));
    item_tooltip("filter files by relative path (case-insensitive)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy path")) {
        if (!selected_asset_.empty()) {
            ImGui::SetClipboardText(selected_asset_.c_str());
            IGNIS_INFO("copied path '{}'", selected_asset_);
        }
    }
    item_tooltip("copy the selected file's relative path to the clipboard");
    ImGui::SameLine();
    if (ImGui::SmallButton("Use on selected")) assign_selected_asset();
    item_tooltip("assign the selected file to the selected entity (type from the asset registry)");
    ImGui::Separator();
    ImGui::BeginChild("##assets_scroll");
    const fs::path roots[] = {"assets", "scripts", "scenes"};
    const std::string filter = asset_filter_;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string f = lower(filter);
    // One row renderer: icon + name + size, selection, drag source, context menu.
    auto draw_file = [&](const fs::path& path, const std::string& rel) {
        if (!f.empty() && lower(rel).find(f) == std::string::npos) return;
        const AssetTypeInfo* at = find_asset_type_for_file(path.filename().string());
        ImGui::PushID(rel.c_str());
        ImGui::TextColored(asset_icon_color(at), "%s", at ? at->icon : "?");
        ImGui::SameLine();
        const bool sel = selected_asset_ == rel;
        if (ImGui::Selectable(path.filename().string().c_str(), sel)) selected_asset_ = rel;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s (%s) - click to select, drag into the viewport to spawn, right-click for file ops",
                              rel.c_str(), at ? at->label : "file");
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("IGNIS_ASSET", rel.c_str(), rel.size() + 1);
            ImGui::TextColored(asset_icon_color(at), "%s %s", at ? at->icon : "?", rel.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem()) {
            asset_ctx_ = rel;
            if (ImGui::MenuItem("Rename")) {
                asset_rename_open_ = true;
                std::snprintf(asset_name_buf_, sizeof(asset_name_buf_), "%s", path.filename().string().c_str());
            }
            if (ImGui::MenuItem("Delete")) {
                asset_delete_open_ = true;
                asset_delete_refs_.clear();
                const auto refs = find_asset_references("scenes", rel);
                if (!refs.empty()) {
                    asset_delete_refs_ = "referenced by " + std::to_string(refs.size()) + " scene file(s), e.g. " +
                                         refs.front();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%llu B", static_cast<unsigned long long>(fs::file_size(path)));
        ImGui::PopID();
    };
    if (!f.empty()) {
        // Flat filtered list across all roots.
        for (const auto& root : roots) {
            std::error_code ec;
            if (!fs::is_directory(root, ec)) continue;
            for (const auto& entry : fs::recursive_directory_iterator(root, ec))
                if (entry.is_regular_file())
                    draw_file(entry.path(), fs::relative(entry.path(), root).generic_string());
        }
    } else {
        std::function<void(const fs::path&, const fs::path&)> draw_dir = [&](const fs::path& dir, const fs::path& root) {
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) return;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                const std::string name = entry.path().filename().string();
                if (entry.is_directory()) {
                    if (ImGui::TreeNode(name.c_str())) {
                        draw_dir(entry.path(), root);
                        ImGui::TreePop();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s - click to expand/collapse", entry.path().generic_string().c_str());
                } else {
                    draw_file(entry.path(), fs::relative(entry.path(), root).generic_string());
                }
            }
        };
        for (const auto& root : roots) {
            if (ImGui::TreeNode(root.c_str())) {
                draw_dir(root, root);
                ImGui::TreePop();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s/ - click to expand/collapse", root.generic_string().c_str());
        }
    }
    if (ImGui::BeginPopupContextWindow("assets_bg")) {
        if (ImGui::MenuItem("New Lua script")) new_asset_file("scripts", "new_script.lua", "-- new script\n");
        if (ImGui::MenuItem("New scene")) new_asset_file("scenes", "untitled.scene.json", "{\n  \"name\": \"untitled\"\n}\n");
        if (ImGui::MenuItem("New folder")) new_asset_file("assets", "new_folder", "");
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    if (!selected_asset_.empty()) ImGui::Text("selected: %s", selected_asset_.c_str());
}

// Create a file under a project root dir, with a collision-safe name. All
// paths are constrained to the project root (never outside it).
void EditorApp::new_asset_file(const char* root_dir, const std::string& filename, const std::string& content) {
    namespace fs = std::filesystem;
    fs::path target = fs::path(root_dir) / filename;
    if (is_unsafe_resource_path(target.generic_string()) || !is_path_within_root(".", target)) {
        IGNIS_ERROR("refusing to create '{}': outside the project root", target.generic_string());
        return;
    }
    // Collision-safe: append _2, _3, ... before the extension.
    for (u32 i = 2; fs::exists(target); ++i) {
        const auto stem = filename.substr(0, filename.find_last_of('.'));
        const auto ext = filename.substr(filename.find_last_of('.'));
        target = fs::path(root_dir) / (stem + "_" + std::to_string(i) + ext);
    }
    std::error_code ec;
    if (content.empty()) {
        fs::create_directory(target, ec);
    } else {
        std::ofstream f(target);
        f << content;
    }
    if (ec) {
        IGNIS_ERROR("cannot create '{}': {}", target.generic_string(), ec.message());
        return;
    }
    IGNIS_INFO("created {}", target.generic_string());
}

// Viewport drop target for asset drags: raycast the drop point against mesh
// AABBs (fallback: a point in front of the camera) and spawn the asset's
// entity there. Scene files ask before loading.
void EditorApp::spawn_asset_at(const std::string& rel_path, f32 mx, f32 my) {
    namespace fs = std::filesystem;
    const AssetTypeInfo* at = find_asset_type_for_file(fs::path(rel_path).filename().string());
    if (at && at->is_scene) {
        scene_load_pending_ = rel_path; // confirm-load modal
        scene_load_open_ = true;
        return;
    }
    if (!at || !at->can_spawn || !at->component[0]) {
        IGNIS_WARN("'{}' cannot be spawned into the scene", rel_path);
        return;
    }
    const FramebufferSize fb = window_->framebuffer_size();
    if (fb.width == 0 || fb.height == 0) return;
    const f32 aspect = static_cast<f32>(fb.width) / static_cast<f32>(fb.height);
    const f32 ndcX = (mx / static_cast<f32>(fb.width)) * 2.f - 1.f;
    const f32 ndcY = (my / static_cast<f32>(fb.height)) * 2.f - 1.f;
    const picking::Ray ray = picking::screen_ray(view_cam_, aspect, ndcX, -ndcY);
    // Hit point: nearest mesh AABB along the ray, else 5 units in front.
    math::Vec3 pos = view_cam_.position + (view_cam_.rotation * math::Vec3(0.f, 0.f, -1.f)) * 5.f;
    f32 bestT = 1e30f;
    for (EntityId e : scene_->world().all_entities()) {
        if (!scene_->world().get<MeshRendererComponent>(e)) continue;
        const auto* tc = scene_->world().get<TransformComponent>(e);
        if (!tc) continue;
        const math::Mat4 wm = world_matrix(scene_->world(), e);
        const math::Vec3 center{static_cast<f32>(wm[3][0]), static_cast<f32>(wm[3][1]), static_cast<f32>(wm[3][2])};
        if (const auto t = picking::ray_aabb(ray.origin, ray.dir, center, tc->scale * 0.5f)) {
            if (*t < bestT) {
                bestT = *t;
                pos = ray.origin + ray.dir * *t;
            }
        }
    }
    World& w = scene_->world();
    history_->begin("spawn '" + rel_path + "'");
    const EntityId e = w.create_entity(fs::path(rel_path).filename().string());
    w.emplace<TransformComponent>(e)->position = pos;
    if (const ComponentTypeInfo* info = w.registry().find_by_name(at->component)) {
        info->add_default(w, e);
        if (info->name == "MeshRenderer") w.get<MeshRendererComponent>(e)->model_path = rel_path;
        else if (info->name == "AudioSource") w.get<AudioSourceComponent>(e)->path = rel_path;
        else if (info->name == "Script") w.get<ScriptComponent>(e)->path = rel_path;
    }
    history_->commit();
    select_replace(e);
    IGNIS_INFO("spawned '{}' at ({:.1f}, {:.1f}, {:.1f})", rel_path, static_cast<double>(pos.x),
               static_cast<double>(pos.y), static_cast<double>(pos.z));
}

void EditorApp::draw_asset_modals() {
    namespace fs = std::filesystem;
    if (asset_rename_open_ && !asset_ctx_.empty()) {
        ImGui::OpenPopup("Rename asset");
        if (ImGui::BeginPopupModal("Rename asset", &asset_rename_open_, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::InputText("new name", asset_name_buf_, sizeof(asset_name_buf_));
            if (ImGui::Button("Rename")) {
                const fs::path src = asset_ctx_;
                fs::path dst = src.parent_path() / asset_name_buf_;
                if (!is_unsafe_resource_path(dst.generic_string()) && is_path_within_root(".", dst) &&
                    !fs::exists(dst)) {
                    std::error_code ec;
                    fs::rename(src, dst, ec);
                    if (ec) IGNIS_ERROR("rename failed: {}", ec.message());
                    else {
                        IGNIS_INFO("renamed {} -> {}", src.generic_string(), dst.generic_string());
                        if (selected_asset_ == src.generic_string()) selected_asset_ = dst.generic_string();
                    }
                } else {
                    IGNIS_ERROR("refusing to rename to '{}': unsafe or existing path", dst.generic_string());
                }
                asset_rename_open_ = false;
                asset_ctx_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                asset_rename_open_ = false;
                asset_ctx_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    // Confirm-load modal for a scene file dropped into the viewport.
    if (scene_load_open_ && !scene_load_pending_.empty()) {
        ImGui::OpenPopup("Load scene");
        if (ImGui::BeginPopupModal("Load scene", &scene_load_open_, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Load '%s' (unsaved changes are lost)?", scene_load_pending_.c_str());
            if (ImGui::Button("Load")) {
                open_scene(scene_load_pending_);
                scene_load_open_ = false;
                scene_load_pending_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                scene_load_open_ = false;
                scene_load_pending_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    if (asset_delete_open_ && !asset_ctx_.empty()) {
        ImGui::OpenPopup("Delete asset");
        if (ImGui::BeginPopupModal("Delete asset", &asset_delete_open_, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete '%s'?", asset_ctx_.c_str());
            if (!asset_delete_refs_.empty())
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "WARNING: %s", asset_delete_refs_.c_str());
            if (ImGui::Button("Delete")) {
                const fs::path target = asset_ctx_;
                if (!is_unsafe_resource_path(target.generic_string()) && is_path_within_root(".", target)) {
                    std::error_code ec;
                    fs::remove(target, ec);
                    if (ec) IGNIS_ERROR("delete failed: {}", ec.message());
                    else {
                        IGNIS_INFO("deleted {}", target.generic_string());
                        if (selected_asset_ == target.generic_string()) selected_asset_.clear();
                    }
                } else {
                    IGNIS_ERROR("refusing to delete '{}': outside the project root", target.generic_string());
                }
                asset_delete_open_ = false;
                asset_ctx_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                asset_delete_open_ = false;
                asset_ctx_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

// Assign the selected asset file to the selected entity, by extension. Adds
// the target component with defaults if the entity doesn't have it yet.
// Assign the selected asset file to the selected entity, using the central
// asset-type registry (#15): the registry names the target component, the
// editor only knows which field carries the path.
void EditorApp::assign_selected_asset() {
    namespace fs = std::filesystem;
    if (selected_asset_.empty()) {
        IGNIS_WARN("no asset selected");
        return;
    }
    World& w = scene_->world();
    if (selected_ == NULL_ENTITY || !w.alive(selected_)) {
        IGNIS_WARN("no entity selected");
        return;
    }
    const AssetTypeInfo* at = find_asset_type_for_file(fs::path(selected_asset_).filename().string());
    if (!at || !at->component[0]) {
        IGNIS_WARN("'{}' is not assignable to an entity", selected_asset_);
        return;
    }
    const ComponentTypeInfo* info = w.registry().find_by_name(at->component);
    if (!info) {
        IGNIS_WARN("target component '{}' not registered", at->component);
        return;
    }
    history_->begin("assign '" + selected_asset_ + "'");
    bool has = false;
    for (const auto& [type, storage] : w.storages())
        if (type == info->type) { has = storage->has(selected_); break; }
    if (!has) info->add_default(w, selected_);
    if (info->name == "MeshRenderer") w.get<MeshRendererComponent>(selected_)->model_path = selected_asset_;
    else if (info->name == "AudioSource") w.get<AudioSourceComponent>(selected_)->path = selected_asset_;
    else if (info->name == "Script") w.get<ScriptComponent>(selected_)->path = selected_asset_;
    history_->commit();
    IGNIS_INFO("assigned '{}' to '{}'", selected_asset_, w.name_of(selected_));
}

void EditorApp::draw_stats(f32 dt) {
    const RenderStats st = renderer_->stats();
    const FramebufferSize fb = window_->framebuffer_size();
    if (dt > 0.f) {
        fps_history_.push_back(1.f / dt);
        if (fps_history_.size() > 240) fps_history_.erase(fps_history_.begin());
    }
    f32 avg = 0.f, mn = 0.f, mx = 0.f;
    if (!fps_history_.empty()) {
        mn = 1e30f;
        for (const f32 f : fps_history_) {
            avg += f;
            mn = std::min(mn, f);
            mx = std::max(mx, f);
        }
        avg /= static_cast<f32>(fps_history_.size());
    }
    ImGui::Text("frame:      %.2f ms (%.1f fps)", static_cast<double>(dt * 1000.f),
                static_cast<double>(dt > 0.f ? 1.f / dt : 0.f));
    if (!fps_history_.empty()) {
        ImGui::Text("fps avg %.1f  min %.1f  max %.1f", static_cast<double>(avg),
                    static_cast<double>(mn), static_cast<double>(mx));
        ImGui::PlotLines("##fps_graph", fps_history_.data(), static_cast<int>(fps_history_.size()),
                         0, "fps", 0.f, 0.f, ImVec2(-1.f, 60.f));
    }
    ImGui::Text("draw calls: %u", st.draw_calls);
    ImGui::Text("triangles:  %u", st.triangles);
    u64 components = 0;
    for (const auto& [type, storage] : scene_->world().storages()) {
        (void)type;
        components += storage->entities().size();
    }
    ImGui::Text("entities:   %llu (%llu components)",
                static_cast<unsigned long long>(scene_->world().entity_count()),
                static_cast<unsigned long long>(components));
    ImGui::Text("resolution: %u x %u", fb.width, fb.height);
    ImGui::Text("scene:      %s (%s)", scene_->name().c_str(),
                playing_ ? "playing" : "paused");
    ImGui::Text("prefabs:    %zu", scene_->prefab_names().size());
}

void EditorApp::draw_bottom_panel(f32 dt) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - 240.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560.f, 220.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Bottom", &show_bottom_)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("##bottom_tabs")) {
        if (ImGui::BeginTabItem("Console")) {
            item_tooltip("engine log with level/text filters + JSON agent command input");
            bottom_tab_ = 0;
            draw_console();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Assets")) {
            item_tooltip("browse assets/, scripts/, scenes/; assign files to the selected entity");
            bottom_tab_ = 1;
            draw_asset_browser();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Stats")) {
            item_tooltip("frame time, fps graph, draw calls, entities");
            bottom_tab_ = 2;
            draw_stats(dt);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Scene")) {
            item_tooltip("scene-level settings: gravity, time scale, ambient light, clear color");
            bottom_tab_ = 3;
            draw_scene_settings();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void EditorApp::draw_scene_settings() {
    SceneSettings& s = scene_->settings();
    if (ImGui::DragFloat3("gravity", &s.gravity.x, 0.1f, -100.f, 100.f))
        physics_->set_gravity(s.gravity); // live: affects a running simulation
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("world gravity, applied live to the physics simulation");
    ImGui::SliderFloat("time scale", &s.time_scale, 0.f, 4.f); // read in main_loop
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("scales simulation time while playing (0 = frozen); render fps is unaffected");
    ImGui::ColorEdit3("ambient color", &s.ambient_color.x);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("flat ambient light color (kept separate from IBL environment lighting)");
    ImGui::DragFloat("ambient intensity", &s.ambient_intensity, 0.01f, 0.f, 10.f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("strength of the flat ambient light");
    ImGui::ColorEdit4("clear color (scene default)", &s.clear_color.x);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("scene-level default clear color; the active camera's own clear color overrides it");
    if (ImGui::Button("Apply clear color to active camera")) {
        const EntityId cam = find_active_camera();
        if (cam != NULL_ENTITY) {
            if (auto* cc = scene_->world().get<CameraComponent>(cam)) cc->clear_color = s.clear_color;
        } else {
            IGNIS_WARN("no active camera to apply the clear color to");
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("explicitly copy the scene default clear color into the active camera component");
    ImGui::Separator();
    if (ImGui::Button("Reset to defaults")) {
        s = SceneSettings{};
        physics_->set_gravity(s.gravity);
        IGNIS_INFO("scene settings reset to defaults");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("restore default gravity, time scale, ambient and clear color");
    ImGui::TextDisabled("saved into the scene JSON (Ctrl+S) and used by the runtime/export");
}

void EditorApp::draw_status_bar(f32 dt) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - 22.f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, 22.f));
    ImGui::Begin("##statusbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
    const RenderStats st = renderer_->stats();
    const char* gizmo_name = gizmo_op_ == GizmoOp::Translate ? "Translate"
                           : gizmo_op_ == GizmoOp::Rotate ? "Rotate" : "Scale";
    ImGui::Text("%.0f fps  %.2f ms | draws %u | tris %u | entities %llu | sel %zu | gizmo %s | undo %u | redo %u | %s",
                static_cast<double>(dt > 0.f ? 1.f / dt : 0.f), static_cast<double>(dt * 1000.f),
                st.draw_calls, st.triangles,
                static_cast<unsigned long long>(scene_->world().entity_count()), selection_.size(),
                gizmo_name, history_ ? history_->undo_depth() : 0u, history_ ? history_->redo_depth() : 0u,
                playing_ ? "playing" : "paused");
    ImGui::End();
}

void EditorApp::draw_viewport_gizmo(const Camera& camera, f32 aspect) {
    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetRect(0.f, 0.f, io.DisplaySize.x, io.DisplaySize.y);
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
    ImGuizmo::SetOrthographic(camera.orthographic);
    if (selected_ == NULL_ENTITY || !scene_->world().alive(selected_)) return;
    auto* tc = scene_->world().get<TransformComponent>(selected_);
    if (!tc) return;
    const math::Mat4 view = camera.view_matrix();
    const math::Mat4 proj = camera.projection_matrix(aspect);
    math::Mat4 model = tc->local_matrix();
    const ImGuizmo::OPERATION op = gizmo_op_ == GizmoOp::Translate ? ImGuizmo::TRANSLATE
                                   : gizmo_op_ == GizmoOp::Rotate ? ImGuizmo::ROTATE
                                   : ImGuizmo::SCALE;
    const ImGuizmo::MODE mode = gizmo_world_space_ ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
    history_->begin("gizmo " + std::string(gizmo_op_ == GizmoOp::Translate ? "translate"
                                          : gizmo_op_ == GizmoOp::Rotate ? "rotate" : "scale"));
    f32 snap = 0.f;
    const f32* snap_ptr = nullptr;
    if (gizmo_snap_) {
        snap = gizmo_op_ == GizmoOp::Translate ? snap_translate_
             : gizmo_op_ == GizmoOp::Rotate ? glm::radians(snap_rotate_deg_)
             : snap_scale_;
        snap_ptr = &snap;
    }
    if (ImGuizmo::Manipulate(&view[0][0], &proj[0][0], op, mode, &model[0][0], nullptr, snap_ptr)) {
        f32 t[3] = {0.f, 0.f, 0.f}, r[4] = {0.f, 0.f, 0.f, 1.f}, s[3] = {1.f, 1.f, 1.f};
        ImGuizmo::DecomposeMatrixToComponents(&model[0][0], t, r, s);
        tc->position = {t[0], t[1], t[2]};
        tc->rotation = glm::normalize(math::Quat(r[3], r[0], r[1], r[2])); // (w,x,y,z)
        tc->scale = {s[0], s[1], s[2]};
    }
}

// Orientation gizmo in the viewport's top-right corner: click an axis ball to
// snap the view to that axis-aligned orientation (ImGuizmo::ViewManipulate
// rewrites the view matrix in place while animating).
void EditorApp::draw_view_nav_gizmo(const Camera& camera, f32 aspect, EntityId cam_entity) {
    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetRect(0.f, 0.f, io.DisplaySize.x, io.DisplaySize.y);
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
    math::Mat4 view = camera.view_matrix();
    const math::Mat4 before = view;
    // The cube widget only reads the projection for handedness detection, so
    // always feed it a perspective matrix (right-handed) even in ortho mode.
    const math::Mat4 proj = math::perspective_vk(math::radians(camera.fov_y_deg), aspect > 0.f ? aspect : 1.f,
                                                 camera.z_near, camera.z_far);
    f32 dummy_model[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
                           0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
    const f32 size = 96.f;
    const ImVec2 pos(io.DisplaySize.x - size - 12.f, ImGui::GetFrameHeight() + 12.f);
    ImGuizmo::ViewManipulate(&view[0][0], &proj[0][0], ImGuizmo::TRANSLATE, ImGuizmo::WORLD, dummy_model,
                             1.f, pos, ImVec2(size, size), IM_COL32(30, 30, 30, 160));
    if (std::memcmp(&before, &view, sizeof(view)) != 0) {
        // The gizmo only rotates (the eye position is preserved): extract the
        // new orientation and write it to the current view target.
        const math::Mat4 world = glm::inverse(view);
        const math::Quat q = glm::normalize(glm::quat_cast(glm::mat3(world)));
        sync_yaw_pitch_from(q);
        apply_view_rotation(glm::angleAxis(cam_yaw_, math::Vec3(0.f, 1.f, 0.f)) *
                                glm::angleAxis(cam_pitch_, math::Vec3(1.f, 0.f, 0.f)),
                            cam_entity);
    }
}

void EditorApp::draw_viewport_overlay(const Camera& camera, EntityId cam_entity) {
    if (!show_overlay_) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const char* cam_name =
        cam_entity != NULL_ENTITY && scene_->world().alive(cam_entity)
            ? scene_->world().name_of(cam_entity).c_str() : "(none)";
    const char* sel_name =
        selected_ != NULL_ENTITY && scene_->world().alive(selected_)
            ? scene_->world().name_of(selected_).c_str() : "none";
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "scene: %s  [%s]\ncamera: %s%s  pos(%.1f, %.1f, %.1f)  %s\nselected: %s\n"
                  "RMB look | WASDQE fly | wheel dolly | LMB click/drag = select/marquee | W/E/R gizmo | F / Shift+F | Ctrl+S | P",
                  scene_->name().c_str(), scene_path_.c_str(), cam_name,
                  standalone_cam_ ? " (editor cam)" : "",
                  static_cast<double>(camera.position.x), static_cast<double>(camera.position.y),
                  static_cast<double>(camera.position.z), camera.orthographic ? "ortho" : "persp",
                  sel_name);
    const ImVec2 pos(10.f, ImGui::GetFrameHeight() * 2.f + 10.f);
    dl->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), IM_COL32(0, 0, 0, 180), buf);
    dl->AddText(pos, IM_COL32(220, 220, 220, 255), buf);
    // Rubber-band marquee while dragging on empty viewport space.
    if (marquee_active_) {
        const ImVec2 a(lmb_press_.x, lmb_press_.y);
        const ImVec2 b(marquee_end_.x, marquee_end_.y);
        const ImVec2 mn(std::min(a.x, b.x), std::min(a.y, b.y));
        const ImVec2 mx(std::max(a.x, b.x), std::max(a.y, b.y));
        dl->AddRectFilled(mn, mx, IM_COL32(80, 140, 255, 40));
        dl->AddRect(mn, mx, IM_COL32(120, 170, 255, 220));
    }
}

// Geometry for the viewport debug toggles, fed into the renderer's DebugLine
// channel. Geometry generation lives in physics (debug_draw.h) for colliders;
// grid/axes/selection are trivial world-space segments built here.
std::vector<DebugLine> EditorApp::build_debug_lines() {
    constexpr u32 kMaxDebugLines = 4096; // matches the renderer's line VB capacity
    std::vector<DebugLine> out;
    auto add = [&out](const DebugSegment& s, const math::Vec4& color) {
        if (out.size() < kMaxDebugLines) out.push_back({s.a, s.b, color});
    };
    if (dbg_axes_) {
        const math::Vec3 o(0.f);
        add({o, math::Vec3(1.f, 0.f, 0.f)}, {1.f, 0.2f, 0.2f, 1.f}); // X red
        add({o, math::Vec3(0.f, 1.f, 0.f)}, {0.2f, 1.f, 0.2f, 1.f}); // Y green
        add({o, math::Vec3(0.f, 0.f, 1.f)}, {0.2f, 0.4f, 1.f, 1.f}); // Z blue
    }
    if (dbg_grid_) {
        // XZ plane at y=0, 1m cells, +/-10m.
        const math::Vec4 c(0.35f, 0.35f, 0.38f, 1.f);
        for (int i = -10; i <= 10; ++i) {
            const f32 v = static_cast<f32>(i);
            add({math::Vec3(v, 0.f, -10.f), math::Vec3(v, 0.f, 10.f)}, c);
            add({math::Vec3(-10.f, 0.f, v), math::Vec3(10.f, 0.f, v)}, c);
        }
    }
    World& w = scene_->world();
    if (dbg_selection_ && selected_ != NULL_ENTITY && w.alive(selected_)) {
        if (const auto* tc = w.get<TransformComponent>(selected_)) {
            const math::Mat4 wm = world_matrix(w, selected_);
            const math::Vec3 center{static_cast<f32>(wm[3][0]), static_cast<f32>(wm[3][1]),
                                    static_cast<f32>(wm[3][2])};
            // Prefer the collider's own size when present: box half extents,
            // or the radius for sphere/capsule.
            math::Vec3 half = tc->scale * 0.5f;
            if (const auto* col = w.get<ColliderComponent>(selected_)) {
                if (col->shape == ColliderComponent::Shape::Box) half = col->half_extents;
                else half = math::Vec3(col->radius);
            }
            for (const auto& s : aabb_edges(center, half))
                add(s, math::Vec4(1.f, 0.5f, 0.1f, 1.f)); // orange
        }
    }
    if (dbg_colliders_) {
        for (EntityId e : w.all_entities()) {
            const auto* col = w.get<ColliderComponent>(e);
            if (!col) continue;
            math::Vec4 color(0.8f, 0.8f, 0.8f, 1.f); // no rigid body: grey
            if (const auto* rb = w.get<RigidBodyComponent>(e)) {
                color = rb->type == RigidBodyComponent::Type::Static ? math::Vec4(0.55f, 0.55f, 0.55f, 1.f)
                      : rb->type == RigidBodyComponent::Type::Kinematic ? math::Vec4(1.f, 0.85f, 0.3f, 1.f)
                                                                        : math::Vec4(0.3f, 1.f, 0.4f, 1.f);
            }
            // Full collider transform: entity hierarchy and rotation included.
            const math::Mat4 wm = world_matrix(w, e);
            for (const auto& s : collider_wireframe_edges(*col, wm)) add(s, color);
        }
    }
    if (out.size() >= kMaxDebugLines) {
        if (!dbg_cap_warned_) { // warn once per overflow, not every frame
            IGNIS_WARN("debug lines capped at {} segments", kMaxDebugLines);
            dbg_cap_warned_ = true;
        }
    } else {
        dbg_cap_warned_ = false;
    }
    return out;
}

// Default workspace: Hierarchy docked left, Inspector right, Bottom panel at
// the bottom; the center stays the 3D viewport. Applied once when no saved
// layout exists (or after View > Reset dock layout).
void EditorApp::build_default_dock_layout(u32 dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);
    ImGuiID main = dockspace_id;
    const ImGuiID left = ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, 0.22f, nullptr, &main);
    const ImGuiID right = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.25f, nullptr, &main);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.30f, nullptr, &main);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Bottom", bottom);
    ImGui::DockBuilderFinish(main);
}

void EditorApp::main_loop() {
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    // Route engine logs into the in-app console.
    Logger::instance().set_level(LogLevel::Debug);
    Logger::instance().set_listener([this](LogLevel l, const std::string& m) {
        std::lock_guard lock(console_mutex_);
        console_.emplace_back(l, m);
        if (console_.size() > 2000) console_.erase(console_.begin(), console_.begin() + 500);
    });

    update_window_title();
    // Mouse wheel accumulates here; handle_viewport_input consumes it (dolly/zoom).
    window_->on_scroll = [this](f64 /*x*/, f64 y) { scroll_pending_ += y; };
    Camera camera;
    const EntityId camEntity = find_active_camera();
    publish_view_camera(camera, camEntity);
    sync_yaw_pitch_from(camera.rotation);
    view_cam_ = camera;

    while (!window_->should_close()) {
        window_->poll_events();
        const f32 dt = window_->delta_time();

        const FramebufferSize fb = window_->framebuffer_size();
        if (fb.width == 0 || fb.height == 0) continue; // minimized: suspend rendering

        // HiDPI: re-apply the ImGui style/font scale when the content scale
        // changes (monitor switch, OS zoom). Resets to base_style_ first, so
        // repeated changes don't accumulate.
        const ContentScale cs = window_->content_scale();
        if (std::fabs(cs.x - content_scale_) > 0.01f) apply_content_scale(cs.x);

        handle_viewport_input(camera, camEntity, dt);
        // Asset dragged from the browser and released over the viewport: spawn
        // it at the raycast hit point (scene files ask first).
        if (const ImGuiPayload* pl = ImGui::GetDragDropPayload()) {
            if (std::strcmp(pl->DataType, "IGNIS_ASSET") == 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                !io.WantCaptureMouse) {
                double dmx = 0.0, dmy = 0.0;
                glfwGetCursorPos(window_->handle(), &dmx, &dmy);
                spawn_asset_at(static_cast<const char*>(pl->Data), static_cast<f32>(dmx), static_cast<f32>(dmy));
            }
        }
        // time_scale affects simulation time only, never the render frame rate
        if (playing_) scene_->update(dt * scene_->settings().time_scale);
        publish_view_camera(camera, camEntity);
        view_cam_ = camera;
        handle_shortcuts();
        // Dirty = current scene differs from the last save/load snapshot
        // (content comparison, not a mutation flag — see issue #19 review).
        dirty_ = scene_->serialize() != saved_snapshot_;

        // --- scene render first: a pending resize is processed here (after
        // waitForIdle, swapchain recreated) before the UI frame starts.
        renderer_->set_debug_lines(build_debug_lines());
        renderer_->render(*scene_, camera, dt);

        // If the swapchain was recreated this frame, rebuild the
        // swapchain-dependent UI resources before starting the ImGui frame.
        RawVulkanHandles rhNow = renderer_->raw_handles();
        if (rhNow.windowed &&
            (rhNow.width != ui_.fb_width || rhNow.height != ui_.fb_height ||
             rhNow.swap_image_count != ui_.image_count ||
             rhNow.swap_format != static_cast<i32>(ui_.format))) {
            rebuild_ui_after_swapchain_change();
            rhNow = renderer_->raw_handles();
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame(); // must come after ImGui::NewFrame: it calls ImGui::Begin internally

        // Docking workspace covering the main viewport (#56). The central node
        // is passthrough so the 3D scene shows through and stays clickable.
        const ImGuiID dockspace =
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
        if (apply_default_layout_) {
            apply_default_layout_ = false;
            build_default_dock_layout(dockspace);
        }

        draw_menu_bar();
        draw_toolbar();
        if (show_hierarchy_) draw_hierarchy();
        if (show_inspector_) draw_inspector();
        if (show_bottom_) draw_bottom_panel(dt);
        if (show_statusbar_) draw_status_bar(dt);
        draw_save_as_modal();
        draw_prefab_modal();
        draw_asset_modals();
        draw_palette();
        draw_save_prompt();
        draw_help_window();
        const f32 aspect = fb.height > 0 ? static_cast<f32>(fb.width) / static_cast<f32>(fb.height) : 1.f;
        draw_viewport_gizmo(camera, aspect);
        draw_view_nav_gizmo(camera, aspect, camEntity);
        draw_viewport_overlay(camera, camEntity);

        // Close the open edit transaction once no widget/gizmo is being
        // dragged anymore; coalescing made the whole drag one entry.
        if (history_ && !ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing()) history_->commit();

        ImGui::Render();

        // If render() produced no frame this frame (e.g. swapchain out of date
        // and recreation pending), skip the overlay + present: presenting a
        // stale image is what causes visible flicker.
        if (!renderer_->frame_rendered()) continue;

        // --- ImGui overlay pass on the swapchain image ------------------------------
        // (the scene was already rendered above; rhNow reflects the current
        // swapchain, possibly recreated this frame)
        const u32 swapIndex = rhNow.swap_index;
        VkDevice device = ui_.device;
        vkResetCommandPool(device, ui_.cmd_pool, 0);
        VkCommandBuffer uiCB = ui_.cmd_buffer;
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(uiCB, &bi);
        VkRenderPassBeginInfo rpi{};
        rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpi.renderPass = ui_.render_pass;
        rpi.framebuffer = ui_.fbs[swapIndex];
        rpi.renderArea.offset = {0, 0};
        rpi.renderArea.extent = {rhNow.width, rhNow.height};
        vkCmdBeginRenderPass(uiCB, &rpi, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), uiCB);
        vkCmdEndRenderPass(uiCB);
        vkEndCommandBuffer(uiCB);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &uiCB;
        vkQueueSubmit(ui_.queue, 1, &si, VK_NULL_HANDLE);

        renderer_->present();
    }
}

void EditorApp::shutdown_ui() {
    save_editor_state(); // persist viewport/camera prefs for the next session
    save_window_settings();
    // Lifetime order matters (issue #53): UI Vulkan resources and the ImGui
    // backend must be destroyed BEFORE the renderer, because the renderer
    // owns the VkDevice they were created on.
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    const VkDevice device = ui_.device;
    if (device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, ui_.cmd_pool, nullptr);
        destroy_ui_swapchain_resources();
        vkDestroyDescriptorPool(device, ui_.pool, nullptr);
    }
    renderer_->shutdown(); // destroys the device; waits for idle internally
    window_->shutdown();
}

#endif // IGNIS_WITH_GRAPHICS

} // namespace ignis
