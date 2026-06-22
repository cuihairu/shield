# Lua API Tests
cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

find_package(Boost REQUIRED COMPONENTS unit_test_framework)

# Common linkage recipe. Every LAPI executable below uses the same set of
# dependencies; we keep the recipe in a function so the disabled-test
# documentation below stays readable.
function(shield_lua_api_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name}
        PRIVATE
            shield_lua
            shield_core
            Boost::unit_test_framework
            Boost::headers
    )
    target_include_directories(${name}
        PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests/lua_api
    )
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        LABELS "lua_api"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    shield_copy_runtime_dlls(${name})
endfunction()

# Active suites: aligned with the current public Lua API surface in
# include/shield/lua/lua_*.hpp.
shield_lua_api_test(test_lua_api_lifecycle)
shield_lua_api_test(test_lua_api_timers)
shield_lua_api_test(test_lua_api_registry)
shield_lua_api_test(test_lua_api_messaging)
shield_lua_api_test(test_lua_api_call)
shield_lua_api_test(test_lua_api_context)
shield_lua_api_test(test_lua_api_legacy)
shield_lua_api_test(test_lua_api_config)
shield_lua_api_test(test_lua_api_gateway)
