add_library(ignis_window STATIC
    src/window.cpp
)
add_library(ignis::window ALIAS ignis_window)

target_include_directories(ignis_window PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(ignis_window PUBLIC
    ignis_core
    ignis_glfw_headers
)

# The Vulkan surface helper needs vulkan.h and is only compiled in graphics builds.
if(IGNIS_WITH_GRAPHICS)
    target_link_libraries(ignis_window PUBLIC ignis_vulkan_headers)
    target_compile_definitions(ignis_window PUBLIC IGNIS_WINDOW_VULKAN)
endif()

ignis_apply_warnings(ignis_window)
