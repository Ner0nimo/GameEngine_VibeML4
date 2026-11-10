cmake_minimum_required(VERSION 3.27)

project(Ignis
    VERSION 0.1.0
    DESCRIPTION "Ignis - production-grade 3D game engine (Vulkan/NVRHI, ECS, Lua scripting)"
    LANGUAGES C CXX
)

# CMake 4.x removed compatibility with cmake_minimum_required(< 3.5) used by
# some third-party projects. Allow them to configure.
set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(IGNIS_BUILD_TESTS    "Build unit tests and the integration test scene" ON)
option(IGNIS_WITH_GRAPHICS  "Build window/renderer/editor against real GPU deps (GLFW windowing, NVRHI/Vulkan)" ON)
option(IGNIS_BUILD_EDITOR   "Build the editor executable (requires IGNIS_WITH_GRAPHICS)" ON)
option(IGNIS_BUILD_EXPORTER "Build the ignis-export CLI tool" ON)
option(IGNIS_FUZZING "Build libFuzzer targets for the pak/scene parsers (requires Clang + sanitizers)" OFF)

include(cmake/CompilerSettings.cmake)
include(cmake/Dependencies.cmake)

add_subdirectory(engine)
add_subdirectory(scripting)
add_subdirectory(physics)
add_subdirectory(audio)
add_subdirectory(window)
add_subdirectory(renderer)
add_subdirectory(runtime)

if(IGNIS_BUILD_EDITOR AND IGNIS_WITH_GRAPHICS)
    add_subdirectory(editor)
endif()

if(IGNIS_BUILD_EXPORTER)
    add_subdirectory(tools/exporter)
endif()

if(IGNIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
