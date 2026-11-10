set(IGNIS_CORE_SOURCES
    src/core/log.cpp
    src/core/pak.cpp
    src/ecs/world.cpp
    src/scene/scene.cpp
    src/scene/history.cpp
    src/assets/asset_manager.cpp
    src/assets/gltf_loader.cpp
    src/assets/texture_loader.cpp
    src/assets/cgltf_impl.cpp
    src/assets/stb_impl.cpp
    src/agent/commands.cpp
)

add_library(ignis_core STATIC ${IGNIS_CORE_SOURCES})
add_library(ignis::core ALIAS ignis_core)

target_include_directories(ignis_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # internal headers shared between engine TUs
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(ignis_core PUBLIC
    ignis_glm
    ignis_json
    ignis_cgltf_headers
    ignis_stb_headers
)

# Third-party single-header implementations must not be compiled with our
# strict warning flags.
set_source_files_properties(
    src/assets/cgltf_impl.cpp
    src/assets/stb_impl.cpp
    PROPERTIES COMPILE_OPTIONS "-w"
)

ignis_apply_warnings(ignis_core)
