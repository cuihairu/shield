// Test file for the upgraded LuaEngine
#include "shield/script/lua_engine.hpp"
#include "shield/core/logger.hpp"
#include <iostream>
#include <cassert>

namespace shield::script {

void test_lua_engine() {
    // Initialize logging first
    shield::core::LogConfig log_config;
    shield::core::Logger::init(log_config);
    
    std::cout << "=== Testing upgraded LuaEngine with sol2 ===" << std::endl;
    
    // Create and initialize LuaEngine
    LuaEngine engine("test_engine");
    engine.init();
    
    engine.start(); // Add start() call
    
    // Test 1: Basic Lua execution
    std::cout << "\nTest 1: Basic Lua execution" << std::endl;
    bool result = engine.execute_string("print('Hello from Lua!')");
    assert(result && "Basic Lua execution failed");
    std::cout << "✅ Basic execution: PASSED" << std::endl;
    
    // Test 2: Set and get global variables
    std::cout << "\nTest 2: Global variables" << std::endl;
    engine.set_global("test_number", 42);
    engine.set_global("test_string", std::string("Hello World"));
    engine.set_global("test_bool", true);
    
    auto num = engine.get_global<int>("test_number");
    auto str = engine.get_global<std::string>("test_string");
    auto boolean = engine.get_global<bool>("test_bool");
    
    assert(num && *num == 42);
    assert(str && *str == "Hello World");
    assert(boolean && *boolean == true);
    std::cout << "✅ Global variables: PASSED" << std::endl;
    
    // Test 3: Register C++ function
    std::cout << "\nTest 3: C++ function registration" << std::endl;
    engine.register_function("cpp_add", [](int a, int b) {
        return a + b;
    });
    
    engine.execute_string("result = cpp_add(10, 20)");
    auto cpp_result = engine.get_global<int>("result");
    assert(cpp_result && *cpp_result == 30);
    std::cout << "✅ C++ function registration: PASSED" << std::endl;
    
    // Test 4: Call Lua function from C++
    std::cout << "\nTest 4: Call Lua function from C++" << std::endl;
    engine.execute_string(R"(
        function lua_multiply(x, y)
            return x * y
        end
    )");
    
    auto lua_result = engine.call_function<int>("lua_multiply", 5, 6);
    assert(lua_result && *lua_result == 30);
    std::cout << "✅ Lua function call: PASSED" << std::endl;
    
    // Test 5: Error handling
    std::cout << "\nTest 5: Error handling" << std::endl;
    bool error_result = engine.execute_string("invalid_lua_syntax [");
    assert(!error_result && "Error handling failed");
    std::cout << "✅ Error handling: PASSED" << std::endl;
    
    engine.stop();
    std::cout << "\n🎉 All tests passed! LuaEngine with sol2 is working correctly." << std::endl;
}

} // namespace shield::script

int main() {
    try {
        shield::script::test_lua_engine();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}