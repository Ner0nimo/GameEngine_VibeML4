add_executable(ignis_runtime src/main.cpp)

target_link_libraries(ignis_runtime PRIVATE
    ignis_core
    ignis_scripting
    ignis_physics
    ignis_audio
    ignis_window
    ignis_renderer
)

if(IGNIS_WITH_GRAPHICS)
    target_link_libraries(ignis_runtime PRIVATE glfw nvrhi ignis_imgui)
    target_compile_definitions(ignis_runtime PRIVATE IGNIS_WITH_GRAPHICS)
endif()

ignis_apply_warnings(ignis_runtime)
