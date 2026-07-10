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

# Header-first protocol routing tests - verifies route-id based forwarding and
# lazy body decode primitives used by protocol profiles.
add_executable(test_protocol_routing transport/test_protocol_routing.cpp)
target_link_libraries(test_protocol_routing
    PRIVATE
        shield_transport
        Boost::unit_test_framework
        Boost::headers
)
target_include_directories(test_protocol_routing
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME test_protocol_routing COMMAND test_protocol_routing)
set_tests_properties(test_protocol_routing PROPERTIES
    LABELS "transport"
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
shield_copy_runtime_dlls(test_protocol_routing)

# TCP listener tests - verifies startup failure surfaces correctly instead of
# allowing bootstrap to report a listener as started.
add_executable(test_tcp_listener net/test_tcp_listener.cpp)
target_link_libraries(test_tcp_listener
    PRIVATE
        shield_net
        Boost::unit_test_framework
        Boost::headers
)
target_include_directories(test_tcp_listener
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME test_tcp_listener COMMAND test_tcp_listener)
set_tests_properties(test_tcp_listener PROPERTIES
    LABELS "net"
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
shield_copy_runtime_dlls(test_tcp_listener)

# Lua API test matrix (LAPI-001 ~ LAPI-010).
add_subdirectory(lua_api)

# ---------------------------------------------------------------------------
# Plugin system v1 tests
# ---------------------------------------------------------------------------

# Manifest (`manifest.yaml`) parsing + validation
add_executable(test_plugin_manifest plugin/test_manifest.cpp)
target_link_libraries(test_plugin_manifest
    PRIVATE shield_plugin nlohmann_json::nlohmann_json
            Boost::unit_test_framework Boost::headers)
target_include_directories(test_plugin_manifest
    PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/plugin)
add_test(NAME test_plugin_manifest COMMAND test_plugin_manifest)
set_tests_properties(test_plugin_manifest PROPERTIES LABELS "plugin" TIMEOUT 10)
shield_copy_runtime_dlls(test_plugin_manifest)

# Schema validator (minimal JSON-Schema subset)
add_executable(test_plugin_schema plugin/test_schema_validator.cpp)
target_link_libraries(test_plugin_schema
    PRIVATE shield_plugin nlohmann_json::nlohmann_json
            Boost::unit_test_framework Boost::headers)
target_include_directories(test_plugin_schema
    PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/plugin)
add_test(NAME test_plugin_schema COMMAND test_plugin_schema)
set_tests_properties(test_plugin_schema PROPERTIES LABELS "plugin" TIMEOUT 10)
shield_copy_runtime_dlls(test_plugin_schema)

# Minimal test plugin module, deployed as a discoverable package for the
# load/create/start pipeline tests.
add_library(shield_minimal_test_plugin MODULE plugin/fixtures/minimal_test_plugin.cpp)
target_include_directories(shield_minimal_test_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
set(_shield_minimal_plugin_output_dir "${CMAKE_BINARY_DIR}/test_plugins/minimal.test/bin")
set_target_properties(shield_minimal_test_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_minimal_test_plugin"
    LIBRARY_OUTPUT_DIRECTORY "${_shield_minimal_plugin_output_dir}"
    RUNTIME_OUTPUT_DIRECTORY "${_shield_minimal_plugin_output_dir}"
    ARCHIVE_OUTPUT_DIRECTORY "${_shield_minimal_plugin_output_dir}")
if(CMAKE_CONFIGURATION_TYPES)
    foreach(_shield_cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${_shield_cfg}" _shield_cfg_upper)
        set_target_properties(shield_minimal_test_plugin PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_${_shield_cfg_upper} "${_shield_minimal_plugin_output_dir}"
            RUNTIME_OUTPUT_DIRECTORY_${_shield_cfg_upper} "${_shield_minimal_plugin_output_dir}"
            ARCHIVE_OUTPUT_DIRECTORY_${_shield_cfg_upper} "${_shield_minimal_plugin_output_dir}")
    endforeach()
endif()
if(WIN32)
    set(_shield_minimal_plugin_filename "libshield_minimal_test_plugin.dll")
elseif(APPLE)
    set(_shield_minimal_plugin_filename "libshield_minimal_test_plugin.dylib")
    set_target_properties(shield_minimal_test_plugin PROPERTIES SUFFIX ".dylib")
else()
    set(_shield_minimal_plugin_filename "libshield_minimal_test_plugin.so")
endif()
configure_file(plugin/fixtures/minimal.manifest.yaml
               "${CMAKE_BINARY_DIR}/test_plugins/minimal.test/manifest.yaml"
               COPYONLY)

# PluginHost pipeline: scan / catalog / plan / resolve / cycle detection
add_executable(test_plugin_host plugin/test_plugin_host.cpp)
target_link_libraries(test_plugin_host
    PRIVATE shield_plugin Boost::unit_test_framework Boost::headers)
target_include_directories(test_plugin_host PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_plugin_host PRIVATE
    SHIELD_TEST_PLUGINS_DIR="${CMAKE_BINARY_DIR}/test_plugins"
    SHIELD_MINIMAL_TEST_PLUGIN_LIBRARY="${_shield_minimal_plugin_filename}")
add_dependencies(test_plugin_host shield_minimal_test_plugin)
add_test(NAME test_plugin_host COMMAND test_plugin_host)
set_tests_properties(test_plugin_host PROPERTIES LABELS "plugin" TIMEOUT 15)
shield_copy_runtime_dlls(test_plugin_host)

if(SHIELD_BUILD_PLUGIN_PROTOBUF)
    find_package(Protobuf CONFIG REQUIRED)
    add_executable(test_protocol_protobuf_plugin
        plugin/test_protocol_protobuf_plugin.cpp
        $<TARGET_OBJECTS:shield_protocol_protobuf_obj>)
    target_link_libraries(test_protocol_protobuf_plugin
        PRIVATE
            protobuf::libprotobuf
            nlohmann_json::nlohmann_json
            Boost::unit_test_framework
            Boost::headers)
    target_include_directories(test_protocol_protobuf_plugin
        PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME test_protocol_protobuf_plugin
             COMMAND test_protocol_protobuf_plugin)
    set_tests_properties(test_protocol_protobuf_plugin PROPERTIES
        LABELS "plugin;protocol"
        TIMEOUT 10)
    shield_copy_runtime_dlls(test_protocol_protobuf_plugin)
endif()

if(SHIELD_BUILD_PLUGIN_MSGPACK)
    add_executable(test_protocol_msgpack_plugin
        plugin/test_protocol_msgpack_plugin.cpp
        $<TARGET_OBJECTS:shield_protocol_msgpack_obj>)
    target_link_libraries(test_protocol_msgpack_plugin
        PRIVATE
            nlohmann_json::nlohmann_json
            Boost::unit_test_framework
            Boost::headers)
    target_include_directories(test_protocol_msgpack_plugin
        PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME test_protocol_msgpack_plugin
             COMMAND test_protocol_msgpack_plugin)
    set_tests_properties(test_protocol_msgpack_plugin PROPERTIES
        LABELS "plugin;protocol"
        TIMEOUT 10)
    shield_copy_runtime_dlls(test_protocol_msgpack_plugin)
endif()
