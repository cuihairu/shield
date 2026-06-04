enable_testing()

# --- Core tests ---

add_executable(test_logger log/test_logger.cpp)
target_link_libraries(
  test_logger PRIVATE shield_core Boost::unit_test_framework Boost::log
                      Boost::log_setup Boost::headers)
add_test(NAME test_logger COMMAND test_logger)

add_executable(test_config config/test_config.cpp)
target_link_libraries(
  test_config PRIVATE shield_core Boost::unit_test_framework Boost::filesystem
                      yaml-cpp::yaml-cpp Boost::headers)
add_test(NAME test_config COMMAND test_config)

add_executable(test_ptree_yaml config/test_ptree_yaml.cpp)
target_link_libraries(
  test_ptree_yaml PRIVATE shield_core Boost::unit_test_framework
                          yaml-cpp::yaml-cpp Boost::headers)
add_test(NAME test_ptree_yaml COMMAND test_ptree_yaml)

add_executable(test_lua_vm_pool script/test_lua_vm_pool.cpp)
target_link_libraries(
  test_lua_vm_pool PRIVATE shield_core Boost::unit_test_framework
                           Boost::filesystem yaml-cpp::yaml-cpp Boost::headers)
add_test(NAME test_lua_vm_pool COMMAND test_lua_vm_pool)

add_executable(test_lua_engine script/test_lua_engine.cpp)
target_link_libraries(
  test_lua_engine PRIVATE shield_core Boost::unit_test_framework
                          Boost::filesystem yaml-cpp::yaml-cpp Boost::headers)
add_test(NAME test_lua_engine COMMAND test_lua_engine)

add_executable(test_lua_actor actor/test_lua_actor.cpp)
target_link_libraries(
  test_lua_actor PRIVATE shield_core Boost::unit_test_framework
                         Boost::filesystem yaml-cpp::yaml-cpp Boost::headers)
add_test(NAME test_lua_actor COMMAND test_lua_actor)

add_executable(test_simple_lua_actor actor/test_simple_lua_actor.cpp)
target_link_libraries(
  test_simple_lua_actor
  PRIVATE shield_core Boost::unit_test_framework Boost::filesystem
          yaml-cpp::yaml-cpp Boost::headers)
add_test(NAME test_simple_lua_actor COMMAND test_simple_lua_actor)

add_executable(test_integration integration/test_integration.cpp)
target_link_libraries(
  test_integration
  PRIVATE shield_core shield_extensions Boost::unit_test_framework Boost::beast Boost::headers
          nlohmann_json::nlohmann_json ${OPENSSL_LIBRARIES})
target_include_directories(test_integration PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME test_integration COMMAND test_integration)

add_executable(test_file_watcher fs/test_file_watcher.cpp)
target_link_libraries(
  test_file_watcher PRIVATE shield_core Boost::unit_test_framework
                            Boost::headers)
add_test(NAME test_file_watcher COMMAND test_file_watcher)

add_executable(test_network net/test_network.cpp)
target_link_libraries(
  test_network PRIVATE shield_core Boost::unit_test_framework Boost::headers)
add_test(NAME test_network COMMAND test_network)

add_executable(test_udp_server net/test_udp_server.cpp)
target_link_libraries(
  test_udp_server PRIVATE shield_core Boost::unit_test_framework Boost::headers)
add_test(NAME test_udp_server COMMAND test_udp_server)

add_executable(test_serialization serialization/test_serialization.cpp)
target_link_libraries(
  test_serialization PRIVATE shield_core Boost::unit_test_framework
                              nlohmann_json::nlohmann_json Boost::headers)
add_test(NAME test_serialization COMMAND test_serialization)

add_executable(test_beast_http_server http/test_beast_http_server.cpp)
target_link_libraries(
  test_beast_http_server PRIVATE shield_core Boost::unit_test_framework
                                  Boost::beast Boost::headers nlohmann_json::nlohmann_json)
add_test(NAME test_beast_http_server COMMAND test_beast_http_server)

add_executable(test_udp_session net/test_udp_session.cpp)
target_link_libraries(
  test_udp_session PRIVATE shield_core Boost::unit_test_framework Boost::headers)
add_test(NAME test_udp_session COMMAND test_udp_session)

add_executable(test_command_execution unit/cli/test_command_execution.cpp)
target_link_libraries(
  test_command_execution PRIVATE shield_core Boost::unit_test_framework
                                 Boost::headers)
add_test(NAME test_command_execution COMMAND test_command_execution)

add_executable(test_cli_command_parsing cli/test_command_parsing.cpp)
target_link_libraries(
  test_cli_command_parsing PRIVATE shield_core Boost::unit_test_framework
                                   Boost::headers)
add_test(NAME test_cli_command_parsing COMMAND test_cli_command_parsing)

# --- Extension tests (link against shield_extensions) ---

add_executable(test_health_check health/test_health_check.cpp)
target_link_libraries(
  test_health_check PRIVATE shield_core shield_extensions
                            Boost::unit_test_framework Boost::headers)
add_test(NAME test_health_check COMMAND test_health_check)

add_executable(test_container di/test_container.cpp)
target_link_libraries(
  test_container PRIVATE shield_core shield_extensions
                         Boost::unit_test_framework Boost::headers)
add_test(NAME test_container COMMAND test_container)

add_executable(test_conditional_registry conditions/test_conditional_registry.cpp)
target_link_libraries(
  test_conditional_registry PRIVATE shield_core shield_extensions
                                      Boost::unit_test_framework
                                      Boost::headers yaml-cpp::yaml-cpp)
add_test(NAME test_conditional_registry COMMAND test_conditional_registry)

add_executable(test_component_registry annotations/test_component_registry.cpp)
target_link_libraries(
  test_component_registry PRIVATE shield_core shield_extensions
                                  Boost::unit_test_framework
                                  Boost::headers yaml-cpp::yaml-cpp)
add_test(NAME test_component_registry COMMAND test_component_registry)

add_executable(test_prometheus_service metrics/test_prometheus_service.cpp)
target_link_libraries(
  test_prometheus_service PRIVATE shield_core shield_extensions
                                    Boost::unit_test_framework
                                    Boost::headers yaml-cpp::yaml-cpp)
add_test(NAME test_prometheus_service COMMAND test_prometheus_service)

add_executable(test_health_check_util util/test_health_check.cpp)
target_link_libraries(
  test_health_check_util PRIVATE shield_core shield_extensions
                                 Boost::unit_test_framework Boost::headers)
add_test(NAME test_health_check_util COMMAND test_health_check_util)

add_executable(test_event_system util/test_event_system.cpp)
target_link_libraries(
  test_event_system PRIVATE shield_core shield_extensions
                            Boost::unit_test_framework Boost::headers)
add_test(NAME test_event_system COMMAND test_event_system)

# --- Previously unregistered tests ---

add_executable(test_binary_protocol unit/protocol/test_binary_protocol.cpp)
target_link_libraries(
  test_binary_protocol PRIVATE shield_core Boost::unit_test_framework
                               Boost::headers)
add_test(NAME test_binary_protocol COMMAND test_binary_protocol)

add_executable(test_http_handler unit/protocol/test_http_handler.cpp)
target_link_libraries(
  test_http_handler PRIVATE shield_core Boost::unit_test_framework Boost::beast
                            Boost::headers)
add_test(NAME test_http_handler COMMAND test_http_handler)

add_executable(test_local_discovery discovery/test_local_discovery.cpp)
target_link_libraries(
  test_local_discovery PRIVATE shield_core Boost::unit_test_framework
                               Boost::headers)
add_test(NAME test_local_discovery COMMAND test_local_discovery)

add_executable(test_config_constants unit/config/test_config_constants.cpp)
target_link_libraries(
  test_config_constants PRIVATE shield_core Boost::unit_test_framework
                                Boost::headers)
add_test(NAME test_config_constants COMMAND test_config_constants)

add_executable(test_full_system e2e/test_full_system.cpp)
target_link_libraries(
  test_full_system PRIVATE shield_core Boost::unit_test_framework Boost::beast
                           Boost::headers nlohmann_json::nlohmann_json)
add_test(NAME test_full_system COMMAND test_full_system)

add_executable(test_gateway_component
               integration/components/test_gateway_component.cpp)
target_link_libraries(
  test_gateway_component
  PRIVATE shield_core shield_extensions Boost::unit_test_framework Boost::headers
          yaml-cpp::yaml-cpp)
add_test(NAME test_gateway_component COMMAND test_gateway_component)
