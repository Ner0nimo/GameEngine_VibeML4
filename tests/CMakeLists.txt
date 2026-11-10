add_executable(ignis_tests
    src/test_main.cpp
    src/test_log.cpp
    src/test_math.cpp
    src/test_json.cpp
    src/test_path_util.cpp
    src/test_ecs.cpp
    src/test_scene.cpp
    src/test_history.cpp
    src/test_pak.cpp
    src/test_assets.cpp
    src/test_scripting.cpp
    src/test_physics.cpp
    src/test_audio.cpp
    src/test_agent_commands.cpp
    src/test_picking_ibl.cpp
    src/test_render.cpp
    src/test_integration_scene.cpp
    src/test_export.cpp
    src/test_runtime_limits.cpp
)

target_link_libraries(ignis_tests PRIVATE
    ignis_core
    ignis_scripting
    ignis_physics
    ignis_audio
    ignis_renderer
    GTest::gtest
)

if(IGNIS_WITH_GRAPHICS)
    # test_render.cpp drives the real Vulkan renderer; window code needs glfw.
    target_link_libraries(ignis_tests PRIVATE glfw)
endif()

target_compile_definitions(ignis_tests PRIVATE
    IGNIS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    IGNIS_SCENES_DIR="${CMAKE_SOURCE_DIR}/scenes"
    IGNIS_SCRIPTS_DIR="${CMAKE_SOURCE_DIR}/scripts"
)

# Process-level security tests (export verification, runtime limits) drive the
# built executables directly.
if(TARGET ignis_runtime)
    target_compile_definitions(ignis_tests PRIVATE IGNIS_RUNTIME_EXE="$<TARGET_FILE:ignis_runtime>")
endif()
if(TARGET ignis_export)
    target_compile_definitions(ignis_tests PRIVATE IGNIS_EXPORTER_EXE="$<TARGET_FILE:ignis_export>")
endif()

if(IGNIS_FUZZING)
    add_subdirectory(fuzz)
endif()

# gtest headers as SYSTEM includes so our -Werror never fires inside them.
target_include_directories(ignis_tests SYSTEM PRIVATE
    "${googletest_SOURCE_DIR}/googletest/include"
    "${googletest_SOURCE_DIR}/googlemock/include"
)

ignis_apply_warnings(ignis_tests)

include(GoogleTest)
# Tests resolve scene/script/asset paths relative to the repo root.
gtest_discover_tests(ignis_tests WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
