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
)

# Lua API test matrix (LAPI-001 ~ LAPI-010).
add_subdirectory(lua_api)
