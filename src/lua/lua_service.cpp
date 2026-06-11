// [SHIELD_LUA] Lua service implementation
#include "shield/lua/lua_service.hpp"
#include "shield/lua/lua_runtime.hpp"

#include "shield/base/error.hpp"
#include "shield/base/result.hpp"

#include <nlohmann/json.hpp>

namespace shield::lua {

struct LuaServiceManager::Impl {
    LuaRuntime& runtime;

    Impl(LuaRuntime& rt) : runtime(rt) {}
};

LuaServiceManager::LuaServiceManager(LuaRuntime& runtime)
    : impl_(std::make_unique<Impl>(runtime)) {}

LuaServiceManager::~LuaServiceManager() = default;

SpawnResult LuaServiceManager::spawn(std::string_view module,
                                     std::string_view opts_json) {
    try {
        // Parse options
        nlohmann::json opts = nlohmann::json::parse(opts_json);

        std::string service_name = opts.value("name", "");
        std::string args = opts.value("args", nlohmann::json::object()).dump();

        // Generate service ID if no name provided
        if (service_name.empty()) {
            service_name = module;
            service_name += ":";
            service_name += std::to_string(std::hash<std::string_view>{}(module));
        }

        // Create VM and load module
        auto vm = impl_->runtime.create_vm();
        impl_->runtime.register_api(vm);

        if (!impl_->runtime.load_script(vm, module)) {
            return SpawnResult::error("Failed to load module: " + std::string(module));
        }

        // Call on_init if exists
        auto init_result = impl_->runtime.call_function(vm, "on_init",
            R"({"name": ")" + service_name + R"(", "args": )" + args + "}");

        // Check if init succeeded
        // (Real implementation would check for return value or errors)

        return SpawnResult::ok(service_name);

    } catch (const std::exception& e) {
        return SpawnResult::error(std::string("Spawn failed: ") + e.what());
    }
}

void LuaServiceManager::send(std::string_view target,
                            std::string_view method,
                            std::string_view payload) {
    // Send message to target service
    // Real implementation would route through service registry
}

std::string LuaServiceManager::call(std::string_view target,
                                   std::string_view method,
                                   std::string_view payload,
                                   int32_t timeout_ms) {
    // Call service and wait for response
    // Real implementation would:
    // 1. Find target service
    // 2. Send message with response promise
    // 3. Wait for response or timeout
    return R"({"ok": true, "result": null})";
}

void LuaServiceManager::exit(std::string_view service_id,
                            std::string_view reason) {
    // Exit a service
    // Real implementation would:
    // 1. Call on_exit(reason)
    // 2. Unregister service
    // 3. Clean up VM
}

std::string LuaServiceManager::current_service_id() const {
    // Get current service ID from TLS
    return "";
}

std::vector<std::string> LuaServiceManager::list_services() const {
    // List all registered services
    return {};
}

}  // namespace shield::lua
