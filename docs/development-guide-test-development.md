# 测试开发

#### 单元测试
```cpp
#define BOOST_TEST_MODULE MyModuleTest
#include <boost/test/unit_test.hpp>
#include "shield/your_module.hpp"

BOOST_AUTO_TEST_CASE(test_your_function) {
    // 测试代码
    BOOST_CHECK_EQUAL(your_function(1, 2), 3);
}
```

#### 集成测试
```cpp
BOOST_FIXTURE_TEST_SUITE(IntegrationTests, TestFixture)

BOOST_AUTO_TEST_CASE(test_http_api) {
    // 启动测试服务器
    // 发送 HTTP 请求
    // 验证响应
}

BOOST_AUTO_TEST_SUITE_END()
```