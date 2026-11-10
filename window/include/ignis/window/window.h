#pragma once

#include "ignis/core/result.h"
#include "ignis/core/types.h"

#include <functional>
#include <string>

struct GLFWwindow;

namespace ignis {

struct WindowProps {
    std::string title = "Ignis";
    u32 width = 1280;
    u32 height = 720;
    bool resizable = true;
    bool visible = true;
};

struct FramebufferSize {
    u32 width = 0;
    u32 height = 0;
};

// Thin GLFW wrapper. The window is created with GLFW_NO_API (Vulkan); the
// renderer owns the swapchain.
class Window {
public:
    explicit Window(WindowProps props);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    Result<void> init();
    void shutdown();
    bool is_initialized() const { return window_ != nullptr; }

    void poll_events();
    bool should_close() const;
    void request_close();

    GLFWwindow* handle() const { return window_; }
    u32 width() const { return props_.width; }
    u32 height() const { return props_.height; }
    FramebufferSize framebuffer_size() const;
    f64 time() const { return time_; }
    f32 delta_time() const { return dt_; }

    // Event callbacks (set after init).
    std::function<void(int key, int scancode, int action, int mods)> on_key;
    std::function<void(f64 x, f64 y)> on_cursor_pos;
    std::function<void(int button, int action, int mods)> on_mouse_button;
    std::function<void(f64 x, f64 y)> on_scroll;
    std::function<void(u32 w, u32 h)> on_resize;

#if defined(IGNIS_WINDOW_VULKAN)
    // Creates a VkSurfaceKHR for this window. `instance` is a VkInstance.
    // Returned as void* to keep vulkan.h out of this header.
    Result<void*> create_vulkan_surface(void* instance);
#endif

private:
    static void key_trampoline(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void cursor_trampoline(GLFWwindow* w, double x, double y);
    static void mouse_button_trampoline(GLFWwindow* w, int button, int action, int mods);
    static void scroll_trampoline(GLFWwindow* w, double x, double y);
    static void resize_trampoline(GLFWwindow* win, int width, int height);

    WindowProps props_;
    GLFWwindow* window_ = nullptr;
    f64 time_ = 0.0;
    f64 last_time_ = 0.0;
    f32 dt_ = 0.f;
};

} // namespace ignis
