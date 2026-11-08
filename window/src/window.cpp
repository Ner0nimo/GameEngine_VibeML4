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

bool Window::init(std::string& err) {
    if (window_) return true;
    if (!glfwInit()) {
        err = "glfwInit failed (no display / missing X11 or Wayland)";
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, props_.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, props_.visible ? GLFW_TRUE : GLFW_FALSE);

    window_ = glfwCreateWindow(static_cast<int>(props_.width), static_cast<int>(props_.height),
                               props_.title.c_str(), nullptr, nullptr);
    if (!window_) {
        err = "glfwCreateWindow failed";
        glfwTerminate();
        return false;
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, key_trampoline);
    glfwSetCursorPosCallback(window_, cursor_trampoline);
    glfwSetMouseButtonCallback(window_, mouse_button_trampoline);
    glfwSetScrollCallback(window_, scroll_trampoline);
    glfwSetFramebufferSizeCallback(window_, resize_trampoline);

    time_ = glfwGetTime();
    last_time_ = time_;
    IGNIS_INFO("Window created: '{}' {}x{}", props_.title, props_.width, props_.height);
    return true;
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

void Window::framebuffer_size(u32& w, u32& h) const {
    int fw = 0, fh = 0;
    if (window_) glfwGetFramebufferSize(window_, &fw, &fh);
    w = static_cast<u32>(fw);
    h = static_cast<u32>(fh);
}

#if defined(IGNIS_WINDOW_VULKAN)
void* Window::create_vulkan_surface(void* instance, std::string& err) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult res = glfwCreateWindowSurface(static_cast<VkInstance>(instance), window_,
                                           nullptr, &surface);
    if (res != VK_SUCCESS) {
        err = "glfwCreateWindowSurface failed: " + std::to_string(static_cast<int>(res));
        return nullptr;
    }
    return reinterpret_cast<void*>(surface);
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
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
    if (self) {
        self->props_.width = static_cast<u32>(width);
        self->props_.height = static_cast<u32>(height);
        if (self->on_resize) self->on_resize(static_cast<u32>(width), static_cast<u32>(height));
    }
}

} // namespace ignis
