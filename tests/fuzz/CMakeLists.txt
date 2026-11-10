# libFuzzer targets, built only with IGNIS_FUZZING=ON (Clang + sanitizers,
# see the "fuzz" CMake preset). Kept separate from the GoogleTest suite.
# One target per untrusted-input parser: pak, scene JSON, raw JSON, glTF,
# images, audio, agent command batches.

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "IGNIS_FUZZING requires Clang (libFuzzer). Use the 'fuzz' preset.")
endif()

function(ignis_fuzz_target name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ARGN})
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    ignis_apply_warnings(${name})
endfunction()

ignis_fuzz_target(fuzz_pak ignis_core)
ignis_fuzz_target(fuzz_scene ignis_core)
ignis_fuzz_target(fuzz_json ignis_core)
ignis_fuzz_target(fuzz_commands ignis_core)
ignis_fuzz_target(fuzz_texture ignis_core)
ignis_fuzz_target(fuzz_gltf ignis_core)
ignis_fuzz_target(fuzz_audio ignis_audio)

# fuzz_texture/fuzz_gltf use the internal texture_decode.h from engine/src.
target_include_directories(fuzz_texture PRIVATE "${CMAKE_SOURCE_DIR}/engine/src")
target_include_directories(fuzz_gltf PRIVATE "${CMAKE_SOURCE_DIR}/engine/src")
