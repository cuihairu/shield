// [SHIELD_LUA] Lua API registration
#include "shield/lua/lua_api.hpp"

#include "shield/config/config.hpp"
#include "shield/data/data.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <vector>

namespace shield::lua {
namespace {

nlohmann::json lua_to_json(const sol::object& value);
sol::variadic_results call_with_timeout(sol::this_state state,
                                        LuaServiceManager* manager,
                                        LuaRuntime* runtime,
                                        int timeout_ms,
                                        const std::string& target,
                                        const std::string& method,
                                        const nlohmann::json& args);

sol::table make_error(sol::this_state state,
                      std::string code,
                      std::string message,
                      bool retryable = false) {
    sol::state_view lua(state);
    return lua.create_table_with("code", std::move(code),
                                 "message", std::move(message),
                                 "retryable", retryable);
}

sol::object json_to_lua(sol::state_view lua, const nlohmann::json& value) {
    if (value.is_null()) {
        return sol::make_object(lua, sol::nil);
    }
    if (value.is_boolean()) {
        return sol::make_object(lua, value.get<bool>());
    }
    if (value.is_number_integer()) {
        return sol::make_object(lua, value.get<std::int64_t>());
    }
    if (value.is_number_unsigned()) {
        return sol::make_object(lua, value.get<std::uint64_t>());
    }
    if (value.is_number_float()) {
        return sol::make_object(lua, value.get<double>());
    }
    if (value.is_string()) {
        return sol::make_object(lua, value.get<std::string>());
    }
    if (value.is_array()) {
        sol::table table = lua.create_table();
        int index = 1;
        for (const auto& item : value) {
            table[index++] = json_to_lua(lua, item);
        }
        return sol::make_object(lua, table);
    }
    if (value.is_object()) {
        sol::table table = lua.create_table();
        for (const auto& [key, item] : value.items()) {
            table[key] = json_to_lua(lua, item);
        }
        return sol::make_object(lua, table);
    }
    return sol::make_object(lua, sol::nil);
}

nlohmann::json lua_table_to_json(const sol::table& table) {
    bool array_like = true;
    std::size_t max_index = 0;
    std::size_t entry_count = 0;

    for (const auto& [key, _] : table) {
        ++entry_count;
        sol::object key_obj = key;
        if (!key_obj.is<int>()) {
            array_like = false;
            break;
        }

        const int index = key_obj.as<int>();
        if (index <= 0) {
            array_like = false;
            break;
        }
        max_index = std::max(max_index, static_cast<std::size_t>(index));
    }

    if (array_like && max_index == entry_count) {
        nlohmann::json array = nlohmann::json::array();
        for (std::size_t i = 1; i <= max_index; ++i) {
            array.push_back(lua_to_json(table[static_cast<int>(i)]));
        }
        return array;
    }

    nlohmann::json object = nlohmann::json::object();
    for (const auto& [key, value] : table) {
        sol::object key_obj = key;
        std::string object_key;
        if (key_obj.is<std::string>()) {
            object_key = key_obj.as<std::string>();
        } else if (key_obj.is<int>()) {
            object_key = std::to_string(key_obj.as<int>());
        } else {
            continue;
        }
        object[object_key] = lua_to_json(value);
    }
    return object;
}

nlohmann::json lua_to_json(const sol::object& value) {
    if (!value.valid() || value == sol::nil) {
        return nullptr;
    }
    if (value.is<bool>()) {
        return value.as<bool>();
    }
    if (value.is<std::int64_t>()) {
        return value.as<std::int64_t>();
    }
    if (value.is<int>()) {
        return value.as<int>();
    }
    if (value.is<double>()) {
        return value.as<double>();
    }
    if (value.is<std::string>()) {
        return value.as<std::string>();
    }
    if (value.is<sol::table>()) {
        return lua_table_to_json(value.as<sol::table>());
    }
    return "<unsupported>";
}

nlohmann::json variadic_to_json_array(sol::variadic_args args) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& arg : args) {
        values.push_back(lua_to_json(arg));
    }
    return values;
}

std::vector<std::string> table_to_string_vector(const sol::table& table) {
    std::vector<std::string> params;
    for (const auto& [_, value] : table) {
        sol::object item = value;
        if (item == sol::nil) {
            params.emplace_back("");
        } else if (item.is<std::string>()) {
            params.push_back(item.as<std::string>());
        } else if (item.is<int>()) {
            params.push_back(std::to_string(item.as<int>()));
        } else if (item.is<std::int64_t>()) {
            params.push_back(std::to_string(item.as<std::int64_t>()));
        } else if (item.is<double>()) {
            params.push_back(std::to_string(item.as<double>()));
        } else if (item.is<bool>()) {
            params.push_back(item.as<bool>() ? "true" : "false");
        } else {
            params.push_back(lua_to_json(item).dump());
        }
    }
    return params;
}

// Helper to extract service ID from ServiceHandle or string
std::string extract_service_id(const sol::object& target) {
    if (target.is<ServiceHandle>()) {
        return target.as<ServiceHandle>().id();
    }
    if (target.is<std::string>()) {
        return target.as<std::string>();
    }
    return "";
}

void register_service_api(sol::table& shield, LuaServiceManager* manager) {
    shield.set_function("spawn",
        [manager](sol::this_state state,
                  std::string module,
                  sol::optional<sol::table> opts) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(make_error(state, "runtime_unavailable",
                                             "Lua service manager is not available"));
                return results;
            }

            nlohmann::json options = opts ? lua_table_to_json(*opts)
                                          : nlohmann::json::object();
            if (!options.is_object()) {
                options = nlohmann::json::object();
            }

            SpawnResult result = manager->spawn(module, options.dump());
            if (!result.success) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(make_error(state, "spawn_failed",
                                             result.error_message));
                return results;
            }

            // Return ServiceHandle userdata instead of string
            results.push_back(sol::make_object(lua, sol::make_object<ServiceHandle>(
                result.service_id)));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    shield.set_function("exit",
        [manager](sol::optional<std::string> reason) {
            if (manager) {
                manager->request_current_exit(reason.value_or("normal"));
            }
        });

    shield.set_function("self",
        [manager](sol::this_state state) -> sol::object {
            sol::state_view lua(state);
            if (!manager) {
                return sol::make_object(lua, sol::nil);
            }
            const auto service_id = manager->current_service_id();
            if (service_id.empty()) {
                return sol::make_object(lua, sol::nil);
            }
            return sol::make_object(lua, sol::make_object<ServiceHandle>(service_id));
        });
                return sol::nullopt;
            }
            const auto service_id = manager->current_service_id();
            if (service_id.empty()) {
                return sol::nullopt;
            }
            return service_id;
        });

    shield.set_function("names",
        [manager](sol::this_state state) -> sol::table {
            sol::state_view lua(state);
            sol::table names = lua.create_table();
            if (!manager) {
                return names;
            }

            int index = 1;
            for (const auto& name : manager->list_services()) {
                names[index++] = name;
            }
            return names;
        });

    shield.set_function("query",
        [manager](sol::this_state state,
                  std::string name) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(make_error(state, "runtime_unavailable",
                                             "Lua service manager is not available"));
                return results;
            }

            const auto service = manager->query_service(name);
            if (!service.empty()) {
                results.push_back(sol::make_object(lua, sol::make_object<ServiceHandle>(
                    service)));
                results.push_back(sol::make_object(lua, sol::nil));
                return results;
            }

            results.push_back(sol::make_object(lua, sol::nil));
            results.push_back(make_error(state, "service_not_found",
                                         "service not found: " + name));
            return results;
        });

    shield.set_function("register",
        [manager](sol::this_state state, std::string name) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "runtime_unavailable",
                                             "Lua service manager is not available"));
                return results;
            }

            std::string error;
            if (!manager->register_name(name, &error)) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "register_failed", error));
                return results;
            }

            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    shield.set_function("unregister",
        [manager](sol::this_state state, std::string name) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "runtime_unavailable",
                                             "Lua service manager is not available"));
                return results;
            }

            std::string error;
            if (!manager->unregister_name(name, &error)) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "unregister_failed", error));
                return results;
            }

            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });
}

void register_message_api(sol::table& shield, LuaServiceManager* manager,
                          LuaRuntime* runtime) {
    shield.set_function("send",
        [manager](sol::this_state state,
                  sol::object target,
                  std::string method,
                  sol::variadic_args args) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "runtime_unavailable",
                                             "Lua service manager is not available"));
                return results;
            }

            std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "invalid_target",
                                             "target must be ServiceHandle or string"));
                return results;
            }

            std::string error;
            if (!manager->send(target_id, method, variadic_to_json_array(args),
                               &error)) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "send_failed", error));
                return results;
            }

            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    shield.set_function("call",
        [manager, runtime](sol::this_state state,
                  sol::object target,
                  std::string method,
                  sol::variadic_args args) -> sol::variadic_results {
            std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                sol::state_view lua(state);
                sol::variadic_results results;
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "invalid_target",
                                             "target must be ServiceHandle or string"));
                return results;
            }
            return call_with_timeout(state, manager, runtime, 5000, target_id, method,
                                     variadic_to_json_array(args));
        });

    shield.set_function("call_timeout",
        [manager, runtime](sol::this_state state,
                  int timeout_ms,
                  sol::object target,
                  std::string method,
                  sol::variadic_args args) -> sol::variadic_results {
            std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                sol::state_view lua(state);
                sol::variadic_results results;
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "invalid_target",
                                             "target must be ServiceHandle or string"));
                return results;
            }
            return call_with_timeout(state, manager, runtime, timeout_ms, target_id, method,
                                     variadic_to_json_array(args));
        });

    shield.set_function("sender",
        [manager]() -> sol::optional<std::string> {
            if (!manager) {
                return sol::nullopt;
            }
            const auto sender = manager->current_sender_id();
            if (sender.empty()) {
                return sol::nullopt;
            }
            return sender;
        });

    shield.set_function("trace", []() -> std::string {
        return "trace:0";
    });

    shield.set_function("deadline", []() -> sol::optional<int64_t> {
        return sol::nullopt;
    });
}

sol::variadic_results call_with_timeout(sol::this_state state,
                                        LuaServiceManager* manager,
                                        LuaRuntime* runtime,
                                        int timeout_ms,
                                        const std::string& target,
                                        const std::string& method,
                                        const nlohmann::json& args) {
    sol::state_view lua(state);
    sol::variadic_results results;
    if (!manager) {
        results.push_back(sol::make_object(lua, false));
        results.push_back(make_error(state, "runtime_unavailable",
                                     "Lua service manager is not available"));
        return results;
    }

    // Check if we're in a coroutine context
    sol::co_context co_context = lua.get<sol::co_context>("_ coroutine_ctx");
    bool in_coroutine = co_context.valid();

    if (!in_coroutine || !runtime) {
        // Fallback to synchronous implementation
        CallResult result = manager->call(target, method, args, timeout_ms);
        if (!result.success) {
            results.push_back(sol::make_object(lua, false));
            results.push_back(make_error(state, "call_failed",
                                         result.error_message));
            return results;
        }

        results.push_back(sol::make_object(lua, true));
        for (const auto& value : result.values) {
            results.push_back(json_to_lua(lua, value));
        }
        return results;
    }

    // Coroutine-aware implementation
    // Get current coroutine
    sol::coroutine current_co = co_context;

    // Get current service ID
    const std::string current_service = manager->current_service_id();
    if (current_service.empty()) {
        results.push_back(sol::make_object(lua, false));
        results.push_back(make_error(state, "no_service_context",
                                     "Not in a service context"));
        return results;
    }

    // Suspend current coroutine and register callback
    CoroutineScheduler::CoroutineId coro_id =
        runtime->coroutine_scheduler().suspend(current_service, current_co, timeout_ms);

    // Send message and register callback to resume coroutine
    // TODO: This needs async call support in LuaServiceManager
    // For now, fallback to sync
    CallResult result = manager->call(target, method, args, timeout_ms);

    // Resume coroutine with result
    runtime->coroutine_scheduler().resume(coro_id, result.values);

    results.push_back(sol::make_object(lua, true));
    for (const auto& value : result.values) {
        results.push_back(json_to_lua(lua, value));
    }
    return results;
}

void register_timer_api(sol::table& shield, LuaRuntime* runtime) {
    shield.set_function("now", []() -> int64_t {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    });

    shield.set_function("timer_once",
        [runtime](int delay_ms, sol::function callback) -> uint64_t {
            if (!runtime) {
                // Fallback to thread-based implementation
                static std::atomic<uint64_t> timer_id{1};
                const uint64_t id = timer_id.fetch_add(1);
                std::thread([delay_ms, callback]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                    callback();
                }).detach();
                return id;
            }

            // Get current service ID
            auto& manager = runtime->coroutine_scheduler();  // Access to service context needed
            // For now, use empty service_id; proper implementation needs service context
            return runtime->timer_manager().schedule_once(delay_ms, callback, "");
        });

    shield.set_function("timer",
        [runtime](int interval_ms, sol::function callback) -> uint64_t {
            if (!runtime) {
                // Fallback to thread-based implementation
                static std::atomic<uint64_t> timer_id{1};
                const uint64_t id = timer_id.fetch_add(1);
                std::thread([interval_ms, callback]() {
                    while (true) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(interval_ms));
                        callback();
                    }
                }).detach();
                return id;
            }

            return runtime->timer_manager().schedule_fixed_delay(interval_ms, callback, "");
        });

    shield.set_function("cancel_timer",
        [runtime](sol::this_state state, uint64_t id) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;

            if (!runtime) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "runtime_unavailable",
                                             "Timer manager is not available"));
                return results;
            }

            const bool cancelled = runtime->timer_manager().cancel(id);
            results.push_back(sol::make_object(lua, cancelled));
            if (!cancelled) {
                results.push_back(make_error(state, "timer_not_found",
                                             "Timer not found or already completed"));
            } else {
                results.push_back(sol::make_object(lua, sol::nil));
            }
            return results;
        });

    shield.set_function("sleep", [runtime](int delay_ms) {
        if (!runtime) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            return;
        }

        // Get the current coroutine
        sol::state_view lua = sol::state_view(runtime->coroutine_scheduler());
        // Note: This is a simplified implementation that still blocks
        // A full implementation would need to access the current coroutine
        // from the Lua state and suspend it
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    });
}

void register_task_api(sol::table& shield, LuaRuntime* runtime) {
    static std::atomic<uint64_t> task_id{1};
    (void)runtime;  // Will be used for coroutine-aware fork

    shield.set_function("fork",
        [](sol::function fn) -> uint64_t {
            const uint64_t id = task_id.fetch_add(1);
            std::thread([fn, id]() {
                try {
                    fn();
                } catch (const std::exception& e) {
                    auto& log = shield::log::get_logger("lua");
                    SHIELD_LOG_ERROR(log, "task " + std::to_string(id) +
                                     " error: " + e.what());
                }
            }).detach();
            return id;
        });
}

void register_config_api(sol::table& shield) {
    shield.set_function("config",
        [](sol::this_state state,
           std::string key,
           sol::optional<sol::object> default_value) -> sol::object {
            sol::state_view lua(state);
            auto& config = shield::config::global_config();
            if (!config.has(key)) {
                if (default_value) {
                    return *default_value;
                }
                return sol::make_object(lua, sol::nil);
            }

            const auto value = config.get_string(key, "");
            if (value == "true") {
                return sol::make_object(lua, true);
            }
            if (value == "false") {
                return sol::make_object(lua, false);
            }

            try {
                return sol::make_object(lua, std::stoll(value));
            } catch (const std::exception&) {}

            try {
                return sol::make_object(lua, std::stod(value));
            } catch (const std::exception&) {}

            return sol::make_object(lua, value);
        });
}

void register_log_api(sol::table& shield) {
    auto& log = shield::log::get_logger("lua");
    sol::state_view lua(shield.lua_state());
    auto log_table = lua.create_table();

    log_table.set_function("debug", [&log](sol::object value) {
        SHIELD_LOG_DEBUG(log, lua_to_json(value).dump());
    });
    log_table.set_function("info", [&log](sol::object value) {
        SHIELD_LOG_INFO(log, lua_to_json(value).dump());
    });
    log_table.set_function("warn", [&log](sol::object value) {
        SHIELD_LOG_WARNING(log, lua_to_json(value).dump());
    });
    log_table.set_function("error", [&log](sol::object value) {
        SHIELD_LOG_ERROR(log, lua_to_json(value).dump());
    });

    shield["log"] = log_table;
}

void register_data_api(sol::table& shield) {
    sol::state_view lua(shield.lua_state());

    auto db_table = lua.create_table();
    db_table.set_function("query",
        [](sol::this_state state,
           std::string sql,
           sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& db = shield::data::database();
            if (!db.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "database is not initialized"));
                return results;
            }

            auto query_result = db.query(sql, params ? table_to_string_vector(*params)
                                                     : std::vector<std::string>{});
            results.push_back(sol::make_object(lua, query_result.success));
            if (!query_result.success) {
                results.push_back(make_error(state, "database_error",
                                             query_result.error_message));
                return results;
            }

            sol::table rows = lua.create_table();
            int row_index = 1;
            for (const auto& row : query_result.rows) {
                sol::table row_table = lua.create_table();
                for (const auto& [column, value] : row) {
                    row_table[column] = value;
                }
                rows[row_index++] = row_table;
            }
            results.push_back(rows);
            return results;
        });

    db_table.set_function("query_one",
        [](sol::this_state state,
           std::string sql,
           sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& db = shield::data::database();
            if (!db.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "database is not initialized"));
                return results;
            }

            auto query_result =
                db.query_one(sql, params ? table_to_string_vector(*params)
                                         : std::vector<std::string>{});
            results.push_back(sol::make_object(lua, query_result.success));
            if (!query_result.success || query_result.rows.empty()) {
                results.push_back(sol::make_object(lua, sol::nil));
                return results;
            }

            sol::table row_table = lua.create_table();
            for (const auto& [column, value] : query_result.rows.front()) {
                row_table[column] = value;
            }
            results.push_back(row_table);
            return results;
        });

    db_table.set_function("execute",
        [](sol::this_state state,
           std::string sql,
           sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& db = shield::data::database();
            if (!db.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "database is not initialized"));
                return results;
            }

            auto exec_result =
                db.execute(sql, params ? table_to_string_vector(*params)
                                       : std::vector<std::string>{});
            results.push_back(sol::make_object(lua, exec_result.success));
            if (!exec_result.success) {
                results.push_back(make_error(state, "database_error",
                                             exec_result.error_message));
                return results;
            }
            sol::table result_table = lua.create_table_with(
                "affected", exec_result.affected_rows,
                "last_insert_id", exec_result.last_insert_id);
            results.push_back(result_table);
            return results;
        });
    shield["db"] = db_table;

    auto redis_table = lua.create_table();
    redis_table.set_function("get",
        [](sol::this_state state, std::string key) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& redis = shield::data::redis();
            if (!redis.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "redis is not initialized"));
                return results;
            }

            auto [ok, value] = redis.get(key);
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? sol::make_object(lua, value)
                                 : sol::make_object(lua, sol::nil));
            return results;
        });

    redis_table.set_function("set",
        [](sol::this_state state,
           std::string key,
           std::string value,
           sol::optional<int> ttl) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& redis = shield::data::redis();
            if (!redis.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "redis is not initialized"));
                return results;
            }

            const bool ok = redis.set(key, value, ttl.value_or(0));
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? sol::make_object(lua, sol::nil)
                                 : make_error(state, "redis_error",
                                              "redis set failed"));
            return results;
        });

    redis_table.set_function("publish",
        [](sol::this_state state,
           std::string channel,
           sol::object message) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& redis = shield::data::redis();
            if (!redis.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "redis is not initialized"));
                return results;
            }

            int receivers = redis.publish(channel, lua_to_json(message).dump());
            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, receivers));
            return results;
        });

    redis_table.set_function("del",
        [](sol::this_state state, std::string key) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& redis = shield::data::redis();
            if (!redis.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "redis is not initialized"));
                return results;
            }

            const int removed = redis.del(key);
            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, removed));
            return results;
        });

    redis_table.set_function("exists",
        [](sol::this_state state, std::string key) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& redis = shield::data::redis();
            if (!redis.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "redis is not initialized"));
                return results;
            }

            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, redis.exists(key)));
            return results;
        });
    shield["redis"] = redis_table;
}

}  // namespace

void register_shield_api(LuaRuntime& runtime) {
    (void)runtime;
}

namespace api {

void register_service_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_message_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_timer_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_task_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_config_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_log_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_data_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_gateway_api(LuaRuntime& runtime) {
    (void)runtime;
}

}  // namespace api

void register_full_shield_api(sol::state& lua, LuaServiceManager* manager,
                               LuaRuntime* runtime) {
    // Register ServiceHandle usertype first
    ServiceHandle::register_usertype(lua);

    auto shield = lua.create_table();

    register_service_api(shield, manager);
    register_message_api(shield, manager, runtime);
    register_timer_api(shield, runtime);
    register_task_api(shield, runtime);
    register_config_api(shield);
    register_log_api(shield);
    register_data_api(shield);

    lua["shield"] = shield;
}

}  // namespace shield::lua
