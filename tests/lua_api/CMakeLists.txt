# Lua API Tests
cmake_minimum_required(VERSION 3.20)

# Find required packages
find_package(Boost REQUIRED COMPONENTS unit_test_framework)

# Create the test harness library (if needed)
# For now, tests are standalone

# Test executable: Lifecycle Tests (LAPI-001, LAPI-002)
add_executable(test_lua_api_lifecycle
    test_lua_api_lifecycle.cpp
)

target_link_libraries(test_lua_api_lifecycle
    PRIVATE
    shield_lua
    shield_core
    Boost::unit_test_framework
    Boost::headers
)

target_include_directories(test_lua_api_lifecycle
    PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/lua_api
)

# Add test
add_test(NAME test_lua_api_lifecycle COMMAND test_lua_api_lifecycle)

# Set test properties
set_tests_properties(test_lua_api_lifecycle PROPERTIES
    LABELS "lua_api"
    TIMEOUT 60
)

# Test executable: Registry Tests (LAPI-003)
add_executable(test_lua_api_registry
    test_lua_api_registry.cpp
)

target_link_libraries(test_lua_api_registry
    PRIVATE
    shield_lua
    shield_core
    Boost::unit_test_framework
    Boost::headers
)

target_include_directories(test_lua_api_registry
    PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/lua_api
)

add_test(NAME test_lua_api_registry COMMAND test_lua_api_registry)

set_tests_properties(test_lua_api_registry PROPERTIES
    LABELS "lua_api"
    TIMEOUT 60
)

# Test executable: Messaging Tests (LAPI-004, LAPI-005)
add_executable(test_lua_api_messaging
    test_lua_api_messaging.cpp
)

target_link_libraries(test_lua_api_messaging
    PRIVATE
    shield_lua
    shield_core
    Boost::unit_test_framework
    Boost::headers
)

target_include_directories(test_lua_api_messaging
    PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/lua_api
)

add_test(NAME test_lua_api_messaging COMMAND test_lua_api_messaging)

set_tests_properties(test_lua_api_messaging PROPERTIES
    LABELS "lua_api"
    TIMEOUT 60
)

# Test executable: Call Tests (LAPI-005)
add_executable(test_lua_api_call
    test_lua_api_call.cpp
)

target_link_libraries(test_lua_api_call
    PRIVATE
    shield_lua
    shield_core
    Boost::unit_test_framework
    Boost::headers
)

target_include_directories(test_lua_api_call
    PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/lua_api
)

add_test(NAME test_lua_api_call COMMAND test_lua_api_call)

set_tests_properties(test_lua_api_call PROPERTIES
    LABELS "lua_api"
    TIMEOUT 60
)

# Test executable: Context Tests (LAPI-006)
add_executable(test_lua_api_context
    test_lua_api_context.cpp
)

target_link_libraries(test_lua_api_context
    PRIVATE
    shield_lua
    shield_core
    Boost::unit_test_framework
    Boost::headers
)

target_include_directories(test_lua_api_context
    PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/lua_api
)

add_test(NAME test_lua_api_context COMMAND test_lua_api_context)

set_tests_properties(test_lua_api_context PROPERTIES
    LABELS "lua_api"
    TIMEOUT 60
)

# Test executable: Legacy API Tests (LAPI-010)
add_executable(test_lua_api_legacy
    test_lua_api_legacy.cpp
)

target_link_libraries(test_lua_api_legacy
    PRIVATE
    shield_lua
    shield_core
    Boost::unit_test_framework
    Boost::headers
)

target_include_directories(test_lua_api_legacy
    PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/lua_api
)

add_test(NAME test_lua_api_legacy COMMAND test_lua_api_legacy)

set_tests_properties(test_lua_api_legacy PROPERTIES
    LABELS "lua_api"
    TIMEOUT 60
)
