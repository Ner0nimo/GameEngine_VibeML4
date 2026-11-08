add_library(ignis_audio STATIC
    src/audio_engine.cpp
)
add_library(ignis::audio ALIAS ignis_audio)

target_include_directories(ignis_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(ignis_audio PUBLIC
    ignis_core
    ignis_miniaudio_headers
    lua::lua
)

ignis_apply_warnings(ignis_audio)
