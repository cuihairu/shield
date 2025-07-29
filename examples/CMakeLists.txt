# Examples CMakeLists.txt

# Reactor example (disabled due to missing reactor.hpp)
# add_executable(reactor_example reactor_example.cpp)
# target_link_libraries(reactor_example PRIVATE shield_core)

# Distributed actor demo (disabled due to CAF configuration issues)
# add_executable(distributed_actor_demo distributed_actor_demo.cpp)
# target_link_libraries(distributed_actor_demo PRIVATE shield_core)

# Set output directory for examples
# set_target_properties(distributed_actor_demo
#     PROPERTIES
#     RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
# )