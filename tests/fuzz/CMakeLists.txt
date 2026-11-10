# libFuzzer targets, built only with IGNIS_FUZZING=ON (Clang + sanitizers,
# see the "fuzz" CMake preset). Kept separate from the GoogleTest suite.

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "IGNIS_FUZZING requires Clang (libFuzzer). Use the 'fuzz' preset.")
endif()

foreach(target fuzz_pak fuzz_scene)
    add_executable(${target} ${target}.cpp)
    target_link_libraries(${target} PRIVATE ignis_core)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
    ignis_apply_warnings(${target})
endforeach()
