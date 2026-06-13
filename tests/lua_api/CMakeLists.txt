# Lua API Tests
cmake_minimum_required(VERSION 3.20)

# Find required packages
find_package(Boost REQUIRED COMPONENTS unit_test_framework)

# Create the test harness library (if needed)
# For now, tests are standalone

# Test executable
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
