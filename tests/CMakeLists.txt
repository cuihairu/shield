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

# Removed: test_lua_vm_pool (script module deleted)
# Removed: test_lua_engine (script module deleted)
# Removed: test_lua_actor (actor module deleted)
# Removed: test_simple_lua_actor (actor module deleted)

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

# Removed: test_serialization (serialization module deleted)

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

# Removed: test_local_discovery (discovery module deleted)

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

# --- New coverage tests ---
# Removed: test_middleware (gateway module deleted)
# Removed: test_runtime_diagnostics (gateway module deleted)
# Removed: test_gateway_config (gateway module deleted)

add_executable(test_application_context core/test_application_context.cpp)
target_link_libraries(
  test_application_context PRIVATE shield_core Boost::unit_test_framework
                                   Boost::headers)
add_test(NAME test_application_context COMMAND test_application_context)

add_executable(test_starter_manager core/test_starter_manager.cpp)
target_link_libraries(
  test_starter_manager PRIVATE shield_core Boost::unit_test_framework
                               Boost::headers)
add_test(NAME test_starter_manager COMMAND test_starter_manager)

# Removed: test_service_instance (discovery module deleted)
# Removed: test_discovery_config (discovery module deleted)

add_executable(test_dynamic_config config/test_dynamic_config.cpp)
target_link_libraries(
  test_dynamic_config PRIVATE shield_core Boost::unit_test_framework
                              Boost::headers)
add_test(NAME test_dynamic_config COMMAND test_dynamic_config)

add_executable(test_network_config net/test_network_config.cpp)
target_link_libraries(
  test_network_config PRIVATE shield_core Boost::unit_test_framework
                              Boost::headers)
add_test(NAME test_network_config COMMAND test_network_config)

add_executable(test_log_config log/test_log_config.cpp)
target_link_libraries(
  test_log_config PRIVATE shield_core Boost::unit_test_framework Boost::log
                          Boost::log_setup Boost::headers)
add_test(NAME test_log_config COMMAND test_log_config)

add_executable(test_protocol_handler protocol/test_protocol_handler.cpp)
target_link_libraries(
  test_protocol_handler PRIVATE shield_core Boost::unit_test_framework
                                Boost::headers)
add_test(NAME test_protocol_handler COMMAND test_protocol_handler)

# Removed: test_actor_system_config (actor module deleted)

add_executable(test_prometheus_config metrics/test_prometheus_config.cpp)
target_link_libraries(
  test_prometheus_config PRIVATE shield_core Boost::unit_test_framework
                                 Boost::headers)
add_test(NAME test_prometheus_config COMMAND test_prometheus_config)

# Removed: test_lua_vm_pool_config_properties (script module deleted)
# Removed: test_discovery_service_factory (discovery module deleted)

# --- More coverage tests ---

# Removed: test_service_handle (service module deleted)
# Removed: test_universal_serialization_system (serialization module deleted)

add_executable(test_data_access_framework
               data/test_data_access_framework.cpp)
target_link_libraries(
  test_data_access_framework
  PRIVATE shield_core Boost::unit_test_framework Boost::headers)
add_test(NAME test_data_access_framework COMMAND test_data_access_framework)

add_executable(test_plugin_manager core/test_plugin_manager.cpp)
target_link_libraries(
  test_plugin_manager PRIVATE shield_core Boost::unit_test_framework
                              Boost::headers)
add_test(NAME test_plugin_manager COMMAND test_plugin_manager)

add_executable(test_config_reload_binder core/test_config_reload_binder.cpp)
target_link_libraries(
  test_config_reload_binder
  PRIVATE shield_core Boost::unit_test_framework Boost::headers)
add_test(NAME test_config_reload_binder COMMAND test_config_reload_binder)

# Removed: test_service_context (service module deleted)

add_executable(test_command_context cli/test_command_context.cpp)
target_link_libraries(
  test_command_context PRIVATE shield_core Boost::unit_test_framework
                               Boost::headers)
add_test(NAME test_command_context COMMAND test_command_context)

add_executable(test_command_class cli/test_command_class.cpp)
target_link_libraries(
  test_command_class PRIVATE shield_core Boost::unit_test_framework
                             Boost::headers)
add_test(NAME test_command_class COMMAND test_command_class)

# Removed: test_messagepack_serializer (serialization module deleted)

add_executable(test_config_registry config/test_config_registry.cpp)
target_link_libraries(
  test_config_registry PRIVATE shield_core Boost::unit_test_framework
                               Boost::headers yaml-cpp::yaml-cpp)
add_test(NAME test_config_registry COMMAND test_config_registry)

# Removed: test_actor_registry (actor module deleted)

add_executable(test_command_line_parser cli/test_command_line_parser.cpp)
target_link_libraries(
  test_command_line_parser PRIVATE shield_core Boost::unit_test_framework
                                   Boost::headers)
add_test(NAME test_command_line_parser COMMAND test_command_line_parser)

add_executable(test_query_cache data/test_query_cache.cpp)
target_link_libraries(
  test_query_cache PRIVATE shield_core Boost::unit_test_framework
                           Boost::headers)
add_test(NAME test_query_cache COMMAND test_query_cache)

add_executable(test_plugin_host core/test_plugin_host.cpp)
target_link_libraries(
  test_plugin_host PRIVATE shield_core Boost::unit_test_framework
                           Boost::headers)
add_test(NAME test_plugin_host COMMAND test_plugin_host)

# Removed: test_commands (commands module deleted)

add_executable(test_command_base cli/test_command_base.cpp)
target_link_libraries(
  test_command_base PRIVATE shield_core Boost::unit_test_framework
                            Boost::headers)
add_test(NAME test_command_base COMMAND test_command_base)

add_executable(test_root_command cli/test_root_command.cpp)
target_link_libraries(
  test_root_command PRIVATE shield_core Boost::unit_test_framework
                            Boost::headers)
add_test(NAME test_root_command COMMAND test_root_command)

add_executable(test_protocol_handler_factory
               protocol/test_protocol_handler_factory.cpp)
target_link_libraries(
  test_protocol_handler_factory PRIVATE shield_core
                                        Boost::unit_test_framework Boost::headers)
add_test(NAME test_protocol_handler_factory
         COMMAND test_protocol_handler_factory)

add_executable(test_websocket_handler protocol/test_websocket_handler.cpp)
target_link_libraries(
  test_websocket_handler PRIVATE shield_core Boost::unit_test_framework
                                 Boost::headers OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME test_websocket_handler COMMAND test_websocket_handler)

add_executable(test_advanced_container di/test_advanced_container.cpp)
target_link_libraries(
  test_advanced_container PRIVATE shield_core Boost::unit_test_framework
                                  Boost::headers)
add_test(NAME test_advanced_container COMMAND test_advanced_container)

# Removed: test_server_command (commands module deleted)
# Removed: test_actor_system_coordinator (actor module deleted)
# Removed: test_database_actor_service (database module deleted)

add_executable(test_beast_http_server_config
               http/test_beast_http_server_config.cpp)
target_link_libraries(
  test_beast_http_server_config
  PRIVATE shield_core Boost::unit_test_framework Boost::headers)
add_test(NAME test_beast_http_server_config
         COMMAND test_beast_http_server_config)

# Removed: test_distributed_actor_config (actor module deleted)

add_executable(test_service_container di/test_service_container.cpp)
target_link_libraries(
  test_service_container PRIVATE shield_core Boost::unit_test_framework
                                 Boost::headers)
add_test(NAME test_service_container COMMAND test_service_container)

# Lua API Tests
add_subdirectory(lua_api)
