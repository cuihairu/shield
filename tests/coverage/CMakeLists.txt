# Coverage-focused unit tests. One executable per source module under
# src/; each file targets the lines not exercised by the suites in the
# sibling directories. Kept separate so coverage gaps stay visible and
# cheap to iterate on.

# Helper: link a coverage test against the given shield libraries.
#
# Optional first-keyword argument POSIX_ONLY: build and register the suite
# only on non-Windows platforms (boost.asio local stream_protocol /
# socketpair fixtures cannot run there, and compiling every case out would
# leave an empty Boost.Test module, which itself reports failure).
function(shield_coverage_test name)
    set(options POSIX_ONLY)
    cmake_parse_arguments(PARSE_ARGV 1 arg "${options}" "" "")
    if(arg_POSIX_ONLY AND WIN32)
        return()
    endif()
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name}
        PRIVATE
            ${arg_UNPARSED_ARGUMENTS}
            Boost::unit_test_framework
            Boost::headers
    )
    if(SHIELD_ENABLE_CLUSTER)
        # Keep the tests' view of the build shape in sync with the
        # libraries: some suites assert both compile branches of code that
        # is guarded by this define (root.cluster, cluster Lua API).
        target_compile_definitions(${name} PRIVATE SHIELD_ENABLE_CLUSTER)
    endif()
    target_include_directories(${name}
        PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        LABELS "coverage"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    shield_copy_runtime_dlls(${name})
endfunction()

# shield_base / shield_transport
shield_coverage_test(test_cov_base_id shield_base)
shield_coverage_test(test_cov_codec shield_transport)
shield_coverage_test(test_cov_encryption shield_transport)
shield_coverage_test(test_cov_frame shield_transport)
shield_coverage_test(test_cov_protocol shield_transport)

# shield_log / shield_plugin / shield_config
shield_coverage_test(test_cov_logger shield_log)
shield_coverage_test(test_cov_plugin_config shield_plugin shield_config)
shield_coverage_test(test_cov_manifest shield_plugin)
shield_coverage_test(test_cov_schema_validator shield_plugin)
shield_coverage_test(test_cov_plugin_host shield_plugin)
shield_coverage_test(test_cov_config shield_config)

# shield_net
shield_coverage_test(test_cov_session shield_net)
shield_coverage_test(test_cov_listener shield_net)
shield_coverage_test(test_cov_http_server shield_net)
shield_coverage_test(test_cov_http_client shield_net)
shield_coverage_test(test_cov_console_net POSIX_ONLY shield_net shield_bootstrap)

# shield_bootstrap (console commands, starter, bootstrap, shield facade)
shield_coverage_test(test_cov_root_commands POSIX_ONLY shield_bootstrap shield_net shield_plugin)
shield_coverage_test(test_cov_lua_commands POSIX_ONLY shield_bootstrap shield_net shield_plugin)
shield_coverage_test(test_cov_ops_http shield_bootstrap shield_net shield_plugin)
shield_coverage_test(test_cov_starter shield_bootstrap shield_net shield_plugin)
shield_coverage_test(test_cov_bootstrap shield_bootstrap shield_net shield_plugin)
shield_coverage_test(test_cov_bootstrap2 shield_bootstrap shield_net shield_plugin)
shield_coverage_test(test_cov_shield shield_bootstrap shield_net shield_plugin)

# shield_lua
shield_coverage_test(test_cov_cpp_lua_api shield_lua shield_core CAF::core CAF::io)
shield_coverage_test(test_cov_lua_gateway_bridge shield_lua shield_core CAF::core CAF::io)
shield_coverage_test(test_cov_lua_http_bridge shield_lua shield_core shield_net CAF::core CAF::io)
shield_coverage_test(test_cov_lua_runtime shield_lua shield_core CAF::core CAF::io)
shield_coverage_test(test_cov_lua_service shield_lua shield_core CAF::core CAF::io)

# Round-2 coverage suites (same modules, deeper branches).
shield_coverage_test(test_cov_lua_api2 shield_lua shield_core shield_net CAF::core CAF::io)
shield_coverage_test(test_cov_lua_runtime2 shield_lua shield_core CAF::core CAF::io)
shield_coverage_test(test_cov_lua_service2 shield_lua shield_core CAF::core CAF::io)
shield_coverage_test(test_cov_http_bridge2 shield_lua shield_core shield_net CAF::core CAF::io)

# The ops HTTP client drains each response until its read deadline, so the
# suite's wall time is dominated by per-request timeouts; give it headroom.
set_tests_properties(test_cov_ops_http PROPERTIES TIMEOUT 300)
