add_library(ignis_physics STATIC
    src/physics_world.cpp
)
add_library(ignis::physics ALIAS ignis_physics)

target_include_directories(ignis_physics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(ignis_physics PUBLIC
    ignis_core
    reactphysics3d
    lua::lua
)

# rp3d headers as SYSTEM includes so our -Werror never fires inside them.
target_include_directories(ignis_physics SYSTEM PRIVATE "${reactphysics3d_SOURCE_DIR}/include")

ignis_apply_warnings(ignis_physics)
