# Examples CMakeLists.txt

add_executable(hello_world hello_world/main.cpp)
target_link_libraries(hello_world PRIVATE shield_bootstrap)
shield_copy_runtime_dlls(hello_world)
