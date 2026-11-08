add_library(ignis_scripting STATIC
    src/script_engine.cpp
)
add_library(ignis::scripting ALIAS ignis_scripting)

target_include_directories(ignis_scripting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(ignis_scripting PUBLIC
    ignis_core
    lua::lua
)

ignis_apply_warnings(ignis_scripting)
