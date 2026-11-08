set(IGNIS_RENDERER_SOURCES
    src/renderer.cpp
    src/vulkan_renderer.cpp
    src/vulkan_passes.cpp
    src/ibl.cpp
    src/shader_embed.cpp
)

add_library(ignis_renderer STATIC ${IGNIS_RENDERER_SOURCES})
add_library(ignis::renderer ALIAS ignis_renderer)

target_include_directories(ignis_renderer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(ignis_renderer PUBLIC
    ignis_core
    ignis_nvrhi_headers
    ignis_vulkan_headers
    ignis_spirv_headers
    ignis_stb_headers
    ignis_window
)

# --- Shader compilation (glslangValidator) + SPIR-V embedding ----------------
set(IGNIS_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(IGNIS_SPV_DIR "${CMAKE_BINARY_DIR}/shaders-spv")
set(IGNIS_EMBED_HEADER "${CMAKE_BINARY_DIR}/generated/shaders_embedded.h")

find_program(GLSLANG_VALIDATOR NAMES glslangValidator)
file(MAKE_DIRECTORY "${IGNIS_SPV_DIR}" "${CMAKE_BINARY_DIR}/generated")
file(GLOB IGNIS_SHADER_SOURCES "${IGNIS_SHADER_DIR}/*.vert" "${IGNIS_SHADER_DIR}/*.frag")

set(IGNIS_SPV_FILES "")
foreach(SHADER_SRC ${IGNIS_SHADER_SOURCES})
    get_filename_component(BASE "${SHADER_SRC}" NAME_WE)
    get_filename_component(EXT "${SHADER_SRC}" EXT)
    string(SUBSTRING "${EXT}" 1 -1 STAGE) # vert / frag
    set(SPV "${IGNIS_SPV_DIR}/${BASE}_${STAGE}.spv")
    if(GLSLANG_VALIDATOR)
        add_custom_command(
            OUTPUT "${SPV}"
            COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.3
                    -o "${SPV}" "${SHADER_SRC}"
            DEPENDS "${SHADER_SRC}"
            COMMENT "Compiling shader ${BASE}.${STAGE}"
            VERBATIM)
    else()
        # Headless builds without glslang: embed an empty module so the code
        # still compiles; real shaders are produced in graphics/CI builds.
        add_custom_command(
            OUTPUT "${SPV}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${SPV}"
            DEPENDS "${SHADER_SRC}"
            COMMENT "No glslangValidator - empty placeholder for ${BASE}.${STAGE}"
            VERBATIM)
    endif()
    list(APPEND IGNIS_SPV_FILES "${SPV}")
endforeach()

add_custom_command(
    OUTPUT "${IGNIS_EMBED_HEADER}"
    COMMAND "${CMAKE_COMMAND}" "-DSHADER_DIR=${IGNIS_SPV_DIR}" "-DOUT=${IGNIS_EMBED_HEADER}"
            "-P" "${CMAKE_SOURCE_DIR}/cmake/EmbedShaders.cmake"
    DEPENDS ${IGNIS_SPV_FILES}
    COMMENT "Embedding SPIR-V shaders"
    VERBATIM)
add_custom_target(ignis_shaders_embedded DEPENDS "${IGNIS_EMBED_HEADER}")

target_include_directories(ignis_renderer PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(ignis_renderer ignis_shaders_embedded)

if(IGNIS_WITH_GRAPHICS)
    # NVRHI is built as a shared lib (see Dependencies.cmake): it emits and
    # initializes the Vulkan-Hpp dispatcher itself, so no manual dispatch TU.
    target_link_libraries(ignis_renderer PUBLIC nvrhi ignis_vulkan_lib)
    target_compile_definitions(ignis_renderer PUBLIC IGNIS_RENDERER_VULKAN)
endif()

ignis_apply_warnings(ignis_renderer)
