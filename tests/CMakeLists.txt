enable_testing()

# Phase 1 refactor verification entry point.
# Only tests aligned with the active public headers are registered here.
# Legacy tests for deleted modules (annotations, conditions, di, health,
# metrics, plugin, discovery, gateway middleware, etc.) have been removed.

# Hello World acceptance test - verifies the example app builds and the
# documented Lua scripts/config exist on disk.
add_executable(test_hello_world_acceptance hello_world_acceptance.cpp)
target_link_libraries(test_hello_world_acceptance
    PRIVATE
        shield_bootstrap
        shield_lua
        Boost::unit_test_framework
        Boost::headers
)
target_include_directories(test_hello_world_acceptance
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME test_hello_world_acceptance COMMAND test_hello_world_acceptance)
set_tests_properties(test_hello_world_acceptance PROPERTIES
    LABELS "acceptance"
    TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
shield_copy_runtime_dlls(test_hello_world_acceptance)

# Transport frame tests - verifies length-prefixed framing boundaries used by
# TCP sessions and gateway listeners.
add_executable(test_transport_frame transport/test_transport_frame.cpp)
target_link_libraries(test_transport_frame
    PRIVATE
        shield_transport
        Boost::unit_test_framework
        Boost::headers
)
target_include_directories(test_transport_frame
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME test_transport_frame COMMAND test_transport_frame)
set_tests_properties(test_transport_frame PROPERTIES
    LABELS "transport"
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
shield_copy_runtime_dlls(test_transport_frame)

# Data pool tests - verifies configured pool sizing, acquisition timeout, and
# explicit mock behaviour independent of Lua bindings.
add_executable(test_data_pool data/test_data_pool.cpp)
target_link_libraries(test_data_pool
    PRIVATE
        shield_data
        yaml-cpp::yaml-cpp
        Boost::unit_test_framework
        Boost::headers
)
target_include_directories(test_data_pool
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME test_data_pool COMMAND test_data_pool)
set_tests_properties(test_data_pool PROPERTIES
    LABELS "data"
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
shield_copy_runtime_dlls(test_data_pool)

# Lua API test matrix (LAPI-001 ~ LAPI-010).
add_subdirectory(lua_api)
