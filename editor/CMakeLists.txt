add_executable(ignis_editor
    src/main.cpp
    src/editor_app.cpp
)
target_include_directories(ignis_editor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(ignis_editor PRIVATE
    ignis_core
    ignis_scripting
    ignis_physics
    ignis_audio
    ignis_window
    ignis_renderer
    ignis_imgui
    ignis_imguizmo
    glfw
    nvrhi
)
target_compile_definitions(ignis_editor PRIVATE IGNIS_WITH_GRAPHICS IGNIS_WINDOW_VULKAN)
ignis_apply_warnings(ignis_editor)
