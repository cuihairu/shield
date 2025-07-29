# 集成测试CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

# 查找Google Test
find_package(GTest REQUIRED)
find_package(Boost REQUIRED COMPONENTS beast)

# 创建集成测试目录
add_executable(integration_tests
    test_integration.cpp
)

# 链接库
target_link_libraries(integration_tests
    PRIVATE
    shield_core
    GTest::GTest
    GTest::Main
    ${Boost_LIBRARIES}
    nlohmann_json::nlohmann_json
)

# 包含头文件
target_include_directories(integration_tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
)

# 添加测试
add_test(NAME IntegrationTests COMMAND integration_tests)

# 设置测试属性
set_tests_properties(IntegrationTests PROPERTIES
    TIMEOUT 60
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)