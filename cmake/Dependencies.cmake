# Third-party dependencies, fetched and pinned via FetchContent.
#
# Strategy:
#  - Header-only deps are always available as INTERFACE targets so that all
#    Ignis modules compile everywhere (including headless CI machines).
#  - GLFW / NVRHI / ImGui *libraries* are only built when IGNIS_WITH_GRAPHICS=ON
#    (they need X11/Wayland dev packages and a Vulkan SDK on the host).

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# ---------------------------------------------------------------------------
# Always available (header-only or self-contained builds)
# ---------------------------------------------------------------------------

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.3
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)

# Wrap third-party include dirs as SYSTEM so our strict warning flags never
# fire inside dependency headers.
add_library(ignis_glm INTERFACE)
target_include_directories(ignis_glm SYSTEM INTERFACE "${glm_SOURCE_DIR}")
target_compile_definitions(ignis_glm INTERFACE GLM_ENABLE_EXPERIMENTAL)

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(json)
add_library(ignis_json INTERFACE)
target_include_directories(ignis_json SYSTEM INTERFACE "${json_SOURCE_DIR}/include")

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0
    GIT_SHALLOW TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Lua has no CMake project of its own - build the interpreter core ourselves.
FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG v5.4.8
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(lua)
if(NOT lua_POPULATED)
    FetchContent_Populate(lua)
endif()
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS "${lua_SOURCE_DIR}/*.c")
list(REMOVE_ITEM LUA_SOURCES
    "${lua_SOURCE_DIR}/lua.c"
    "${lua_SOURCE_DIR}/luac.c"
    "${lua_SOURCE_DIR}/onelua.c"
    "${lua_SOURCE_DIR}/ltests.c")
add_library(lua_static STATIC ${LUA_SOURCES})
target_include_directories(lua_static SYSTEM PUBLIC "${lua_SOURCE_DIR}")
set_target_properties(lua_static PROPERTIES PREFIX "lib")
if(UNIX)
    target_link_libraries(lua_static PUBLIC m dl)
endif()
add_library(lua::lua ALIAS lua_static)

# Header-only single-file libraries
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.26
    GIT_SHALLOW TRUE)
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG v1.15
    GIT_SHALLOW TRUE)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 4e9b368bfc071e97206ea5c6adba68dd0cbf8897 # master, pinned
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(miniaudio cgltf stb)

add_library(ignis_miniaudio_headers INTERFACE)
target_include_directories(ignis_miniaudio_headers SYSTEM INTERFACE "${miniaudio_SOURCE_DIR}")

add_library(ignis_cgltf_headers INTERFACE)
target_include_directories(ignis_cgltf_headers SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")

add_library(ignis_stb_headers INTERFACE)
target_include_directories(ignis_stb_headers SYSTEM INTERFACE "${stb_SOURCE_DIR}")

# reactphysics3d - simple, CMake-based 3D physics library
FetchContent_Declare(reactphysics3d
    GIT_REPOSITORY https://github.com/DanielChappuis/reactphysics3d.git
    GIT_TAG v0.10.2
    GIT_SHALLOW TRUE)
set(BUILD_TESTBED OFF CACHE BOOL "" FORCE)
set(BUILD_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(BUILD_TESTBED OFF CACHE BOOL "" FORCE)
set(BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_PYTHON_WRAPPER OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(reactphysics3d)

# ---------------------------------------------------------------------------
# Graphics stack (GLFW + NVRHI/Vulkan + ImGui). Headers are always available
# so window/renderer/editor translation units compile on headless machines;
# the actual libraries are only built with IGNIS_WITH_GRAPHICS=ON.
# ---------------------------------------------------------------------------

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.5.1
    GIT_SHALLOW TRUE)
FetchContent_Declare(nvrhi
    GIT_REPOSITORY https://github.com/NVIDIA-RTX/NVRHI.git
    GIT_TAG bf0bbd9b666a7297492beb1999a4ddc0dfded143
    GIT_SHALLOW TRUE)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.93.0
    GIT_SHALLOW TRUE)
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG vulkan-sdk-1.4.363.0
    GIT_SHALLOW TRUE)
FetchContent_Declare(spirv_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
    GIT_TAG vulkan-sdk-1.4.363.0
    GIT_SHALLOW TRUE)

# These are only *populated* (sources fetched); their CMake projects are NOT
# added here. GLFW/NVRHI libraries are added further below only for graphics
# builds; the rest is consumed header-only.
foreach(dep glfw nvrhi imgui vulkan_headers spirv_headers)
    FetchContent_GetProperties(${dep})
    if(NOT ${dep}_POPULATED)
        FetchContent_Populate(${dep})
    endif()
endforeach()


# Header-only interface targets (always)
add_library(ignis_glfw_headers INTERFACE)
target_include_directories(ignis_glfw_headers SYSTEM INTERFACE "${glfw_SOURCE_DIR}/include")

add_library(ignis_nvrhi_headers INTERFACE)
target_include_directories(ignis_nvrhi_headers SYSTEM INTERFACE "${nvrhi_SOURCE_DIR}/include")

add_library(ignis_vulkan_headers INTERFACE)
target_include_directories(ignis_vulkan_headers SYSTEM INTERFACE
    "${vulkan_headers_SOURCE_DIR}/include")

add_library(ignis_spirv_headers INTERFACE)
target_include_directories(ignis_spirv_headers SYSTEM INTERFACE
    "${spirv_headers_SOURCE_DIR}/include")

# ImGui core (always compilable; backends only with graphics)
set(IMGUI_SOURCES
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
add_library(ignis_imgui STATIC ${IMGUI_SOURCES})
target_include_directories(ignis_imgui SYSTEM PUBLIC
    "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
# Third-party code: no ignis_apply_warnings on purpose.

if(IGNIS_WITH_GRAPHICS)
    # Vulkan/SPIRV header packages as real CMake targets (NVRHI expects
    # Vulkan::Headers / SPIRV-Headers targets).
    add_subdirectory("${vulkan_headers_SOURCE_DIR}" "${vulkan_headers_BINARY_DIR}" EXCLUDE_FROM_ALL)
    add_subdirectory("${spirv_headers_SOURCE_DIR}" "${spirv_headers_BINARY_DIR}" EXCLUDE_FROM_ALL)

    # Real GLFW library (needs X11/Wayland dev packages on Linux)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${glfw_SOURCE_DIR}" "${glfw_BINARY_DIR}" EXCLUDE_FROM_ALL)

    # libvulkan for our raw bootstrap code (renderer window/swapchain path).
    find_library(VULKAN_LIBRARY NAMES vulkan REQUIRED)
    add_library(ignis_vulkan_lib INTERFACE)
    target_link_libraries(ignis_vulkan_lib INTERFACE "${VULKAN_LIBRARY}")

    # Real NVRHI library (Vulkan backend only; loads libvulkan at runtime)
    set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
    set(NVRHI_WITH_OPENGL OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_METAL OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DEBUG_LAYER OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_NVRHI OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_TRACY OFF CACHE BOOL "" FORCE)
    set(NVRHI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(NVRHI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NVRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NVRHI_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    add_subdirectory("${nvrhi_SOURCE_DIR}" "${nvrhi_BINARY_DIR}" EXCLUDE_FROM_ALL)
    # Ship NVRHI without its internal asserts (Release-style); our own targets
    # keep full warnings-as-errors.
    target_compile_definitions(nvrhi PRIVATE NDEBUG)
    if(TARGET nvrhi_vk)
        target_compile_definitions(nvrhi_vk PRIVATE NDEBUG)
    endif()

    # ImGui backends for GLFW + raw Vulkan (used by the editor overlay)
    target_sources(ignis_imgui PRIVATE
        "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
    target_link_libraries(ignis_imgui PUBLIC glfw ignis_vulkan_headers)
    target_compile_definitions(ignis_imgui PUBLIC
        IMGUI_IMPL_VULKAN_NO_PROTOTYPES GLFW_INCLUDE_NONE)
endif()
