// Lua API Test Harness
#pragma once

#include <string>
#include <memory>
#include <functional>

namespace shield::testing {

/// @brief Result of a Lua service test
struct LuaTestResult {
    bool success = false;
    std::string error_message;
    std::string log_output;
};

/// @brief Test harness for Lua API testing
class LuaApiTestHarness {
public:
    LuaApiTestHarness();
    ~LuaApiTestHarness();

    // Initialize the test harness
    bool initialize(std::string* error = nullptr);

    // Load a test service from a Lua script
    bool load_service(const std::string& service_name,
                     const std::string& script_path,
                     std::string* error = nullptr);

    // Unload a service
    void unload_service(const std::string& service_name);

    // Call a method on a service
    LuaTestResult call_service(const std::string& service_name,
                               const std::string& method,
                               const std::string& args_json = "{}");

    // Send a message to a service
    LuaTestResult send_message(const std::string& service_name,
                               const std::string& method,
                               const std::string& args_json = "{}");

    // Get captured log output
    std::string get_log_output() const;

    // Clear log output
    void clear_log_output();

    // Get service count
    size_t service_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace shield::testing
