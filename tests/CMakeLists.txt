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

# Lua API test matrix (LAPI-001 ~ LAPI-010).
add_subdirectory(lua_api)

# ---------------------------------------------------------------------------
# Plugin system v1 tests
# ---------------------------------------------------------------------------

# Manifest (plugin.json) parsing + validation
add_executable(test_plugin_manifest plugin/test_manifest.cpp)
target_link_libraries(test_plugin_manifest
    PRIVATE shield_plugin nlohmann_json::nlohmann_json
            Boost::unit_test_framework Boost::headers)
target_include_directories(test_plugin_manifest
    PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/plugin)
add_test(NAME test_plugin_manifest COMMAND test_plugin_manifest)
set_tests_properties(test_plugin_manifest PROPERTIES LABELS "plugin" TIMEOUT 10)

# Schema validator (minimal JSON-Schema subset)
add_executable(test_plugin_schema plugin/test_schema_validator.cpp)
target_link_libraries(test_plugin_schema
    PRIVATE shield_plugin nlohmann_json::nlohmann_json
            Boost::unit_test_framework Boost::headers)
target_include_directories(test_plugin_schema
    PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/plugin)
add_test(NAME test_plugin_schema COMMAND test_plugin_schema)
set_tests_properties(test_plugin_schema PROPERTIES LABELS "plugin" TIMEOUT 10)

# Minimal test plugin module, deployed as a discoverable package for the
# load/create/start pipeline tests.
add_library(shield_minimal_test_plugin MODULE plugin/fixtures/minimal_test_plugin.cpp)
target_include_directories(shield_minimal_test_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(shield_minimal_test_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_minimal_test_plugin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test_plugins/minimal.test/bin")
file(COPY plugin/fixtures/minimal.plugin.json
     DESTINATION "${CMAKE_BINARY_DIR}/test_plugins/minimal.test")

# PluginHost pipeline: scan / catalog / plan / resolve / cycle detection
add_executable(test_plugin_host plugin/test_plugin_host.cpp)
target_link_libraries(test_plugin_host
    PRIVATE shield_plugin Boost::unit_test_framework Boost::headers)
target_include_directories(test_plugin_host PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME test_plugin_host COMMAND test_plugin_host)
set_tests_properties(test_plugin_host PROPERTIES LABELS "plugin" TIMEOUT 15)
shield_copy_runtime_dlls(test_plugin_host)
