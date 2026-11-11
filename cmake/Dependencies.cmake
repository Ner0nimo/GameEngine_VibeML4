# Third-party dependencies, fetched and pinned via FetchContent.
#
# Strategy:
#  - Header-only deps are always available as INTERFACE targets so that all
#    Ignis modules compile everywhere (including headless CI machines).
#  - GLFW / NVRHI / ImGui *libraries* are only built when IGNIS_WITH_GRAPHICS=ON
#    (they need X11/Wayland dev packages and a Vulkan SDK on the host).
#
# DECLARATION FORMAT (enforced by the dep-pin-audit job in
# .github/workflows/security.yml): every FetchContent_Declare must use
# literal values on their own lines —
#     GIT_REPOSITORY https://github.com/<org>/<repo>.git
#     GIT_TAG <40-hex commit SHA>
# No variables, no moving tags, no branch names, no plain http.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# ---------------------------------------------------------------------------
# Always available (header-only or self-contained builds)
# ---------------------------------------------------------------------------

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 8d1fd52e5ab5590e2c81768ace50c72bae28f2ed # was tag 1.0.3
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)

# Wrap third-party include dirs as SYSTEM so our strict warning flags never
# fire inside dependency headers.
add_library(ignis_glm INTERFACE)
target_include_directories(ignis_glm SYSTEM INTERFACE "${glm_SOURCE_DIR}")
target_compile_definitions(ignis_glm INTERFACE GLM_ENABLE_EXPERIMENTAL)

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 65ee68451d8eb2b5f3a30b410476ab83deb3289b # was tag v3.12.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(json)
add_library(ignis_json INTERFACE)
target_include_directories(ignis_json SYSTEM INTERFACE "${json_SOURCE_DIR}/include")

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 063de7e9578f82b369302001269680b4b1553359 # was tag v1.18.0
    GIT_SHALLOW TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Lua has no CMake project of its own - build the interpreter core ourselves.
FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG 6e22fedb74cf0c9b6656e9fce8b7331db847c605 # was tag v5.4.8
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
    GIT_TAG 877ea4a7c2601c7b0f2a44e83dfac42e6470fea6 # was tag 0.11.26
    GIT_SHALLOW TRUE)
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG bbeb5b0b070ddacddac6852fb72143eb68454937 # was tag v1.15
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
    GIT_TAG cd958bbc0c6e84a869388cba6613f10cc645b3cb # was tag v0.10.2
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
    GIT_TAG 70a9bb3881fe80fd483236e2b203cb451c6ecf40 # was tag 3.5.1
    GIT_SHALLOW TRUE)
FetchContent_Declare(nvrhi
    GIT_REPOSITORY https://github.com/NVIDIA-RTX/NVRHI.git
    GIT_TAG bf0bbd9b666a7297492beb1999a4ddc0dfded143
    GIT_SHALLOW TRUE)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    # docking branch head, 1.93.1 WIP, master-merged; mainline v1.93.0 has no
    # DockSpace/DockBuilder API, needed for dockable panels, issue #56.
    # Re-pin to a tagged 1.93.1+ docking release once available.
    GIT_TAG cf53467ab0db7a783f04b32dd9160e6aa5b720ef
    GIT_SHALLOW TRUE)
FetchContent_Declare(imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG 105e4fde45e4e74aae8973d98750820ef7aca7e2
    GIT_SHALLOW FALSE) # small repo; pin the exact commit
FetchContent_Declare(vulkan_headers_ignis
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG 6802bb4733b63ed5efd3adb308a6c885ef180ea1 # was tag vulkan-sdk-1.4.363.0 (Vulkan-Headers)
    GIT_SHALLOW TRUE)
FetchContent_Declare(spirv_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
    GIT_TAG 496543121ce6419f23d6fa5d7194ba66c36212d2 # was tag vulkan-sdk-1.4.363.0 (SPIRV-Headers)
    GIT_SHALLOW TRUE)

# These are only *populated* (sources fetched); their CMake projects are NOT
# added here. GLFW/NVRHI libraries are added further below only for graphics
# builds; the rest is consumed header-only.
foreach(dep glfw nvrhi imgui vulkan_headers_ignis spirv_headers)
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
    "${vulkan_headers_ignis_SOURCE_DIR}/include")

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

# ImGuizmo (viewport transform gizmo for the editor), pinned commit. The repo
# ships no CMakeLists, so we only populate it (not MakeAvailable) and build
# the single TU ourselves below.
cmake_policy(PUSH)
cmake_policy(SET CMP0169 OLD)
FetchContent_Populate(imguizmo)
cmake_policy(POP)
add_library(ignis_imguizmo STATIC "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
target_include_directories(ignis_imguizmo SYSTEM PUBLIC "${imguizmo_SOURCE_DIR}/src")
target_link_libraries(ignis_imguizmo PUBLIC ignis_imgui)

if(IGNIS_WITH_GRAPHICS)
    # NVRHI fetches its own pinned Vulkan-Headers (its shared-build dispatch init
    # needs the header version it was written against). We only provide the
    # SPIRV-Headers target; our own code uses vulkan headers via the
    # ignis_vulkan_headers interface target (source include dir).
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
    set(NVRHI_FETCH_VULKAN_HEADERS ON CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VALIDATION OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_OPENGL OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_METAL OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DEBUG_LAYER OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_NVRHI OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_TRACY OFF CACHE BOOL "" FORCE)
    set(NVRHI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(NVRHI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NVRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    # Shared build: NVRHI then emits and initializes the Vulkan-Hpp dynamic
    # dispatcher inside its own .so (vulkan-device.cpp), avoiding static-link
    # dispatcher/ODR pitfalls on Linux.
    set(NVRHI_BUILD_SHARED ON CACHE BOOL "" FORCE)
    set(NVRHI_BUILD_SHARED_LIBRARY ON CACHE BOOL "" FORCE)
    add_subdirectory("${nvrhi_SOURCE_DIR}" "${nvrhi_BINARY_DIR}" EXCLUDE_FROM_ALL)
    # Ship NVRHI without its internal asserts (Release-style); our own targets
    # keep full warnings-as-errors.
    target_compile_definitions(nvrhi PRIVATE NDEBUG)

    # ImGui backends for GLFW + raw Vulkan (used by the editor overlay)
    target_sources(ignis_imgui PRIVATE
        "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
    target_link_libraries(ignis_imgui PUBLIC glfw ignis_vulkan_headers)
    target_compile_definitions(ignis_imgui PUBLIC
        IMGUI_IMPL_VULKAN_NO_PROTOTYPES GLFW_INCLUDE_NONE)
endif()
