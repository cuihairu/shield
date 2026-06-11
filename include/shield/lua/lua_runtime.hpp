// [SHIELD_LUA] Lua Runtime
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace shield::lua {

// Opaque handle to a Lua VM state
class LuaVM;

/// @brief Lua runtime manager
/// Manages a pool of Lua VMs and provides API registration
class LuaRuntime {
public:
    LuaRuntime();
    ~LuaRuntime();

    // Non-copyable
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    // Create a new Lua VM
    std::shared_ptr<LuaVM> create_vm();

    // Load a script into a VM
    /// @param vm The VM to load into
    /// @param script_path Path to the Lua script
    /// @return true if successful
    bool load_script(std::shared_ptr<LuaVM> vm, std::string_view script_path);

    // Call a function in a VM
    /// @param vm The VM to call in
    /// @param func_name Function name
    /// @param args Arguments (as JSON string)
    /// @return Result as JSON string
    std::string call_function(std::shared_ptr<LuaVM> vm,
                             std::string_view func_name,
                             std::string_view args = "{}");

    // Register API functions
    void register_api(std::shared_ptr<LuaVM> vm);

    // Get global variable
    std::string get_global(std::shared_ptr<LuaVM> vm,
                          std::string_view name);

    // Set global variable
    void set_global(std::shared_ptr<LuaVM> vm,
                   std::string_view name,
                   std::string_view value);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief Per-VM context for a Lua service
class ServiceContext {
public:
    std::string service_id;
    std::string service_name;
    std::string module_path;

    // Current message context (only valid during handler execution)
    std::string sender_id;
    std::string trace_id;
    int64_t deadline_ms = 0;
};

}  // namespace shield::lua
