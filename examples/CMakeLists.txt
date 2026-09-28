# Examples CMakeLists.txt

add_executable(hello_world hello_world/main.cpp)
target_link_libraries(hello_world PRIVATE shield_bootstrap)
shield_copy_runtime_dlls(hello_world)

# One-shot experience: Lua business script + sqlite + health_http +
# metric_prometheus. See examples/kickstart/README.md for the 3-command run.
add_executable(kickstart kickstart/main.cpp)
target_link_libraries(kickstart PRIVATE shield_bootstrap)
shield_copy_runtime_dlls(kickstart)
