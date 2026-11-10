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
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
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

void EditorApp::main_loop(const std::string& scene_path) {
    ImGuiIO& io = ImGui::GetIO();

    // --- editor state -----------------------------------------------------------
    Camera camera;
    EntityId camEntity = NULL_ENTITY;
    for (EntityId e : scene_->world().all_entities()) {
        if (const auto* cc = scene_->world().get<CameraComponent>(e)) {
            if (cc->active) camEntity = e;
        }
    }
    f32 camYaw = 0.f, camPitch = 0.f;
    bool rmbDown = false;
    f64 lastMX = 0.0, lastMY = 0.0;
    bool leftClicked = false;

    auto update_camera_from_scene = [&]() {
        camera = Camera{};
        if (camEntity != NULL_ENTITY && scene_->world().alive(camEntity)) {
            const auto* cc = scene_->world().get<CameraComponent>(camEntity);
            const auto* tc = scene_->world().get<TransformComponent>(camEntity);
            if (cc && tc) {
                camera.position = tc->position;
                camera.rotation = tc->rotation;
                camera.fov_y_deg = cc->fov_y_deg;
                camera.z_near = cc->z_near;
                camera.z_far = cc->z_far;
            }
        }
    };
    update_camera_from_scene();

    // --- main loop ---------------------------------------------------------------
    while (!window_->should_close()) {
        window_->poll_events();
        const f32 dt = window_->delta_time();

        // Fly camera: RMB drag to look, WASDQE to move (writes the scene camera
        // entity's transform, so the camera is editable like any object).
        {
            GLFWwindow* glfw = window_->handle();
            const bool rmb = glfwGetMouseButton(glfw, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
            double mx = 0.0, my = 0.0;
            glfwGetCursorPos(glfw, &mx, &my);
            if (rmb && !io.WantCaptureMouse) {
                if (rmbDown) {
                    camYaw -= static_cast<f32>(mx - lastMX) * 0.005f;
                    camPitch -= static_cast<f32>(my - lastMY) * 0.005f;
                    camPitch = glm::clamp(camPitch, -1.55f, 1.55f);
                }
                rmbDown = true;
            } else {
                rmbDown = false;
            }
            lastMX = mx;
            lastMY = my;
            if (rmbDown && camEntity != NULL_ENTITY && scene_->world().alive(camEntity)) {
                auto* tc = scene_->world().get<TransformComponent>(camEntity);
                if (tc) {
                    tc->rotation = glm::angleAxis(camYaw, math::Vec3(0.f, 1.f, 0.f)) *
                                   glm::angleAxis(camPitch, math::Vec3(1.f, 0.f, 0.f));
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
            update_camera_from_scene();

            // Click to select (ray vs. mesh AABBs), unless the click went to the UI.
            const bool lmb = glfwGetMouseButton(glfw, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb && !leftClicked && !io.WantCaptureMouse) {
                const FramebufferSize fb = window_->framebuffer_size();
                if (fb.width > 0 && fb.height > 0) {
                    const f32 ndcX = (static_cast<f32>(mx) / static_cast<f32>(fb.width)) * 2.f - 1.f;
                    const f32 ndcY = (static_cast<f32>(my) / static_cast<f32>(fb.height)) * 2.f - 1.f;
                    const picking::Ray ray = picking::screen_ray(
                        camera, static_cast<f32>(fb.width) / static_cast<f32>(fb.height),
                        ndcX, -ndcY);
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
            leftClicked = lmb;
        }

        if (playing_) scene_->update(dt);

        // --- UI ------------------------------------------------------------------
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save scene", "Ctrl+S")) {
                    if (auto r = scene_->save_to_file(scene_path); r.ok()) IGNIS_INFO("scene saved to {}", scene_path);
                    else IGNIS_ERROR("{}", r.error());
                }
                if (ImGui::MenuItem("Quit", "Esc")) window_->request_close();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Run")) {
                if (ImGui::MenuItem(playing_ ? "Pause" : "Play", "P")) playing_ = !playing_;
                ImGui::EndMenu();
            }
            ImGui::TextDisabled("%s", playing_ ? "[playing]" : "[paused]");
            ImGui::EndMainMenuBar();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
            if (auto r = scene_->save_to_file(scene_path); r.ok()) IGNIS_INFO("scene saved to {}", scene_path);
            else IGNIS_ERROR("{}", r.error());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_P)) playing_ = !playing_;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) window_->request_close();

        // Hierarchy.
        ImGui::SetNextWindowPos(ImVec2(0.f, 19.f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260.f, 400.f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Hierarchy")) {
            for (EntityId e : scene_->world().all_entities()) {
                const std::string& nm = scene_->world().name_of(e);
                char label[128];
                std::snprintf(label, sizeof(label), "%s (#%llu)", nm.c_str(),
                              static_cast<unsigned long long>(e));
                if (ImGui::Selectable(label, e == selected_)) selected_ = e;
            }
        }
        ImGui::End();

        // Inspector.
        ImGui::SetNextWindowPos(ImVec2(1020.f, 19.f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260.f, 400.f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Inspector")) {
            if (selected_ != NULL_ENTITY && scene_->world().alive(selected_)) {
                ImGui::Text("Entity #%llu: %s", static_cast<unsigned long long>(selected_),
                            scene_->world().name_of(selected_).c_str());
                ImGui::Separator();
                auto& w = scene_->world();
                if (auto* tc = w.get<TransformComponent>(selected_)) {
                    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::DragFloat3("position", &tc->position.x, 0.05f);
                        math::Vec4 q{tc->rotation.w, tc->rotation.x, tc->rotation.y, tc->rotation.z};
                        if (ImGui::DragFloat4("rotation (wxyz)", &q.x, 0.01f)) {
                            tc->rotation = glm::normalize(math::Quat(q.x, q.y, q.z, q.w));
                        }
                        ImGui::DragFloat3("scale", &tc->scale.x, 0.05f, 0.01f, 100.f);
                    }
                }
                if (auto* mr = w.get<MeshRendererComponent>(selected_)) {
                    if (ImGui::CollapsingHeader("MeshRenderer", ImGuiTreeNodeFlags_DefaultOpen)) {
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "%s", mr->model_path.c_str());
                        if (ImGui::InputText("model_path", buf, sizeof(buf))) mr->model_path = buf;
                    }
                }
                if (auto* cc = w.get<CameraComponent>(selected_)) {
                    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::DragFloat("fov_y_deg", &cc->fov_y_deg, 0.5f, 10.f, 170.f);
                        ImGui::ColorEdit4("clear_color", &cc->clear_color.x);
                        ImGui::Checkbox("active", &cc->active);
                    }
                }
                if (auto* dl = w.get<DirectionalLightComponent>(selected_)) {
                    if (ImGui::CollapsingHeader("DirectionalLight", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::DragFloat3("direction", &dl->direction.x, 0.01f);
                        ImGui::ColorEdit3("color", &dl->color.x);
                        ImGui::DragFloat("intensity", &dl->intensity, 0.05f, 0.f, 100.f);
                        ImGui::Checkbox("cast_shadow", &dl->cast_shadow);
                    }
                }
                if (auto* sc = w.get<ScriptComponent>(selected_)) {
                    if (ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen)) {
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "%s", sc->path.c_str());
                        if (ImGui::InputText("path", buf, sizeof(buf))) sc->path = buf;
                        ImGui::Checkbox("enabled", &sc->enabled);
                    }
                }
            } else {
                ImGui::TextDisabled("no entity selected (click one in the viewport)");
            }
        }
        ImGui::End();

        // Stats.
        if (ImGui::Begin("Stats")) {
            const RenderStats st = renderer_->stats();
            ImGui::Text("draw calls: %u", st.draw_calls);
            ImGui::Text("triangles:  %u", st.triangles);
            ImGui::Text("frame:      %.2f ms (%.0f fps)", static_cast<double>(dt * 1000.f),
                        static_cast<double>(dt > 0.f ? 1.f / dt : 0.f));
        }
        ImGui::End();

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
