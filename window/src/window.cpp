#include "ignis/window/window.h"

#include "ignis/core/log.h"

#if defined(IGNIS_WINDOW_VULKAN)
#include <vulkan/vulkan.h> // must come before glfw3.h so glfwCreateWindowSurface is declared
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace ignis {

Window::Window(WindowProps props) : props_(std::move(props)) {}

Window::~Window() {
    shutdown();
}

Result<void> Window::init() {
    if (window_) return Result<void>::success();
    if (!glfwInit())
        return Result<void>::failure("glfwInit failed (no display / missing X11 or Wayland)");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, props_.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, props_.visible ? GLFW_TRUE : GLFW_FALSE);

    window_ = glfwCreateWindow(static_cast<int>(props_.width), static_cast<int>(props_.height),
                               props_.title.c_str(), nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        return Result<void>::failure("glfwCreateWindow failed");
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, key_trampoline);
    glfwSetCursorPosCallback(window_, cursor_trampoline);
    glfwSetMouseButtonCallback(window_, mouse_button_trampoline);
    glfwSetScrollCallback(window_, scroll_trampoline);
    glfwSetFramebufferSizeCallback(window_, resize_trampoline);
    glfwSetWindowSizeCallback(window_, window_size_trampoline);
    glfwSetWindowContentScaleCallback(window_, content_scale_trampoline);

    time_ = glfwGetTime();
    last_time_ = time_;
    IGNIS_INFO("Window created: '{}' {}x{}", props_.title, props_.width, props_.height);
    return Result<void>::success();
}

void Window::shutdown() {
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    glfwTerminate();
}

void Window::poll_events() {
    time_ = glfwGetTime();
    dt_ = static_cast<f32>(time_ - last_time_);
    last_time_ = time_;
    glfwPollEvents();
}

bool Window::should_close() const {
    return window_ ? glfwWindowShouldClose(window_) != 0 : true;
}

void Window::request_close() {
    if (window_) glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

FramebufferSize Window::framebuffer_size() const {
    FramebufferSize out;
    int fw = 0, fh = 0;
    if (window_) glfwGetFramebufferSize(window_, &fw, &fh);
    out.width = static_cast<u32>(fw);
    out.height = static_cast<u32>(fh);
    return out;
}

WindowSize Window::window_size() const {
    WindowSize out;
    int w = 0, h = 0;
    if (window_) glfwGetWindowSize(window_, &w, &h);
    out.width = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    return out;
}

ContentScale Window::content_scale() const {
    ContentScale out;
    if (window_) glfwGetWindowContentScale(window_, &out.x, &out.y);
    return out;
}

#if defined(IGNIS_WINDOW_VULKAN)
Result<void*> Window::create_vulkan_surface(void* instance) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult res = glfwCreateWindowSurface(static_cast<VkInstance>(instance), window_,
                                           nullptr, &surface);
    if (res != VK_SUCCESS)
        return Result<void*>::failure("glfwCreateWindowSurface failed: " +
                                       std::to_string(static_cast<int>(res)));
    return Result<void*>::success(reinterpret_cast<void*>(surface));
}
#endif

// --- trampolines -------------------------------------------------------------

void Window::key_trampoline(GLFWwindow* w, int key, int scancode, int action, int mods) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (self && self->on_key) self->on_key(key, scancode, action, mods);
}

void Window::cursor_trampoline(GLFWwindow* w, double x, double y) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (self && self->on_cursor_pos) self->on_cursor_pos(x, y);
}

void Window::mouse_button_trampoline(GLFWwindow* w, int button, int action, int mods) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (self && self->on_mouse_button) self->on_mouse_button(button, action, mods);
}

void Window::scroll_trampoline(GLFWwindow* w, double x, double y) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (self && self->on_scroll) self->on_scroll(x, y);
}

void Window::resize_trampoline(GLFWwindow* win, int width, int height) {
    // Framebuffer size in pixels - this is what the renderer cares about.
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
    if (self && self->on_resize)
        self->on_resize(static_cast<u32>(width), static_cast<u32>(height));
}

void Window::window_size_trampoline(GLFWwindow* win, int width, int height) {
    // Logical window size in screen coordinates.
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
    if (self) {
        self->props_.width = static_cast<u32>(width);
        self->props_.height = static_cast<u32>(height);
    }
}

void Window::content_scale_trampoline(GLFWwindow* win, float x, float y) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
    if (self && self->on_content_scale_changed) self->on_content_scale_changed(x, y);
}

} // namespace ignis
