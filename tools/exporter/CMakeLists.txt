add_executable(ignis_export src/main.cpp)
target_link_libraries(ignis_export PRIVATE ignis_core)
ignis_apply_warnings(ignis_export)
