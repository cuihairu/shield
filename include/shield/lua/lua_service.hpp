// [SHIELD_LUA] Lua service interface
#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace shield::lua {

class LuaRuntime;
class ServiceContext;

/// @brief Result of spawning a Lua service
struct SpawnResult {
    bool success;
    std::string service_id;
    std::string error_message;

    static SpawnResult ok(std::string id) {
        return {true, std::move(id), ""};
    }

    static SpawnResult error(std::string msg) {
        return {false, "", std::move(msg)};
    }
};

/// @brief Lua service manager
/// Handles loading and spawning Lua services
class LuaServiceManager {
public:
    explicit LuaServiceManager(LuaRuntime& runtime);
    ~LuaServiceManager();

    // Spawn a service from a Lua module
    /// @param module Module name (e.g., "services.player")
    /// @param opts Optional spawn options (name, args, timeout)
    SpawnResult spawn(std::string_view module,
                     std::string_view opts_json = "{}");

    // Send a message to a service
    void send(std::string_view target, std::string_view method,
             std::string_view payload);

    // Call a service and wait for response
    /// @return Response payload or error
    std::string call(std::string_view target, std::string_view method,
                    std::string_view payload, int32_t timeout_ms = 5000);

    // Exit a service
    void exit(std::string_view service_id, std::string_view reason = "normal");

    // Get current service ID
    std::string current_service_id() const;

    // List registered services
    std::vector<std::string> list_services() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace shield::lua
