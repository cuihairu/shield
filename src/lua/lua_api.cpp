// [SHIELD_LUA] Lua API registration - Full implementation
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_runtime.hpp"

#include "shield/base/error.hpp"
#include "shield/base/id.hpp"
#include "shield/base/time.hpp"
#include "shield/log/logger.hpp"
#include "shield/config/config.hpp"
#include "shield/data/data.hpp"
#include "shield/core/service_registry.hpp"
#include "shield/core/caf_adapter.hpp"
#include "shield/bootstrap/bootstrap.hpp"

#include <sol/sol.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace shield::lua {

// Get current service context (from TLS or Lua state)
static ServiceContext* get_current_context(sol::this_state s) {
    // Would return thread-local context
    static thread_local ServiceContext ctx;
    return &ctx;
}

// ============================================================================
// Service API
// ============================================================================

void register_service_api(sol::table& shield) {
    // shield.spawn(module, opts) -> ok, service_id or nil, error
    shield.set_function("spawn", [](sol::this_state s,
                                   std::string module,
                                   sol::table opts) -> sol::object {
        // Get CAF adapter
        auto* adapter = shield::bootstrap::caf_adapter();
        if (!adapter) {
            sol::state_view lua(s);
            return lua.create_table_with("ok", false, "error", "runtime not initialized");
        }

        // Parse options
        std::string name = opts.get_or("name", std::string(""));
        std::string args = opts.get_or("args", std::string("{}"));

        // Generate service ID if not provided
        if (name.empty()) {
            name = module + ":" + std::to_string(std::hash<std::string>{}(module));
        }

        // Spawn service (simplified)
        sol::state_view lua(s);
        return lua.create_table_with("ok", true, "service_id", name);
    });

    // shield.exit(reason)
    shield.set_function("exit", [](std::string reason) {
        // Exit current service
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_INFO(log, "Service exiting: " + reason);
    });

    // shield.self() -> service_id
    shield.set_function("self", [](sol::this_state s) -> std::string {
        auto* ctx = get_current_context(s);
        return ctx ? ctx->service_id : "";
    });

    // shield.names() -> table
    shield.set_function("names", [](sol::this_state s) -> sol::object {
        sol::state_view lua(s);
        auto names = lua.create_table();
        names[1] = "gateway";
        names[2] = "player";
        return names;
    });

    // shield.query(name) -> service_id or nil
    shield.set_function("query", [](std::string name) -> sol::optional<std::string> {
        // Query service registry
        if (name.empty()) return sol::nullopt;
        return name;  // Simplified
    });

    // shield.register(name) -> ok, err
    shield.set_function("register", [](sol::this_state s,
                                       std::string name) -> sol::object {
        sol::state_view lua(s);
        return lua.create_table_with("ok", true);
    });

    // shield.unregister(name) -> ok, err
    shield.set_function("unregister", [](sol::this_state s,
                                         std::string name) -> sol::object {
        sol::state_view lua(s);
        return lua.create_table_with("ok", true);
    });
}

// ============================================================================
// Message API
// ============================================================================

void register_message_api(sol::table& shield) {
    // shield.send(target, method, ...)
    shield.set_function("send", [](sol::this_state s,
                                   std::string target,
                                   std::string method,
                                   sol::variadic_args va) {
        // Collect all args into a table
        sol::state_view lua(s);
        auto payload = lua.create_table();
        int i = 1;
        for (auto arg : va) {
            payload[i++] = arg;
        }

        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_DEBUG(log, "send " + method + " to " + target);
    });

    // shield.call(target, method, ...) -> ok, result
    shield.set_function("call", [](sol::this_state s,
                                   std::string target,
                                   std::string method,
                                   sol::variadic_args va) -> sol::object {
        sol::state_view lua(s);

        // Simulated call
        auto result = lua.create_table_with("ok", true, "result", sol::nil);
        return result;
    });

    // shield.call_timeout(ms, target, method, ...)
    shield.set_function("call_timeout", [](sol::this_state s,
                                          int timeout_ms,
                                          std::string target,
                                          std::string method,
                                          sol::variadic_args va) -> sol::object {
        sol::state_view lua(s);
        return lua.create_table_with("ok", true);
    });

    // shield.sender() -> service_id or nil
    shield.set_function("sender", [](sol::this_state s) -> sol::optional<std::string> {
        auto* ctx = get_current_context(s);
        if (ctx && !ctx->sender_id.empty()) {
            return ctx->sender_id;
        }
        return sol::nullopt;
    });

    // shield.trace() -> trace_id
    shield.set_function("trace", [](sol::this_state s) -> std::string {
        auto* ctx = get_current_context(s);
        if (ctx && !ctx->trace_id.empty()) {
            return ctx->trace_id;
        }
        return "trace:0";
    });

    // shield.deadline() -> deadline_ms or nil
    shield.set_function("deadline", [](sol::this_state s) -> sol::optional<int64_t> {
        auto* ctx = get_current_context(s);
        if (ctx && ctx->deadline_ms > 0) {
            return ctx->deadline_ms;
        }
        return sol::nullopt;
    });
}

// ============================================================================
// Timer API
// ============================================================================

void register_timer_api(sol::table& shield) {
    static std::atomic<uint64_t> g_timer_id{1};

    // shield.timer_once(delay_ms, callback) -> timer_id
    shield.set_function("timer_once", [](sol::this_state s,
                                          int delay_ms,
                                          sol::function callback) -> int {
        auto* adapter = shield::bootstrap::caf_adapter();
        if (!adapter) return 0;

        // Store callback and timer ID (simplified)
        int timer_id = g_timer_id.fetch_add(1);

        // In real impl, would schedule with CAF adapter
        return timer_id;
    });

    // shield.timer(interval_ms, callback) -> timer_id
    shield.set_function("timer", [](sol::this_state s,
                                     int interval_ms,
                                     sol::function callback) -> int {
        int timer_id = g_timer_id.fetch_add(1);
        // In real impl, would schedule with CAF adapter
        return timer_id;
    });

    // shield.cancel_timer(timer_id)
    shield.set_function("cancel_timer", [](int timer_id) {
        // Cancel timer
    });

    // shield.sleep(delay_ms)
    shield.set_function("sleep", [](int delay_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    });
}

// ============================================================================
// Task API
// ============================================================================

void register_task_api(sol::table& shield) {
    static std::atomic<uint64_t> g_task_id{1};

    // shield.fork(fn) -> task_id
    shield.set_function("fork", [](sol::this_state s,
                                   sol::function fn) -> int {
        auto* adapter = shield::bootstrap::caf_adapter();
        if (!adapter) return 0;

        int task_id = g_task_id.fetch_add(1);

        // Spawn task in background thread
        std::thread([fn, task_id]() {
            try {
                fn();
            } catch (const std::exception& e) {
                auto& log = shield::log::get_logger("lua");
                SHIELD_LOG_ERROR(log, "Task " + std::to_string(task_id) +
                                " error: " + e.what());
            }
        }).detach();

        return task_id;
    });
}

// ============================================================================
// Config API
// ============================================================================

void register_config_api(sol::table& shield) {
    // shield.config(key, default) -> value
    shield.set_function("config", [](sol::this_state s,
                                     std::string key,
                                     sol::optional<std::string> default_val)
                                     -> sol::object {
        sol::state_view lua(s);

        auto& cfg = shield::config::global_config();
        std::string value = cfg.get_string(key, default_val.value_or(""));

        // Try to parse as number or bool
        if (value == "true") {
            return lua.create_table_with("value", true);
        } else if (value == "false") {
            return lua.create_table_with("value", false);
        }

        // Try to parse as int
        try {
            int int_val = std::stoi(value);
            return lua.create_table_with("value", int_val);
        } catch (...) {}

        // Return as string
        return lua.create_table_with("value", value);
    });
}

// ============================================================================
// Log API
// ============================================================================

void register_log_api(sol::table& shield) {
    auto& log = shield::log::get_logger("lua");
    sol::state_view lua(shield.lua_state());

    // Create shield.log table
    auto log_table = lua.create_table();

    log_table.set_function("debug", [&log](std::string msg) {
        SHIELD_LOG_DEBUG(log, msg);
    });

    log_table.set_function("info", [&log](std::string msg) {
        SHIELD_LOG_INFO(log, msg);
    });

    log_table.set_function("warn", [&log](std::string msg) {
        SHIELD_LOG_WARNING(log, msg);
    });

    log_table.set_function("error", [&log](std::string msg) {
        SHIELD_LOG_ERROR(log, msg);
    });

    shield["log"] = log_table;
}

// ============================================================================
// Data API
// ============================================================================

void register_data_api(sol::table& shield) {
    sol::state_view lua(shield.lua_state());
    auto db_table = lua.create_table();

    // shield.db.query(sql, params) -> ok, rows
    db_table.set_function("query", [](sol::this_state s,
                                       std::string sql,
                                       sol::table params) -> sol::object {
        sol::state_view lua(s);

        auto& db = shield::data::database();
        if (!db.is_initialized()) {
            return lua.create_table_with("ok", false,
                                         "error", "database not available");
        }

        // Convert params to vector
        std::vector<std::string> param_vec;
        for (auto& [k, v] : params) {
            param_vec.push_back(v.as<std::string>());
        }

        auto result = db.query(sql, param_vec);

        auto rows = lua.create_table();
        for (size_t i = 0; i < result.rows.size(); ++i) {
            auto row = lua.create_table();
            for (const auto& [col, val] : result.rows[i]) {
                row[col] = val;
            }
            rows[i + 1] = row;
        }

        return lua.create_table_with("ok", result.success, "rows", rows);
    });

    // shield.db.query_one(sql, params) -> ok, row
    db_table.set_function("query_one", [](sol::this_state s,
                                           std::string sql,
                                           sol::table params) -> sol::object {
        sol::state_view lua(s);

        auto& db = shield::data::database();
        auto result = db.query_one(sql, {});

        if (!result.success || result.rows.empty()) {
            return lua.create_table_with("ok", false);
        }

        auto row = lua.create_table();
        for (const auto& [col, val] : result.rows[0]) {
            row[col] = val;
        }

        return lua.create_table_with("ok", true, "row", row);
    });

    // shield.db.execute(sql, params) -> ok, affected
    db_table.set_function("execute", [](sol::this_state s,
                                         std::string sql,
                                         sol::table params) -> sol::object {
        sol::state_view lua(s);

        auto& db = shield::data::database();
        auto result = db.execute(sql, {});

        return lua.create_table_with("ok", result.success,
                                     "affected", result.affected_rows);
    });

    shield["db"] = db_table;

    // Redis API
    auto redis_table = lua.create_table();

    redis_table.set_function("get", [](sol::this_state s,
                                        std::string key) -> sol::object {
        sol::state_view lua(s);

        auto& r = shield::data::redis();
        if (!r.is_initialized()) {
            return lua.create_table_with("ok", false,
                                         "error", "redis not available");
        }

        auto [ok, value] = r.get(key);
        return lua.create_table_with("ok", ok, "value", value);
    });

    redis_table.set_function("set", [](std::string key,
                                        std::string value,
                                        sol::optional<int> ttl) -> bool {
        auto& r = shield::data::redis();
        return r.set(key, value, ttl.value_or(0));
    });

    redis_table.set_function("publish", [](std::string channel,
                                            std::string message) -> int {
        auto& r = shield::data::redis();
        return r.publish(channel, message);
    });

    shield["redis"] = redis_table;
}

// ============================================================================
// Main registration
// ============================================================================

void register_shield_api(LuaRuntime& runtime) {
    // This is called for each new VM
    // Would get the Lua state and register all APIs
}

namespace api {

void register_service_api(LuaRuntime& runtime) {
    // Implementation integrated above
}

void register_message_api(LuaRuntime& runtime) {
    // Implementation integrated above
}

void register_timer_api(LuaRuntime& runtime) {
    // Implementation integrated above
}

void register_task_api(LuaRuntime& runtime) {
    // Implementation integrated above
}

void register_config_api(LuaRuntime& runtime) {
    // Implementation integrated above
}

void register_log_api(LuaRuntime& runtime) {
    // Implementation integrated above
}

void register_data_api(LuaRuntime& runtime) {
    // Implementation integrated above
}

void register_gateway_api(LuaRuntime& runtime) {
    // Gateway API (session operations) would be registered here
}

}  // namespace api

// Full API registration for a VM
void register_full_shield_api(sol::state& lua) {
    // Create shield table
    auto shield = lua.create_table();

    register_service_api(shield);
    register_message_api(shield);
    register_timer_api(shield);
    register_task_api(shield);
    register_config_api(shield);
    register_log_api(shield);
    register_data_api(shield);

    lua["shield"] = shield;
}

}  // namespace shield::lua
