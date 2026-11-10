// Editor application: window + Vulkan renderer + ImGui overlay (hierarchy,
// inspector, play/pause, save) + AI-agent command interface (--commands).
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
#include <ImGuizmo.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include "ignis/ecs/component_registry.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
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
    if (auto r = init_imgui_ui(); !r.ok()) {
        IGNIS_ERROR("ImGui UI init failed: {}", r.error());
        return 1;
    }
    main_loop(scene_path);
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
    props.resizable = false; // windowed swapchain recreation is not implemented yet
    window_ = std::make_unique<Window>(props);
    if (auto r = window_->init(); !r.ok())
        return Result<void>::failure("window init failed: " + r.error());

    assets_ = std::make_unique<AssetManager>();
    renderer_ = std::make_unique<VulkanRenderer>();
    renderer_->set_asset_manager(assets_.get());
    if (auto r = renderer_->init(window_.get()); !r.ok())
        return Result<void>::failure("renderer init failed: " + r.error());
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
    if (auto r = physics_->init({0.f, -9.81f, 0.f}); !r.ok())
        return Result<void>::failure("physics init failed: " + r.error());
    PhysicsSystem::register_lua_bindings(scripts_->state());
    scene_->add_system([&](World&, f32 dt) { scripts_->update(dt); });
    scene_->add_system([&](World& w, f32 dt) { physics_->update(w, dt); });
    scene_->add_system([&](World& w, f32 dt) { audio_sys_->update(w, dt); });

    // Agent commands (AI-controllable).
    commands_ = std::make_unique<CommandProcessor>(*scene_, *assets_);
    if (!commands_path.empty()) {
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
    const VkSwapchainKHR swapchain = static_cast<VkSwapchainKHR>(rh.swapchain);
    const VkFormat swapFormat = static_cast<VkFormat>(rh.swap_format);
    u32 imageCount = rh.swap_image_count;
    std::vector<VkImage> swapImages(imageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapImages.data());
    ui_.image_count = imageCount;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
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

    // UI render pass: loads the scene-rendered swapchain image (NVRHI leaves
    // it in PRESENT_SRC_KHR thanks to keepInitialState) and keeps it there.
    {
        VkRenderPass& uiRenderPass = ui_.render_pass;
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
        vkCreateRenderPass(device, &ci, nullptr, &uiRenderPass);
    }

    ui_.views.resize(imageCount);
    ui_.fbs.resize(imageCount);
    VkRenderPass& uiRenderPass = ui_.render_pass;
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
        fi.renderPass = uiRenderPass;
        fi.attachmentCount = 1;
        fi.pAttachments = &ui_.views[i];
        fi.width = rh.width;
        fi.height = rh.height;
        fi.layers = 1;
        vkCreateFramebuffer(device, &fi, nullptr, &ui_.fbs[i]);
    }

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
    imguiInfo.ImageCount = imageCount;
    imguiInfo.UseDynamicRendering = false;
    imguiInfo.PipelineInfoMain.RenderPass = ui_.render_pass;
    imguiInfo.PipelineInfoMain.Subpass = 0;
    imguiInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    imguiInfo.CheckVkResultFn = check_vk;
    ImGui_ImplVulkan_Init(&imguiInfo); // font texture is created lazily on first NewFrame
    return Result<void>::success();
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
    }
    last_mx_ = mx;
    last_my_ = my;
    // Fly camera: RMB drag to look, WASDQE to move (writes the scene camera
    // entity's transform, so the camera stays editable like any object).
    if (rmb_down_ && cam_entity != NULL_ENTITY && scene_->world().alive(cam_entity)) {
        auto* tc = scene_->world().get<TransformComponent>(cam_entity);
        if (tc) {
            tc->rotation = glm::angleAxis(cam_yaw_, math::Vec3(0.f, 1.f, 0.f)) *
                           glm::angleAxis(cam_pitch_, math::Vec3(1.f, 0.f, 0.f));
            const math::Vec3 fwd = tc->rotation * math::Vec3(0.f, 0.f, -1.f);
            const math::Vec3 right = tc->rotation * math::Vec3(1.f, 0.f, 0.f);
            const math::Vec3 up = math::Vec3(0.f, 1.f, 0.f);
            const f32 speed = (glfwGetKey(glfw, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) ? 20.f : 6.f;
            if (glfwGetKey(glfw, GLFW_KEY_W) == GLFW_PRESS) tc->position += fwd * speed * dt;
            if (glfwGetKey(glfw, GLFW_KEY_S) == GLFW_PRESS) tc->position -= fwd * speed * dt;
            if (glfwGetKey(glfw, GLFW_KEY_D) == GLFW_PRESS) tc->position += right * speed * dt;
            if (glfwGetKey(glfw, GLFW_KEY_A) == GLFW_PRESS) tc->position -= right * speed * dt;
            if (glfwGetKey(glfw, GLFW_KEY_E) == GLFW_PRESS) tc->position += up * speed * dt;
            if (glfwGetKey(glfw, GLFW_KEY_Q) == GLFW_PRESS) tc->position -= up * speed * dt;
        }
    }
    update_camera(camera, cam_entity);

    // Click to select (ray vs. mesh AABBs), unless the click went to the UI.
    const bool lmb = glfwGetMouseButton(glfw, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (lmb && !left_clicked_ && !io.WantCaptureMouse) {
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
            if (best != NULL_ENTITY) selected_ = best;
        }
    }
    left_clicked_ = lmb;
}

void EditorApp::handle_shortcuts(const std::string& scene_path) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) return;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmo_op_ = GizmoOp::Translate;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmo_op_ = GizmoOp::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmo_op_ = GizmoOp::Scale;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && selected_ != NULL_ENTITY &&
        scene_->world().alive(selected_)) {
        delete_entity(selected_);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        if (auto r = scene_->save_to_file(scene_path); r.ok()) IGNIS_INFO("scene saved to {}", scene_path);
        else IGNIS_ERROR("{}", r.error());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) playing_ = !playing_;
}

void EditorApp::delete_entity(EntityId e) {
    if (!scene_->world().alive(e)) return;
    scripts_->on_entity_destroyed(e); // fire on_destroy
    scene_->world().destroy_entity(e);
    if (selected_ == e) selected_ = NULL_ENTITY;
    IGNIS_INFO("deleted entity {}", e);
}

void EditorApp::duplicate_entity(EntityId e) {
    if (!scene_->world().alive(e)) return;
    World& w = scene_->world();
    EntityId copy = w.create_entity(w.name_of(e) + " copy");
    w.set_parent(copy, w.parent_of(e));
    if (const auto* t = w.get<TransformComponent>(e)) {
        auto& ct = w.emplace<TransformComponent>(copy);
        ct = *t;
        ct.position.x += 1.f;
    }
    for (const auto& [type, storage] : w.storages()) {
        if (!storage->has(e)) continue;
        if (const ComponentTypeInfo* info = w.registry().find_by_type(type))
            info->copy(w, e, copy);
    }
    selected_ = copy;
}

void EditorApp::reload_scene(const std::string& scene_path) {
    playing_ = false;
    selected_ = NULL_ENTITY;
    if (auto r = scene_->load_file(scene_path); r.ok()) IGNIS_INFO("scene reloaded from {}", scene_path);
    else IGNIS_ERROR("{}", r.error());
}

void EditorApp::draw_menu_bar(const std::string& scene_path) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            if (auto r = scene_->save_to_file(scene_path); r.ok()) IGNIS_INFO("scene saved to {}", scene_path);
            else IGNIS_ERROR("{}", r.error());
        }
        if (ImGui::MenuItem("Reload scene")) reload_scene(scene_path);
        if (ImGui::MenuItem("Quit", "Esc")) window_->request_close();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool has_sel = selected_ != NULL_ENTITY && scene_->world().alive(selected_);
        if (ImGui::MenuItem("Delete selected", "Del", false, has_sel)) delete_entity(selected_);
        if (ImGui::MenuItem("Duplicate selected", nullptr, false, has_sel)) duplicate_entity(selected_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Hierarchy", nullptr, &show_hierarchy_);
        ImGui::MenuItem("Inspector", nullptr, &show_inspector_);
        ImGui::MenuItem("Bottom panel", nullptr, &show_bottom_);
        ImGui::MenuItem("Status bar", nullptr, &show_statusbar_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Run")) {
        if (ImGui::MenuItem(playing_ ? "Pause" : "Play", "P")) playing_ = !playing_;
        if (ImGui::MenuItem("Stop (reload)", nullptr, false, true)) reload_scene(scene_path);
        ImGui::EndMenu();
    }
    ImGui::TextDisabled("%s", playing_ ? "[playing]" : "[paused]");
    ImGui::EndMainMenuBar();
}

void EditorApp::draw_toolbar(const std::string& scene_path) {
    (void)scene_path;
    ImGui::SetNextWindowPos(ImVec2(0.f, ImGui::GetFrameHeight()), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetMainViewport()->WorkSize.x, 0.f));
    ImGui::Begin("##toolbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings);
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
    if (ImGui::Button(playing_ ? "||" : ">")) playing_ = !playing_;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (P)", playing_ ? "Pause" : "Play");
    ImGui::SameLine();
    if (ImGui::Button("[]")) reload_scene(scene_path);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop and reload scene");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (selected_ != NULL_ENTITY && scene_->world().alive(selected_))
        ImGui::Text("Selected: %s (#%llu)", scene_->world().name_of(selected_).c_str(),
                    static_cast<unsigned long long>(selected_));
    else
        ImGui::TextDisabled("Selected: none (click an object in the viewport)");
    ImGui::End();
}

void EditorApp::draw_hierarchy_node(EntityId e) {
    World& w = scene_->world();
    const auto& children = w.children_of(e);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                ImGuiTreeNodeFlags_DefaultOpen;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (selected_ == e) flags |= ImGuiTreeNodeFlags_Selected;
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(e)), flags,
                                        "%s", w.name_of(e).c_str());
    if (ImGui::IsItemClicked()) selected_ = e;
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Create child")) {
            EntityId c = w.create_entity("New Entity");
            w.set_parent(c, e);
            selected_ = c;
        }
        if (ImGui::MenuItem("Rename")) {
            rename_target_ = e;
            std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", w.name_of(e).c_str());
        }
        if (ImGui::MenuItem("Duplicate")) duplicate_entity(e);
        if (ImGui::MenuItem("Delete")) delete_entity(e);
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
        if (ImGui::MenuItem("Create empty entity")) {
            selected_ = w.create_entity("New Entity");
        }
        ImGui::EndPopup();
    }
    std::vector<EntityId> roots;
    for (EntityId e : w.all_entities())
        if (w.parent_of(e) == NULL_ENTITY) roots.push_back(e);
    std::sort(roots.begin(), roots.end());
    for (EntityId e : roots) draw_hierarchy_node(e);

    // Rename modal.
    if (rename_target_ != NULL_ENTITY && w.alive(rename_target_)) {
        ImGui::OpenPopup("Rename entity");
        if (ImGui::BeginPopupModal("Rename entity", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::InputText("name", rename_buf_, sizeof(rename_buf_));
            if (ImGui::Button("OK")) {
                w.set_name(rename_target_, rename_buf_);
                rename_target_ = NULL_ENTITY;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                rename_target_ = NULL_ENTITY;
                ImGui::CloseCurrentPopup();
            }
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
        if (ImGui::InputText("name", name_buf, sizeof(name_buf))) w.set_name(selected_, name_buf);
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
        ImGui::SameLine(ImGui::GetWindowWidth() - 30.f);
        if (ImGui::SmallButton("x")) {
            info->remove(w, selected_);
            ImGui::PopID();
            continue;
        }
        if (open) {
            if (info->name == "Transform") edit_transform(*w.get<TransformComponent>(selected_));
            else if (info->name == "Camera") edit_camera(*w.get<CameraComponent>(selected_));
            else if (info->name == "DirectionalLight") edit_directional_light(*w.get<DirectionalLightComponent>(selected_));
            else if (info->name == "PointLight") edit_point_light(*w.get<PointLightComponent>(selected_));
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
                ImGui::SameLine();
                if (ImGui::Button("Stop")) audio_sys_->stop_source(selected_, w);
                ImGui::SameLine();
                ImGui::TextDisabled(audio_sys_->source_playing(selected_) ? "playing" : "stopped");
            }
        }
        ImGui::PopID();
    }

    // Add Component combo: every registered type the entity doesn't have yet.
    if (ImGui::BeginCombo("Add Component", "+ add...")) {
        for (const std::string& name : w.registry().names()) {
            const ComponentTypeInfo* info = w.registry().find_by_name(name);
            if (!info) continue;
            bool has = false;
            for (const auto& [type, storage] : w.storages())
                if (type == info->type) { has = storage->has(selected_); break; }
            if (has) continue;
            if (ImGui::Selectable(name.c_str())) info->add_default(w, selected_);
        }
        ImGui::EndCombo();
    }
    ImGui::End();
}

void EditorApp::draw_console() {
    ImGui::InputText("filter", console_filter_, sizeof(console_filter_));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        std::lock_guard lock(console_mutex_);
        console_.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("auto-scroll", &console_autoscroll_);
    const std::string filter = console_filter_;
    ImGui::BeginChild("##console_scroll", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders);
    std::lock_guard lock(console_mutex_);
    for (const auto& [level, msg] : console_) {
        if (!filter.empty() && msg.find(filter) == std::string::npos) continue;
        ImGui::TextColored(log_color(level), "[%s] %s", to_string(level), msg.c_str());
    }
    if (console_autoscroll_) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void EditorApp::draw_asset_browser() {
    namespace fs = std::filesystem;
    ImGui::TextDisabled("assets/  scripts/  (click a file to select)");
    ImGui::Separator();
    ImGui::BeginChild("##assets_scroll");
    const fs::path roots[] = {"assets", "scripts"};
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
            } else {
                const bool sel = selected_asset_ == fs::relative(entry.path(), root).generic_string();
                if (ImGui::Selectable(name.c_str(), sel)) selected_asset_ = fs::relative(entry.path(), root).generic_string();
                ImGui::SameLine();
                ImGui::TextDisabled("%llu B", static_cast<unsigned long long>(entry.file_size()));
            }
        }
    };
    for (const auto& root : roots) {
        if (ImGui::TreeNode(root.c_str())) {
            draw_dir(root, root);
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
    if (!selected_asset_.empty()) ImGui::Text("selected: %s", selected_asset_.c_str());
}

void EditorApp::draw_stats(f32 dt) {
    const RenderStats st = renderer_->stats();
    ImGui::Text("draw calls: %u", st.draw_calls);
    ImGui::Text("triangles:  %u", st.triangles);
    ImGui::Text("frame:      %.2f ms (%.0f fps)", static_cast<double>(dt * 1000.f),
                static_cast<double>(dt > 0.f ? 1.f / dt : 0.f));
    ImGui::Text("entities:   %llu", static_cast<unsigned long long>(scene_->world().entity_count()));
    ImGui::Text("scene:      %s", scene_->name().c_str());
}

void EditorApp::draw_bottom_panel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - 240.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560.f, 220.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Bottom", &show_bottom_)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("##bottom_tabs")) {
        if (ImGui::BeginTabItem("Console")) { bottom_tab_ = 0; draw_console(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Assets")) { bottom_tab_ = 1; draw_asset_browser(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Stats")) { bottom_tab_ = 2; draw_stats(0.f); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void EditorApp::draw_status_bar(f32 dt) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - 22.f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, 22.f));
    ImGui::Begin("##statusbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings);
    const RenderStats st = renderer_->stats();
    const char* gizmo_name = gizmo_op_ == GizmoOp::Translate ? "Translate"
                           : gizmo_op_ == GizmoOp::Rotate ? "Rotate" : "Scale";
    ImGui::Text("%.0f fps  %.2f ms | draws %u | tris %u | entities %llu | gizmo %s | %s",
                static_cast<double>(dt > 0.f ? 1.f / dt : 0.f), static_cast<double>(dt * 1000.f),
                st.draw_calls, st.triangles,
                static_cast<unsigned long long>(scene_->world().entity_count()), gizmo_name,
                playing_ ? "playing" : "paused");
    ImGui::End();
}

void EditorApp::draw_viewport_gizmo(const Camera& camera, f32 aspect) {
    if (selected_ == NULL_ENTITY || !scene_->world().alive(selected_)) return;
    auto* tc = scene_->world().get<TransformComponent>(selected_);
    if (!tc) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetRect(0.f, 0.f, io.DisplaySize.x, io.DisplaySize.y);
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
    const math::Mat4 view = camera.view_matrix();
    const math::Mat4 proj = camera.projection_matrix(aspect);
    math::Mat4 model = tc->local_matrix();
    const ImGuizmo::OPERATION op = gizmo_op_ == GizmoOp::Translate ? ImGuizmo::TRANSLATE
                                   : gizmo_op_ == GizmoOp::Rotate ? ImGuizmo::ROTATE
                                   : ImGuizmo::SCALE;
    if (ImGuizmo::Manipulate(&view[0][0], &proj[0][0], op, ImGuizmo::LOCAL, &model[0][0])) {
        f32 t[3] = {0.f, 0.f, 0.f}, r[4] = {0.f, 0.f, 0.f, 1.f}, s[3] = {1.f, 1.f, 1.f};
        ImGuizmo::DecomposeMatrixToComponents(&model[0][0], t, r, s);
        tc->position = {t[0], t[1], t[2]};
        tc->rotation = glm::normalize(math::Quat(r[3], r[0], r[1], r[2])); // (w,x,y,z)
        tc->scale = {s[0], s[1], s[2]};
    }
}

void EditorApp::main_loop(const std::string& scene_path) {
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    // Route engine logs into the in-app console.
    Logger::instance().set_level(LogLevel::Debug);
    Logger::instance().set_listener([this](LogLevel l, const std::string& m) {
        std::lock_guard lock(console_mutex_);
        console_.emplace_back(l, m);
        if (console_.size() > 2000) console_.erase(console_.begin(), console_.begin() + 500);
    });

    Camera camera;
    const EntityId camEntity = find_active_camera();
    update_camera(camera, camEntity);

    while (!window_->should_close()) {
        window_->poll_events();
        const f32 dt = window_->delta_time();

        handle_viewport_input(camera, camEntity, dt);
        if (playing_) scene_->update(dt);
        update_camera(camera, camEntity);
        handle_shortcuts(scene_path);

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGuizmo::BeginFrame();
        ImGui::NewFrame();

        draw_menu_bar(scene_path);
        draw_toolbar(scene_path);
        if (show_hierarchy_) draw_hierarchy();
        if (show_inspector_) draw_inspector();
        if (show_bottom_) draw_bottom_panel();
        if (show_statusbar_) draw_status_bar(dt);
        const FramebufferSize fb = window_->framebuffer_size();
        const f32 aspect = fb.height > 0 ? static_cast<f32>(fb.width) / static_cast<f32>(fb.height) : 1.f;
        draw_viewport_gizmo(camera, aspect);

        ImGui::Render();

        // --- scene render (no present yet) ----------------------------------------
        renderer_->render(*scene_, camera, dt);

        // --- ImGui overlay pass on the swapchain image ------------------------------
        const RawVulkanHandles rhNow = renderer_->raw_handles();
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
    renderer_->shutdown(); // waits for idle internally
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    const VkDevice device = ui_.device;
    vkDestroyCommandPool(device, ui_.cmd_pool, nullptr);
    for (u32 i = 0; i < ui_.image_count; ++i) {
        vkDestroyFramebuffer(device, ui_.fbs[i], nullptr);
        vkDestroyImageView(device, ui_.views[i], nullptr);
    }
    vkDestroyRenderPass(device, ui_.render_pass, nullptr);
    vkDestroyDescriptorPool(device, ui_.pool, nullptr);
    window_->shutdown();
}

#endif // IGNIS_WITH_GRAPHICS

} // namespace ignis
