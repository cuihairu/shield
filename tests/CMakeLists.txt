enable_testing()

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

# 集成测试
add_executable(test_integration integration/test_integration.cpp)
target_link_libraries(
  test_integration
  PRIVATE shield_core Boost::unit_test_framework Boost::beast Boost::headers
          nlohmann_json::nlohmann_json ${OPENSSL_LIBRARIES})
target_include_directories(test_integration PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME test_integration COMMAND test_integration)

# Health 模块测试
add_executable(test_health_check health/test_health_check.cpp)
target_link_libraries(
  test_health_check PRIVATE shield_core Boost::unit_test_framework
                            Boost::headers)
add_test(NAME test_health_check COMMAND test_health_check)

# 文件系统测试
add_executable(test_file_watcher fs/test_file_watcher.cpp)
target_link_libraries(
  test_file_watcher PRIVATE shield_core Boost::unit_test_framework
                            Boost::headers)
add_test(NAME test_file_watcher COMMAND test_file_watcher)

# DI 容器测试
add_executable(test_container di/test_container.cpp)
target_link_libraries(
  test_container PRIVATE shield_core Boost::unit_test_framework Boost::headers)
add_test(NAME test_container COMMAND test_container)

# 网络模块测试
add_executable(test_network net/test_network.cpp)
target_link_libraries(
  test_network PRIVATE shield_core Boost::unit_test_framework Boost::headers)
add_test(NAME test_network COMMAND test_network)

# UDP 服务器测试
add_executable(test_udp_server net/test_udp_server.cpp)
target_link_libraries(
  test_udp_server PRIVATE shield_core Boost::unit_test_framework Boost::headers)
add_test(NAME test_udp_server COMMAND test_udp_server)

# 序列化系统测试
add_executable(test_serialization serialization/test_serialization.cpp)
target_link_libraries(
  test_serialization PRIVATE shield_core Boost::unit_test_framework
                              nlohmann_json::nlohmann_json Boost::headers)
add_test(NAME test_serialization COMMAND test_serialization)

# HTTP 服务器测试
add_executable(test_beast_http_server http/test_beast_http_server.cpp)
target_link_libraries(
  test_beast_http_server PRIVATE shield_core Boost::unit_test_framework
                                  Boost::beast Boost::headers nlohmann_json::nlohmann_json)
add_test(NAME test_beast_http_server COMMAND test_beast_http_server)

# 条件注册系统测试
add_executable(test_conditional_registry conditions/test_conditional_registry.cpp)
target_link_libraries(
  test_conditional_registry PRIVATE shield_core Boost::unit_test_framework
                                      Boost::headers yaml-cpp::yaml-cpp)
add_test(NAME test_conditional_registry COMMAND test_conditional_registry)

# UDP 会话管理测试
add_executable(test_udp_session net/test_udp_session.cpp)
target_link_libraries(
  test_udp_session PRIVATE shield_core Boost::unit_test_framework Boost::headers)
add_test(NAME test_udp_session COMMAND test_udp_session)

# Prometheus 度量系统测试
add_executable(test_prometheus_service metrics/test_prometheus_service.cpp)
target_link_libraries(
  test_prometheus_service PRIVATE shield_core Boost::unit_test_framework
                                    Boost::headers yaml-cpp::yaml-cpp)
add_test(NAME test_prometheus_service COMMAND test_prometheus_service)

# 健康检查工具测试
add_executable(test_health_check_util util/test_health_check.cpp)
target_link_libraries(
  test_health_check_util PRIVATE shield_core Boost::unit_test_framework Boost::headers)
add_test(NAME test_health_check_util COMMAND test_health_check_util)

# 事件系统工具测试
add_executable(test_event_system util/test_event_system.cpp)
target_link_libraries(
  test_event_system PRIVATE shield_core Boost::unit_test_framework Boost::headers)
add_test(NAME test_event_system COMMAND test_event_system)
