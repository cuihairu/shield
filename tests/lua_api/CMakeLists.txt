# Lua API Tests
cmake_minimum_required(VERSION 3.20)

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
    )
endfunction()

# Active suites: aligned with the current public Lua API surface in
# include/shield/lua/lua_*.hpp.
shield_lua_api_test(test_lua_api_lifecycle)
shield_lua_api_test(test_lua_api_timers)

# Deferred suites: these files were committed against a speculative API
# (`LuaServiceManager::spawn_service`, `send_message`, `query_service`
# returning raw pointers, `LuaRuntime::eval_int64`, etc.) that does not
# exist in the current implementation. They are tracked as Phase 4 work in
# docs/roadmap.md ("LAPI-001~010 已在 `tests/lua_api/` 覆盖正负向 case"
# needs to be reconciled with the actual API). Re-enable one suite at a
# time as it is rewritten to call spawn() / send() / call() and inspect
# state through call_service_method instead of returning raw VM pointers.
#
# shield_lua_api_test(test_lua_api_registry)
# shield_lua_api_test(test_lua_api_messaging)
# shield_lua_api_test(test_lua_api_call)
# shield_lua_api_test(test_lua_api_context)
# shield_lua_api_test(test_lua_api_legacy)
# shield_lua_api_test(test_lua_api_data)
# shield_lua_api_test(test_lua_api_gateway)
