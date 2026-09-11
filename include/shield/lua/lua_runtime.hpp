// [SHIELD_LUA] Lua Runtime
#pragma once

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <sol/sol.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace shield::lua {

// Opaque handle to a Lua VM state
class LuaVM;
class LuaServiceManager;

/// @brief Opaque ServiceHandle userdata for Lua
/// This prevents direct string manipulation of service IDs
class ServiceHandle {
public:
    explicit ServiceHandle(std::string service_id)
        : service_id_(std::move(service_id)) {}

    const std::string& id() const { return service_id_; }

    // Node this handle routes to. Single-node runtime: always 0 (local).
    // Cluster routing will carry a real NodeId once remote services exist.
    uint32_t node() const { return 0; }

    // Check if handle is valid
    bool valid() const { return !service_id_.empty(); }

    // Lua userdata operations
    static void register_usertype(sol::state& lua);

private:
    std::string service_id_;
};

/// @brief LuaPack binary encoder for message serialization
class LuaPackEncoder {
public:
    // Type tags as per LuaPack specification
    enum class TypeTag : uint8_t {
        Nil = 0x00,
        False = 0x01,
        True = 0x02,
        Integer = 0x03,
        Number = 0x04,
        ShortString = 0x05,
        String = 0x06,
        Array = 0x07,
        Map = 0x08,
        ServiceHandle = 0x10,
        Extension = 0xFF,
    };

    static constexpr uint8_t VERSION = 1;
    static constexpr uint8_t MAGIC_HIGH = 0x4C;  // 'L'
    static constexpr uint8_t MAGIC_LOW = 0x50;   // 'P'

    // Configuration
    struct Config {
        size_t max_nesting_depth = 64;
        size_t max_string_length = 1048576;  // 1MB
        size_t max_array_length = 1000000;
        size_t max_map_entries = 100000;
    };

    explicit LuaPackEncoder(const Config& config);

    // Encode a Lua value to LuaPack format
    /// @param lua Lua state
    /// @param value Value to encode
    /// @param out_bytes Output buffer
    /// @return true if encoding succeeded
    bool encode(sol::state_view lua, const sol::object& value,
                std::vector<uint8_t>& out_bytes);

    // Get last error message
    std::string error() const { return error_; }

private:
    bool encode_value(sol::state_view lua, const sol::object& value,
                      std::vector<uint8_t>& out, size_t depth);

    Config config_;
    std::string error_;
};

/// @brief LuaPack binary decoder for message deserialization
class LuaPackDecoder {
public:
    explicit LuaPackDecoder();

    // Decode LuaPack bytes to Lua value
    /// @param lua Lua state
    /// @param bytes Input bytes
    /// @param out_bytes_consumed Number of bytes consumed
    /// @return Decoded Lua value or nil on error
    sol::object decode(sol::state_view lua, const std::vector<uint8_t>& bytes,
                       size_t& out_bytes_consumed);

    // Get last error message
    std::string error() const { return error_; }

private:
    sol::object decode_value(sol::state_view lua, const uint8_t* data,
                             size_t size, size_t& out_consumed);

    std::string error_;
};

/// @brief Lua script cache configuration
struct LuaCacheConfig {
    bool enabled = true;
    size_t max_size = 100;
    int64_t ttl_seconds = 0;  // 0 = never expire
};

/// @brief An inbound HTTP route registered from Lua via shield.httpd.*.
///
/// The handler is owned by the registering service's VM and must only be
/// invoked on that service's dispatch thread (the LuaServiceManager forked
/// task path guarantees this).
struct HttpRouteRegistration {
    std::string service_id;   // Registering service (dispatch target)
    std::string method;       // "GET", "POST", ...
    std::string path;         // Route pattern; ":name" matches a segment
    std::weak_ptr<LuaVM> vm;  // Handler's VM (liveness guard)
    std::shared_ptr<sol::function> handler;  // Bound to that VM
};

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

    // sol state of a VM created by create_vm. Lets the service manager
    // publish generated per-VM helpers (e.g. s2c client_rpc closures)
    // without depending on the LuaVM definition.
    sol::state& vm_state(const std::shared_ptr<LuaVM>& vm);

    // Remove host-access capabilities (os.execute family, the io library,
    // require, and package) from a VM. Intended for VMs that serve untrusted
    // entry points such as the HTTP /ops/eval diagnostics endpoint; business
    // VMs keep the full library set.
    void restrict_vm(std::shared_ptr<LuaVM> vm);

    // Load a script into a VM
    /// @param vm The VM to load into
    /// @param script_path Path to the Lua script
    /// @return true if successful
    bool load_script(std::shared_ptr<LuaVM> vm, std::string_view script_path);

    // Load a Lua service module that returns a table.
    bool load_service_module(std::shared_ptr<LuaVM> vm,
                             std::string_view script_path,
                             std::string* error = nullptr);

    // Call a function on the loaded service module table.
    bool call_service_function(std::shared_ptr<LuaVM> vm,
                               std::string_view func_name,
                               const nlohmann::json& args,
                               std::string* error = nullptr);

    // Resolve a method on the loaded service module to a Lua function
    // handle. Returns false (with *error) when the module is not loaded or
    // the name is missing / not a function. Used by spawn-time RPC binding
    // compilation so dispatch can assume handlers exist.
    bool resolve_service_method(std::shared_ptr<LuaVM> vm,
                                std::string_view method_name,
                                sol::function* out = nullptr,
                                std::string* error = nullptr);

    // Dispatch a service method with JSON-array arguments and collect all
    // return values as a JSON array. Missing or non-function methods are
    // errors.
    bool call_service_method(std::shared_ptr<LuaVM> vm,
                             std::string_view method_name,
                             const nlohmann::json& args,
                             nlohmann::json* returns = nullptr,
                             std::string* error = nullptr);

    // Invoke a service method inside a Lua coroutine so it may yield via
    // shield.sleep / coroutine-aware call. Used by the CAF actor dispatch path.
    // If the handler completes without yielding it behaves like
    // call_service_method; if it yields, this returns true (no error) and the
    // suspended coroutine is resumed later by the runtime (e.g. a sleep timer).
    // Return values are not collected on the async path (returns is ignored).
    bool call_service_method_coroutine(std::shared_ptr<LuaVM> vm,
                                       std::string_view method_name,
                                       const nlohmann::json& args,
                                       std::string* error = nullptr,
                                       uint64_t call_session = 0,
                                       LuaServiceManager* manager = nullptr,
                                       std::string_view service_id = "");

    // Invoke a named hook function on a service's module table.
    // The hook is called as hook(err, context_table) where context_table
    // has fields {type, method}. Returns true if the hook existed and was
    // called without error; false otherwise.
    bool invoke_hook(std::shared_ptr<LuaVM> vm, const char* hook_name,
                     const std::string& err_or_reason,
                     const std::string& error_type,
                     const std::string& method_name);

    // Call a function in a VM
    /// @param vm The VM to call in
    /// @param func_name Function name
    /// @param args Arguments (as JSON string)
    /// @return Result as JSON string
    std::string call_function(std::shared_ptr<LuaVM> vm,
                              std::string_view func_name,
                              std::string_view args = "{}");

    // Register API functions
    bool register_api(std::shared_ptr<LuaVM> vm, std::string* error = nullptr);

    // Bind service-management APIs for VMs created by this runtime.
    void set_service_manager(LuaServiceManager* manager);

    // Get service manager for this runtime
    LuaServiceManager* service_manager() const;

    // Get global variable
    std::string get_global(std::shared_ptr<LuaVM> vm, std::string_view name);

    /// @brief Execute arbitrary Lua code on a VM and capture return values.
    ///
    /// Compiles with luaL_loadbuffer, executes with lua_pcall, and converts
    /// all return values to a JSON array.
    ///
    /// @param vm The VM to execute in
    /// @param code Lua source code
    /// @param result Output: JSON array of return values (nullptr to ignore)
    /// @param error Output: error message on failure (nullptr to ignore)
    /// @return true if compilation and execution succeeded
    bool exec_lua(std::shared_ptr<LuaVM> vm, const std::string& code,
                  nlohmann::json* result = nullptr,
                  std::string* error = nullptr);

    // Set global variable
    void set_global(std::shared_ptr<LuaVM> vm, std::string_view name,
                    std::string_view value);

    // Clear script cache
    void clear_cache();

    // Get cache statistics
    size_t cache_size() const;
    LuaCacheConfig cache_config() const;

    // ---- Inbound HTTP routes (shield.httpd.*) ----

    /// Register an HTTP route from a service VM. Must be called on the
    /// registering service's dispatch thread.
    bool register_http_route(std::shared_ptr<LuaVM> vm,
                             const std::string& service_id,
                             const std::string& method, const std::string& path,
                             sol::function handler,
                             std::string* error = nullptr);

    /// Resolve the runtime-managed VM for a raw lua_State (nullptr if the
    /// state does not belong to this runtime or its VM is gone).
    std::shared_ptr<LuaVM> vm_for_state(lua_State* L);

    /// Find a registration matching the concrete method/path. Supports the
    /// same ':'-segment patterns as HttpServer. Returns nullopt when no
    /// route matches. `out_params` (optional) receives captured parameters.
    std::optional<HttpRouteRegistration> find_http_route(
        const std::string& method, const std::string& path,
        std::vector<std::pair<std::string, std::string>>* out_params =
            nullptr) const;

    /// All currently registered routes (for initial attach by the bridge).
    std::vector<HttpRouteRegistration> http_routes() const;

    /// Number of registered routes.
    size_t http_route_count() const;

    /// Remove every route registered by a service (called on service stop).
    void remove_http_routes_for_service(const std::string& service_id);

    /// Sink notified on every successful registration. Set by the bootstrap
    /// HTTP bridge so late registrations reach a running HttpServer. Pass an
    /// empty function to detach.
    void set_http_route_sink(
        std::function<void(const std::string& method, const std::string& path)>
            sink);

    /// Invoke a registered HTTP handler on its VM. Must be called on the
    /// owning service's dispatch thread (the forked-task path).
    /// `request_json` carries {method, path, query, params, headers, body};
    /// `out_desc` receives {status, body, json_body, headers?} or
    /// {lua_error} on handler failure. Returns false on internal errors.
    bool call_http_handler(const HttpRouteRegistration& route,
                           const nlohmann::json& request_json,
                           nlohmann::json& out_desc, std::string* error);

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
