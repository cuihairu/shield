enable_testing()

add_executable(test_logger log/test_logger.cpp)
target_link_libraries(test_logger PRIVATE shield_core Boost::unit_test_framework Boost::log Boost::log_setup Boost::headers)
add_test(NAME test_logger COMMAND test_logger)

add_executable(test_config config/test_config.cpp)
target_link_libraries(test_config PRIVATE shield_core Boost::unit_test_framework Boost::filesystem yaml-cpp::yaml-cpp Boost::headers)
add_test(NAME test_config COMMAND test_config)

add_executable(test_lua_vm_pool script/test_lua_vm_pool.cpp)
target_link_libraries(test_lua_vm_pool PRIVATE shield_core Boost::unit_test_framework Boost::filesystem yaml-cpp::yaml-cpp Boost::headers)
add_test(NAME test_lua_vm_pool COMMAND test_lua_vm_pool)

add_executable(test_lua_engine script/test_lua_engine.cpp)
target_link_libraries(test_lua_engine PRIVATE shield_core Boost::unit_test_framework Boost::filesystem yaml-cpp::yaml-cpp Boost::headers)
add_test(NAME test_lua_engine COMMAND test_lua_engine)

add_executable(test_lua_actor actor/test_lua_actor.cpp)
target_link_libraries(test_lua_actor PRIVATE shield_core Boost::unit_test_framework Boost::filesystem yaml-cpp::yaml-cpp Boost::headers)
add_test(NAME test_lua_actor COMMAND test_lua_actor)

add_executable(test_simple_lua_actor actor/test_simple_lua_actor.cpp)
target_link_libraries(test_simple_lua_actor PRIVATE shield_core Boost::unit_test_framework Boost::filesystem yaml-cpp::yaml-cpp Boost::headers)
add_test(NAME test_simple_lua_actor COMMAND test_simple_lua_actor)

# 集成测试
add_executable(test_integration integration/test_integration.cpp)
target_link_libraries(test_integration PRIVATE 
    shield_core 
    Boost::unit_test_framework 
    Boost::beast 
    Boost::headers
    nlohmann_json::nlohmann_json
    ${OPENSSL_LIBRARIES}
)
target_include_directories(test_integration PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME test_integration COMMAND test_integration)