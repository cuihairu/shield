// [SHIELD_LUA] Lua API registration
#include "shield/lua/lua_api.hpp"

#include "shield/config/config.hpp"
#ifdef SHIELD_ENABLE_CLUSTER
#include "shield/cluster/cluster_manager.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shield/log/logger.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/session.hpp"
#include "shield/net/http_client.hpp"
#include "shield/plugin/plugin_host.hpp"

namespace shield::lua {

// Resource limits per service.
static constexpr size_t kTimerLimit = 10000;
static constexpr size_t kForkLimit = 1000;

nlohmann::json lua_to_json(const sol::object& value);
sol::variadic_results call_with_timeout(sol::this_state state,
                                        LuaServiceManager* manager,
                                        LuaRuntime* runtime, int timeout_ms,
                                        const std::string& target,
                                        const std::string& method,
                                        const nlohmann::json& args);

sol::table make_error(sol::this_state state, std::string code,
                      std::string message, bool retryable = false,
                      sol::object detail = sol::nil) {
    sol::state_view lua(state);
    sol::table err = lua.create_table();
    err["code"] = std::move(code);
    err["message"] = std::move(message);
    err["retryable"] = retryable;
    if (detail.valid() && detail != sol::nil) {
        err["detail"] = detail;
    }
    return err;
}

namespace {

static constexpr std::string_view kSessionHandleMarker =
    "__shield_session_handle";

std::unordered_map<std::string, std::weak_ptr<shield::net::Session>>&
session_handle_registry() {
    static std::unordered_map<std::string, std::weak_ptr<shield::net::Session>>
        registry;
    return registry;
}

std::mutex& session_handle_registry_mutex() {
    static std::mutex mutex;
    return mutex;
}

void remember_session_handle(
    const std::shared_ptr<shield::net::Session>& session) {
    if (!session) {
        return;
    }

    std::lock_guard<std::mutex> lock(session_handle_registry_mutex());
    session_handle_registry()[std::to_string(session->id())] = session;
}

std::shared_ptr<shield::net::Session> resolve_session_handle(
    std::string_view session_id) {
    std::lock_guard<std::mutex> lock(session_handle_registry_mutex());
    auto& registry = session_handle_registry();
    const auto it = registry.find(std::string(session_id));
    if (it == registry.end()) {
        return nullptr;
    }

    auto session = it->second.lock();
    if (!session) {
        registry.erase(it);
    }
    return session;
}

}  // namespace

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
        if (value.contains(std::string(kSessionHandleMarker)) &&
            value[std::string(kSessionHandleMarker)].is_boolean() &&
            value[std::string(kSessionHandleMarker)].get<bool>()) {
            sol::object maybe_ud = lua["__shield_make_session_handle"];
            if (maybe_ud.valid() && maybe_ud.is<sol::protected_function>()) {
                sol::protected_function make_handle =
                    maybe_ud.as<sol::protected_function>();
                auto result = make_handle(
                    value.value("id", std::string{}),
                    value.value("remote_addr", std::string{}));
                if (result.valid() && result.return_count() > 0) {
                    return result.get<sol::object>(0);
                }
            }
        }
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
    if (value.is<double>()) {
        // sol2's is<int64_t>() accepts any Lua number when
        // SOL_NUMBER_PRECISION_CHECKS is off (the default), so checking it
        // first would let as<int64_t>() truncate floats (e.g. 3.14 -> 3).
        // Read as double, then only round-trip through int64_t when the
        // value is a whole number so the JSON keeps its original type.
        const double d = value.as<double>();
        const auto as_int = static_cast<std::int64_t>(d);
        if (static_cast<double>(as_int) == d) {
            return as_int;
        }
        return d;
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
    const auto size = table.size();
    params.reserve(size);
    for (std::size_t i = 1; i <= size; ++i) {
        sol::object item = table[static_cast<int>(i)];
        if (item == sol::nil) {
            params.emplace_back("");
        } else if (item.is<std::string>()) {
            params.push_back(item.as<std::string>());
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
    shield.set_function(
        "spawn",
        [manager](sol::this_state state, std::string module,
                  sol::optional<sol::table> opts) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(
                    make_error(state, "runtime_unavailable",
                               "Lua service manager is not available"));
                return results;
            }

            nlohmann::json options =
                opts ? lua_table_to_json(*opts) : nlohmann::json::object();
            if (!options.is_object()) {
                options = nlohmann::json::object();
            }

            SpawnResult result = manager->spawn(module, options.dump());
            if (!result.success) {
                std::string code = "spawn_failed";
                if (result.error_message.find("timeout") != std::string::npos) {
                    code = "spawn_timeout";
                } else if (result.error_message.find("on_init failed") !=
                           std::string::npos) {
                    code = "init_failed";
                }
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(
                    make_error(state, std::move(code), result.error_message));
                return results;
            }

            // Return ServiceHandle userdata instead of string
            ServiceHandle handle(result.service_id);
            results.push_back(sol::make_object(lua, handle));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    shield.set_function("exit", [manager](sol::optional<std::string> reason) {
        if (manager) {
            manager->request_current_exit(reason.value_or("normal"));
        }
    });

    shield.set_function(
        "self", [manager](sol::this_state state) -> sol::object {
            sol::state_view lua(state);
            if (!manager) {
                return sol::make_object(lua, sol::nil);
            }
            const auto service_id = manager->current_service_id();
            if (service_id.empty()) {
                return sol::make_object(lua, sol::nil);
            }
            ServiceHandle handle(service_id);
            return sol::make_object(lua, handle);
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

    shield.set_function(
        "query",
        [manager](sol::this_state state,
                  std::string name) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(
                    make_error(state, "runtime_unavailable",
                               "Lua service manager is not available"));
                return results;
            }

            const auto service = manager->query_service(name);
            if (!service.empty()) {
                ServiceHandle handle(service);
                results.push_back(sol::make_object(lua, handle));
                results.push_back(sol::make_object(lua, sol::nil));
                return results;
            }

            results.push_back(sol::make_object(lua, sol::nil));
            results.push_back(make_error(state, "service_not_found",
                                         "service not found: " + name));
            return results;
        });

    shield.set_function(
        "register",
        [manager](sol::this_state state,
                  std::string name) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(
                    make_error(state, "runtime_unavailable",
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
                        [manager](sol::this_state state,
                                  std::string name) -> sol::variadic_results {
                            sol::state_view lua(state);
                            sol::variadic_results results;
                            if (!manager) {
                                results.push_back(sol::make_object(lua, false));
                                results.push_back(make_error(
                                    state, "runtime_unavailable",
                                    "Lua service manager is not available"));
                                return results;
                            }

                            std::string error;
                            if (!manager->unregister_name(name, &error)) {
                                results.push_back(sol::make_object(lua, false));
                                results.push_back(make_error(
                                    state, "unregister_failed", error));
                                return results;
                            }

                            results.push_back(sol::make_object(lua, true));
                            results.push_back(sol::make_object(lua, sol::nil));
                            return results;
                        });
}

void register_message_api(sol::table& shield, LuaServiceManager* manager,
                          LuaRuntime* runtime) {
    shield.set_function(
        "send",
        [manager](sol::this_state state, sol::object target, std::string method,
                  sol::variadic_args args) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(
                    make_error(state, "runtime_unavailable",
                               "Lua service manager is not available"));
                return results;
            }

            std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(
                    make_error(state, "invalid_target",
                               "target must be ServiceHandle or string"));
                return results;
            }

            std::string error;
            if (!manager->send(target_id, method, variadic_to_json_array(args),
                               &error)) {
                // Map error message to stable error code.
                std::string code = "service_not_found";
                bool retryable = false;
                if (error.find("mailbox full") != std::string::npos) {
                    code = "mailbox_full";
                    retryable = true;
                } else if (error.find("runtime is stopping") !=
                           std::string::npos) {
                    code = "runtime_stopping";
                } else if (error.find("message too large") !=
                           std::string::npos) {
                    code = "message_too_large";
                } else if (error.find("unsupported") != std::string::npos) {
                    code = "encode_failed";
                } else if (error.find("permission denied") !=
                           std::string::npos) {
                    code = "permission_denied";
                } else if (error.find("invalid method") != std::string::npos) {
                    code = "invalid_method";
                } else if (error.find("service dead") != std::string::npos) {
                    code = "service_dead";
                } else if (error.find("coroutine limit") != std::string::npos) {
                    code = "coroutine_limit";
                }
                results.push_back(sol::make_object(lua, false));
                results.push_back(
                    make_error(state, std::move(code), error, retryable));
                return results;
            }

            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    shield.set_function(
        "_sync_call",
        [manager, runtime](sol::this_state state, sol::object target,
                           std::string method,
                           sol::variadic_args args) -> sol::variadic_results {
            std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                sol::state_view lua(state);
                sol::variadic_results results;
                results.push_back(sol::make_object(lua, false));
                results.push_back(
                    make_error(state, "invalid_target",
                               "target must be ServiceHandle or string"));
                return results;
            }
            return call_with_timeout(state, manager, runtime, 5000, target_id,
                                     method, variadic_to_json_array(args));
        });

    shield.set_function(
        "_sync_call_timeout",
        [manager, runtime](sol::this_state state, int timeout_ms,
                           sol::object target, std::string method,
                           sol::variadic_args args) -> sol::variadic_results {
            std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                sol::state_view lua(state);
                sol::variadic_results results;
                results.push_back(sol::make_object(lua, false));
                results.push_back(
                    make_error(state, "invalid_target",
                               "target must be ServiceHandle or string"));
                return results;
            }
            return call_with_timeout(state, manager, runtime, timeout_ms,
                                     target_id, method,
                                     variadic_to_json_array(args));
        });

    // Coroutine-aware call primitive. Suspends the caller's coroutine and
    // sends a call-request message to the target; the caller is resumed with
    // [ok, values...] when the callee completes (or on timeout). Returns the
    // session id (0 if the request could not be queued).
    shield.set_function(
        "_coro_call",
        [manager](sol::this_state state, sol::object target, std::string method,
                  sol::table args, int timeout_ms) -> uint64_t {
            if (!manager) {
                return 0;
            }
            const std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                return 0;
            }
            const std::string service_id = manager->query_service(target_id);
            if (service_id.empty()) {
                return 0;
            }
            lua_State* co = state;
            const uint64_t session = manager->suspend_for_call(co, timeout_ms);

            // Build and queue the call-request message.
            nlohmann::json json_args = nlohmann::json::array();
            for (std::size_t i = 1; i <= args.size(); ++i) {
                json_args.push_back(lua_to_json(args[static_cast<int>(i)]));
            }
            std::string send_error;
            // Send carries call_session so the callee's dispatch can route the
            // response back. call_reply_to is the caller's service id.
            if (!manager->send_call_request(service_id, method, json_args,
                                            session, &send_error)) {
                // Could not queue: cancel the pending wait with an error so
                // the caller resumes immediately instead of hanging.
                nlohmann::json err;
                err = send_error;
                manager->resume_caller(session, false,
                                       nlohmann::json::array({err}));
                return 0;
            }
            return session;
        });

    shield.set_function("_is_in_exit", [manager]() -> bool {
        return manager && manager->is_in_exit();
    });

    shield.set_function("sender", [manager]() -> sol::optional<std::string> {
        if (!manager) {
            return sol::nullopt;
        }
        // Returns nil in timer/fork context (no sender).
        // Returns nil outside any dispatch (module-level code).
        // The distinction between "no sender" and "context_expired" is
        // that in timer/fork context we ARE inside a dispatch scope but
        // the sender is empty; outside any scope the context is expired.
        const auto sender = manager->current_sender_id();
        if (sender.empty()) {
            return sol::nullopt;
        }
        return sender;
    });

    shield.set_function("trace", [manager]() -> sol::optional<std::string> {
        if (!manager) return sol::nullopt;
        const auto trace = manager->current_trace_id();
        if (trace.empty()) return sol::nullopt;
        return trace;
    });

    shield.set_function("deadline", [manager]() -> sol::optional<int64_t> {
        if (!manager) return sol::nullopt;
        const auto dl = manager->current_deadline_ms();
        if (dl <= 0) return sol::nullopt;
        return dl;
    });

    // Override shield.call / shield.call_timeout with Lua wrappers that pick
    // the coroutine-aware path when running inside a handler coroutine (so the
    // caller yields instead of blocking the worker) and fall back to the
    // synchronous path on the main thread.
    sol::state_view lua(shield.lua_state());
    lua["shield"] = shield;
    lua.safe_script(
        "shield.call = function(target, method, ...)\n"
        "  if shield._is_in_exit() then\n"
        "    return false, {code='api_not_allowed_in_exit', "
        "message='shield.call is not allowed in on_exit'}\n"
        "  end\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then return shield._sync_call(target, method, ...) end\n"
        "  local session = shield._coro_call(target, method, table.pack(...), "
        "5000)\n"
        "  if session == 0 then return shield._sync_call(target, method, ...) "
        "end\n"
        "  local r = table.pack(coroutine.yield())\n"
        "  if not r[1] then return false, r[2] end\n"
        "  return true, table.unpack(r, 2, r.n)\n"
        "end\n"
        "shield.call_timeout = function(timeout_ms, target, method, ...)\n"
        "  if shield._is_in_exit() then\n"
        "    return false, {code='api_not_allowed_in_exit', "
        "message='shield.call_timeout is not allowed in on_exit'}\n"
        "  end\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then return shield._sync_call_timeout(timeout_ms, target, "
        "method, ...) end\n"
        "  local session = shield._coro_call(target, method, table.pack(...), "
        "timeout_ms)\n"
        "  if session == 0 then return shield._sync_call_timeout(timeout_ms, "
        "target, method, ...) end\n"
        "  local r = table.pack(coroutine.yield())\n"
        "  if not r[1] then return false, r[2] end\n"
        "  return true, table.unpack(r, 2, r.n)\n"
        "end",
        [](lua_State*, sol::protected_function_result pfr)
            -> sol::protected_function_result { return pfr; });
}

sol::variadic_results call_with_timeout(sol::this_state state,
                                        LuaServiceManager* manager,
                                        LuaRuntime* runtime, int timeout_ms,
                                        const std::string& target,
                                        const std::string& method,
                                        const nlohmann::json& args) {
    sol::state_view lua(state);
    sol::variadic_results results;

    // Helper to map call error message to stable error code.
    auto call_error_code = [](const std::string& msg) -> std::string {
        if (msg.find("service not found") != std::string::npos)
            return "service_not_found";
        if (msg.find("service dead") != std::string::npos)
            return "service_dead";
        if (msg.find("method not found") != std::string::npos)
            return "method_not_found";
        if (msg.find("runtime is stopping") != std::string::npos)
            return "runtime_stopping";
        if (msg.find("invalid method") != std::string::npos)
            return "invalid_method";
        if (msg.find("coroutine limit") != std::string::npos)
            return "coroutine_limit";
        return "handler_error";
    };

    if (!manager) {
        results.push_back(sol::make_object(lua, false));
        results.push_back(make_error(state, "runtime_unavailable",
                                     "Lua service manager is not available"));
        return results;
    }

    CallResult result = manager->call(target, method, args, timeout_ms);
    if (!result.success) {
        results.push_back(sol::make_object(lua, false));
        results.push_back(make_error(state,
                                     call_error_code(result.error_message),
                                     result.error_message));
        return results;
    }

    results.push_back(sol::make_object(lua, true));
    for (const auto& value : result.values) {
        results.push_back(json_to_lua(lua, value));
    }
    return results;
}

void register_timer_api(sol::table& shield, LuaServiceManager* manager,
                        LuaRuntime* runtime) {
    shield.set_function("now", []() -> int64_t {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now)
            .count();
    });

    shield.set_function(
        "timer_once",
        [manager, runtime](int delay_ms,
                           sol::function callback) -> sol::variadic_results {
            sol::variadic_results results;
            sol::state_view lua(callback.lua_state());

            if (!runtime) {
                // Fallback to thread-based implementation
                static std::atomic<uint64_t> timer_id{1};
                const uint64_t id = timer_id.fetch_add(1);
                std::thread([delay_ms, callback]() {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(delay_ms));
                    callback();
                }).detach();
                results.push_back(sol::make_object(lua, id));
                return results;
            }

            // Get current service ID
            std::string service_id;
            if (manager) {
                service_id = manager->current_service_id();
            }

            // Check timer limit.
            if (runtime->timer_manager().active_count() >= kTimerLimit) {
                results.push_back(sol::make_object(lua, sol::nil));
                sol::this_state ts(callback.lua_state());
                results.push_back(
                    make_error(ts, "timer_limit", "timer limit reached"));
                return results;
            }

            const uint64_t id = runtime->timer_manager().schedule_once(
                delay_ms, callback, service_id);
            results.push_back(sol::make_object(lua, id));
            return results;
        });

    shield.set_function(
        "timer",
        [manager, runtime](int interval_ms,
                           sol::function callback) -> sol::variadic_results {
            sol::variadic_results results;
            sol::state_view lua(callback.lua_state());

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
                results.push_back(sol::make_object(lua, id));
                return results;
            }

            // Get current service ID
            std::string service_id;
            if (manager) {
                service_id = manager->current_service_id();
            }

            // Check timer limit.
            if (runtime->timer_manager().active_count() >= kTimerLimit) {
                results.push_back(sol::make_object(lua, sol::nil));
                sol::this_state ts(callback.lua_state());
                results.push_back(
                    make_error(ts, "timer_limit", "timer limit reached"));
                return results;
            }

            const uint64_t id = runtime->timer_manager().schedule_fixed_delay(
                interval_ms, callback, service_id);
            results.push_back(sol::make_object(lua, id));
            return results;
        });

    shield.set_function(
        "cancel_timer",
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
                results.push_back(
                    make_error(state, "timer_not_found",
                               "Timer not found or already completed"));
            } else {
                results.push_back(sol::make_object(lua, sol::nil));
            }
            return results;
        });

    // shield.sleep is implemented as a Lua wrapper that schedules a native
    // timer to resume the current coroutine and then yields. The C primitive
    // _resume_after anchors the running coroutine against GC and arms the
    // timer; coroutine.yield suspends until the timer fires and resumes us.
    shield.set_function(
        "_resume_after",
        [manager, runtime](sol::this_state state, int delay_ms) {
            if (!runtime) {
                return;
            }
            if (delay_ms < 0) {
                delay_ms = 0;
            }
            lua_State* co = state;  // current coroutine thread
            // Anchor the thread so it survives GC while suspended.
            lua_pushthread(co);
            const int ref = luaL_ref(co, LUA_REGISTRYINDEX);
            std::string service_id;
            if (manager) {
                service_id = manager->current_service_id();
            }
            auto& timer_mgr = runtime->timer_manager();
            timer_mgr.schedule_once_fn(
                delay_ms,
                [co, ref, manager]() {
                    int nres = 0;
                    const int status = lua_resume(co, nullptr, 0, &nres);
                    if (status == LUA_YIELD) {
                        // Yielded again (e.g. another sleep/call): the API
                        // that yielded has already anchored the coroutine for
                        // its own resume source, so release this sleep anchor.
                        luaL_unref(co, LUA_REGISTRYINDEX, ref);
                        return;
                    }
                    // If this coroutine was servicing a call request that
                    // yielded (e.g. the callee slept), route the response now
                    // that it has completed. No-op for plain handlers.
                    if (status == LUA_OK && manager) {
                        nlohmann::json returns = nlohmann::json::array();
                        for (int i = 0; i < nres; ++i) {
                            sol::stack_object so(sol::state_view(co), i + 1);
                            returns.push_back(lua_to_json(so));
                        }
                        manager->on_handler_completed(co, returns);
                    }
                    // LUA_OK (completed) or an error: release the anchor.
                    luaL_unref(co, LUA_REGISTRYINDEX, ref);
                },
                service_id);
        });

    // Blocking fallback used when shield.sleep is invoked outside any
    // coroutine (e.g. from the synchronous manager.call path). The
    // coroutine-aware branch below handles the yieldable case.
    shield.set_function("_block_sleep", [](int delay_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    });

    sol::state_view lua(shield.lua_state());
    lua["shield"] = shield;
    // Define shield.sleep in Lua so it can use coroutine.yield natively. When
    // not running inside a coroutine (synchronous dispatch) fall back to a
    // blocking sleep so the call still completes.
    lua.safe_script(
        "shield.sleep = function(ms)\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then\n"
        "    shield._block_sleep(ms)\n"
        "  else\n"
        "    shield._resume_after(ms); coroutine.yield()\n"
        "  end\n"
        "end",
        [](lua_State*, sol::protected_function_result pfr)
            -> sol::protected_function_result { return pfr; });
}

void register_task_api(sol::table& shield, LuaServiceManager* manager,
                       LuaRuntime* runtime) {
    (void)runtime;

    shield.set_function(
        "fork",
        [manager](sol::this_state state,
                  sol::function fn) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, sol::nil));
                return results;
            }
            const std::string service_id = manager->current_service_id();

            // Check fork limit.
            if (manager->pending_task_count(service_id) >= kForkLimit) {
                results.push_back(sol::make_object(lua, sol::nil));
                sol::this_state ts(fn.lua_state());
                results.push_back(
                    make_error(ts, "fork_limit", "fork limit reached"));
                return results;
            }
            // Capture the Lua function with its owning state_view. Execution
            // happens on the worker thread; since the worker is the only thread
            // touching Lua VMs once running, this is race-free.
            uint64_t task_id = manager->enqueue_forked_task(
                service_id,
                [fn]() {
                    try {
                        fn();
                    } catch (const std::exception& e) {
                        auto& log = shield::log::get_logger("lua");
                        SHIELD_LOG_ERROR(
                            log, std::string("task error: ") + e.what());
                    }
                },
                fn);  // raw_fn for coroutine wrapping
            results.push_back(sol::make_object(lua, task_id));
            return results;
        });
}

void register_config_api(sol::table& shield) {
    shield.set_function(
        "config",
        [](sol::this_state state, std::string key,
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

            // Require the whole string to be consumed so that "12abc"
            // doesn't get partially parsed as 12.  When the string
            // contains a decimal point or exponent marker, always parse
            // as double first to avoid truncation (e.g. "3.14" → 3).
            const bool looks_like_float =
                value.find('.') != std::string::npos ||
                value.find('e') != std::string::npos ||
                value.find('E') != std::string::npos;

            if (looks_like_float) {
                try {
                    size_t pos = 0;
                    const double parsed = std::stod(value, &pos);
                    if (pos == value.size()) {
                        return sol::make_object(lua, parsed);
                    }
                } catch (const std::exception&) {
                }
            } else {
                try {
                    size_t pos = 0;
                    const long long parsed = std::stoll(value, &pos);
                    if (pos == value.size()) {
                        return sol::make_object(lua, parsed);
                    }
                } catch (const std::exception&) {
                }

                try {
                    size_t pos = 0;
                    const double parsed = std::stod(value, &pos);
                    if (pos == value.size()) {
                        return sol::make_object(lua, parsed);
                    }
                } catch (const std::exception&) {
                }
            }

            return sol::make_object(lua, value);
        });
}

void register_log_api(sol::table& shield, LuaServiceManager* manager) {
    auto& log = shield::log::get_logger("lua");
    sol::state_view lua(shield.lua_state());
    auto log_table = lua.create_table();

    // Helper: build log message with service context prefix.
    auto build_msg = [manager](sol::object value) -> std::string {
        std::string msg = lua_to_json(value).dump();
        if (!manager) return msg;
        const std::string sid = manager->current_service_id();
        if (sid.empty()) return msg;
        return "[" + sid + "] " + msg;
    };

    log_table.set_function("debug", [&log, build_msg](sol::object value) {
        SHIELD_LOG_DEBUG(log, build_msg(value));
    });
    log_table.set_function("info", [&log, build_msg](sol::object value) {
        SHIELD_LOG_INFO(log, build_msg(value));
    });
    log_table.set_function("warn", [&log, build_msg](sol::object value) {
        SHIELD_LOG_WARNING(log, build_msg(value));
    });
    log_table.set_function("error", [&log, build_msg](sol::object value) {
        SHIELD_LOG_ERROR(log, build_msg(value));
    });

    shield["log"] = log_table;
}

void register_shield_api(LuaRuntime& runtime) { (void)runtime; }

namespace api {

using LuaRuntime = shield::lua::LuaRuntime;
using LuaServiceManager = shield::lua::LuaServiceManager;

void register_service_api(LuaRuntime& runtime) { (void)runtime; }

void register_message_api(LuaRuntime& runtime) { (void)runtime; }

void register_timer_api(LuaRuntime& runtime) { (void)runtime; }

void register_timer_api(sol::table& shield, LuaServiceManager* manager,
                        LuaRuntime* runtime) {
    (void)shield;
    (void)manager;
    (void)runtime;
}

void register_task_api(LuaRuntime& runtime) { (void)runtime; }

void register_config_api(LuaRuntime& runtime) { (void)runtime; }

void register_log_api(LuaRuntime& runtime) { (void)runtime; }

void register_gateway_api(LuaRuntime& runtime) { (void)runtime; }

}  // namespace api

// SessionHandle: a Lua userdata representing a network session.
// This is the base registration; actual session objects are created by
// shield_net and passed to gateway handlers.
struct SessionHandle {
    std::weak_ptr<shield::net::Session> session;
    std::string id;
    std::string remote_address;

    SessionHandle() = default;
    SessionHandle(std::weak_ptr<shield::net::Session> s, std::string sid,
                  std::string addr)
        : session(std::move(s)),
          id(std::move(sid)),
          remote_address(std::move(addr)) {}

    std::shared_ptr<shield::net::Session> resolve() {
        if (auto live = session.lock()) {
            return live;
        }

        auto live = resolve_session_handle(id);
        if (live) {
            session = live;
        }
        return live;
    }
};

static void register_session_handle(sol::state& lua) {
    lua.set_function(
        "__shield_make_session_handle",
        [](std::string id, std::string remote_addr) {
            return SessionHandle{
                resolve_session_handle(id), std::move(id),
                std::move(remote_addr)};
        });
    lua.new_usertype<SessionHandle>(
        "SessionHandle", sol::no_constructor, "id",
        [](const SessionHandle& s) { return s.id; }, "remote_addr",
        [](const SessionHandle& s) { return s.remote_address; }, "send",
        [](SessionHandle& s, sol::object payload) -> sol::variadic_results {
            sol::variadic_results results;
            sol::state_view sv(payload.lua_state());
            auto session = s.resolve();
            if (!session || !session->is_alive()) {
                results.push_back(sol::make_object(sv, false));
                sol::table err = sv.create_table();
                err["code"] = "session_closed";
                err["message"] = "session is closed";
                results.push_back(sol::make_object(sv, err));
                return results;
            }

            if (session->has_protocol_pipeline()) {
                shield::transport::DecodedBody message;
                const auto codec_name =
                    std::string(session->protocol_codec_name());
                const bool raw_protocol = codec_name == "raw";
                if (payload.is<sol::table>()) {
                    message.message = lua_table_to_json(payload.as<sol::table>());
                } else if (raw_protocol && payload.is<std::string>()) {
                    const auto value = payload.as<std::string>();
                    message.bytes.assign(value.begin(), value.end());
                } else {
                    auto json = lua_to_json(payload);
                    if (json.is_object() || json.is_array()) {
                        message.message = std::move(json);
                    } else if (raw_protocol && json.is_string()) {
                        const auto value = json.get<std::string>();
                        message.bytes.assign(value.begin(), value.end());
                    } else if (raw_protocol) {
                        const auto serialized = json.dump();
                        message.bytes.assign(serialized.begin(),
                                             serialized.end());
                    } else {
                        results.push_back(sol::make_object(sv, false));
                        sol::table err = sv.create_table();
                        err["code"] = "protocol_message_required";
                        err["message"] =
                            "structured protocol session expects table/object payload";
                        results.push_back(sol::make_object(sv, err));
                        return results;
                    }
                }

                std::string send_error;
                if (!session->send_message(message, &send_error)) {
                    results.push_back(sol::make_object(sv, false));
                    sol::table err = sv.create_table();
                    err["code"] = "protocol_encode_failed";
                    err["message"] =
                        send_error.empty() ? "protocol encode failed"
                                           : send_error;
                    results.push_back(sol::make_object(sv, err));
                    return results;
                }

                results.push_back(sol::make_object(sv, true));
                results.push_back(sol::make_object(sv, sol::nil));
                return results;
            }

            std::vector<std::uint8_t> bytes;
            if (payload.is<std::string>()) {
                const auto value = payload.as<std::string>();
                bytes.assign(value.begin(), value.end());
            } else if (payload.is<sol::table>()) {
                const auto serialized =
                    lua_table_to_json(payload.as<sol::table>()).dump();
                bytes.assign(serialized.begin(), serialized.end());
            } else {
                auto json = lua_to_json(payload);
                if (json.is_string()) {
                    const auto value = json.get<std::string>();
                    bytes.assign(value.begin(), value.end());
                } else {
                    const auto serialized = json.dump();
                    bytes.assign(serialized.begin(), serialized.end());
                }
            }

            if (!session->send(bytes)) {
                results.push_back(sol::make_object(sv, false));
                sol::table err = sv.create_table();
                const auto error_code = session->error_code();
                err["code"] =
                    error_code.empty() ? "session_send_failed" : error_code;
                err["message"] = error_code.empty()
                                     ? "session send failed"
                                     : ("session send failed: " + error_code);
                err["retryable"] =
                    (error_code == "session_send_queue_full");
                results.push_back(sol::make_object(sv, err));
                return results;
            }
            results.push_back(sol::make_object(sv, true));
            results.push_back(sol::make_object(sv, sol::nil));
            return results;
        },
        "close",
        [](SessionHandle& s, sol::optional<std::string> reason) {
            if (auto session = s.resolve()) {
                session->close(reason.value_or("normal"));
            }
        });
}

nlohmann::json make_session_handle_json(
    const std::shared_ptr<shield::net::Session>& session) {
    if (!session) {
        return nullptr;
    }
    remember_session_handle(session);
    return nlohmann::json::object(
        {{std::string(kSessionHandleMarker), true},
         {"id", std::to_string(session->id())},
         {"remote_addr", session->remote_addr().to_string()}});
}

#ifdef SHIELD_ENABLE_CLUSTER
void register_cluster_api(sol::table& shield, LuaServiceManager* manager) {
    sol::state_view lua(shield.lua_state());
    auto cluster = lua.create_table();

    // shield.cluster.query(node_id, service_name) -> service_id or nil, error
    cluster.set_function(
        "query",
        [](sol::this_state state, std::string node_id,
           std::string service_name) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;

            auto* cluster_manager = shield::cluster::global_cluster_manager();
            if (!cluster_manager) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(make_error(state, "module_unavailable",
                                             "shield_cluster is not enabled"));
                return results;
            }

            const auto reachable =
                cluster_manager->check_node_reachable(node_id);
            if (!reachable.empty()) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(
                    make_error(state, reachable,
                               "cluster node is not reachable: " + node_id));
                return results;
            }

            auto service_id =
                cluster_manager->query_remote(node_id, service_name);
            if (service_id.empty()) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(
                    make_error(state, "service_not_found",
                               "remote service not found: " + service_name));
                return results;
            }

            results.push_back(sol::make_object(lua, service_id));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    // shield.cluster.nodes() -> table of node info
    cluster.set_function("nodes", [](sol::this_state state) -> sol::table {
        sol::state_view lua(state);
        sol::table nodes = lua.create_table();
        auto* cluster_manager = shield::cluster::global_cluster_manager();
        if (!cluster_manager) {
            return nodes;
        }
        int index = 1;
        for (const auto& node : cluster_manager->nodes()) {
            sol::table entry = lua.create_table();
            entry["node_id"] = node.node_id;
            entry["address"] = node.address;
            entry["state"] = shield::cluster::node_state_name(node.state);
            entry["last_heartbeat_ms"] = node.last_heartbeat_ms;
            entry["connected_at_ms"] = node.connected_at_ms;
            entry["epoch"] = node.epoch;
            nodes[index++] = entry;
        }
        return nodes;
    });

    // shield.cluster.node_id() -> this node's ID
    cluster.set_function(
        "node_id", [](sol::this_state state) -> sol::optional<std::string> {
            auto* cluster_manager = shield::cluster::global_cluster_manager();
            if (!cluster_manager || cluster_manager->node_id().empty()) {
                return sol::nullopt;
            }
            return cluster_manager->node_id();
        });

    cluster.set_function("node_epoch", []() -> uint64_t {
        auto* cluster_manager = shield::cluster::global_cluster_manager();
        return cluster_manager ? cluster_manager->node_epoch() : 0;
    });

    shield["cluster"] = cluster;
}
#endif

void register_http_api(sol::table& shield) {
    sol::state_view lua(shield.lua_state());

    // =========================================================================
    // shield.http — HTTP 客户端（发请求）
    // =========================================================================
    auto http = lua.create_table();

    // Helper: convert HttpClientResponse to Lua table.
    // Auto-parses JSON body into `data` field when Content-Type is JSON.
    auto to_table =
        [](sol::state_view lua,
           const shield::net::HttpClientResponse& res) -> sol::table {
        sol::table result = lua.create_table();
        result["status"] = res.status_code;
        result["body"] = res.body;
        result["ok"] = res.ok();
        result["error"] = res.error;
        sol::table headers = lua.create_table();
        for (const auto& [k, v] : res.headers) {
            headers[k] = v;
        }
        result["headers"] = headers;

        // Auto-parse JSON response body into `data` field.
        if (!res.body.empty()) {
            auto ct_it = res.headers.find("Content-Type");
            if (ct_it == res.headers.end())
                ct_it = res.headers.find("content-type");
            bool is_json = false;
            if (ct_it != res.headers.end()) {
                is_json =
                    ct_it->second.find("application/json") != std::string::npos;
            }
            // Also try parsing if body starts with { or [
            if (!is_json && !res.body.empty() &&
                (res.body[0] == '{' || res.body[0] == '[')) {
                is_json = true;
            }
            if (is_json) {
                try {
                    auto json = nlohmann::json::parse(res.body);
                    result["data"] = json_to_lua(lua, json);
                } catch (...) {
                    // Not valid JSON, leave data as nil.
                }
            }
        }
        return result;
    };

    // Helper: parse options table into HttpClientOptions.
    auto parse_opts = [](sol::table opts,
                         shield::net::HttpClientOptions& options) {
        if (opts["method"].valid()) {
            options.method = opts["method"].get<std::string>();
        }
        if (opts["body"].valid()) {
            options.body = opts["body"].get<std::string>();
        }
        if (opts["timeout"].valid()) {
            options.timeout_seconds = opts["timeout"].get<int>();
        }
        if (opts["headers"].valid()) {
            sol::table hdrs = opts["headers"];
            for (auto& [k, v] : hdrs) {
                if (k.is<std::string>() && v.is<std::string>()) {
                    options.headers[k.as<std::string>()] = v.as<std::string>();
                }
            }
        }
        if (opts["auth_bearer"].valid()) {
            options.auth_bearer = opts["auth_bearer"].get<std::string>();
            options.headers["Authorization"] = "Bearer " + options.auth_bearer;
        }
        if (opts["auth_basic"].valid()) {
            sol::table basic = opts["auth_basic"];
            if (basic["user"].valid()) {
                options.auth_basic_user = basic["user"].get<std::string>();
            }
            if (basic["password"].valid()) {
                options.auth_basic_password =
                    basic["password"].get<std::string>();
            }
        }
        if (opts["proxy"].valid()) {
            options.proxy = opts["proxy"].get<std::string>();
        }
        if (opts["verify_ssl"].valid()) {
            options.verify_ssl = opts["verify_ssl"].get<bool>();
        }
        if (opts["ca_cert_path"].valid()) {
            options.ca_cert_path = opts["ca_cert_path"].get<std::string>();
        }
        if (opts["retry"].valid()) {
            options.retry_count = opts["retry"].get<int>();
        }
        if (opts["retry_delay"].valid()) {
            options.retry_delay_ms = opts["retry_delay"].get<int>();
        }
        if (opts["follow_redirects"].valid()) {
            options.follow_redirects = opts["follow_redirects"].get<bool>();
        }
        if (opts["max_redirects"].valid()) {
            options.max_redirects = opts["max_redirects"].get<int>();
        }
    };

    // shield.http.request(url, options) -> response_table
    // Full options: method, body, headers, timeout, auth_bearer,
    //   auth_basic={user,password}, proxy, verify_ssl, retry, retry_delay,
    //   follow_redirects, max_redirects
    http.set_function(
        "request",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);

            shield::net::HttpClientOptions options;
            options.url = url;

            if (opts) {
                parse_opts(*opts, options);
            }

            auto res = shield::net::HttpClient::request(options);
            return to_table(lua, res);
        });

    // Convenience: shield.http.get(url [, options]) -> response_table
    http.set_function(
        "get",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "GET";
            options.url = url;
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.post(url [, body] [, options]) -> response_table
    http.set_function(
        "post",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::optional<std::string> body,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "POST";
            options.url = url;
            options.body = body.value_or("");
            options.headers["Content-Type"] = "application/json";
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.put(url [, body] [, options]) -> response_table
    http.set_function(
        "put",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::optional<std::string> body,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PUT";
            options.url = url;
            options.body = body.value_or("");
            options.headers["Content-Type"] = "application/json";
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.delete(url [, options]) -> response_table
    http.set_function(
        "delete",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "DELETE";
            options.url = url;
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.patch(url [, body] [, options]) ->
    // response_table
    http.set_function(
        "patch",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::optional<std::string> body,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PATCH";
            options.url = url;
            options.body = body.value_or("");
            options.headers["Content-Type"] = "application/json";
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // =========================================================================
    // JSON 便捷方法：自动将 Lua table 序列化为 JSON，自动设置 Content-Type
    // 响应自动解析 JSON 到 result.data 字段
    // =========================================================================

    // shield.http.json(url, data [, options]) -> response_table
    // 通用 JSON POST（最常用场景）
    http.set_function(
        "json",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::object data,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "POST";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            // Serialize Lua table/object to JSON string.
            options.body = lua_table_to_json(data.as<sol::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.json_post(url, data [, options]) -> response_table
    http.set_function(
        "json_post",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::object data,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "POST";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            options.body = lua_table_to_json(data.as<sol::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.json_put(url, data [, options]) -> response_table
    http.set_function(
        "json_put",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::object data,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PUT";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            options.body = lua_table_to_json(data.as<sol::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.json_patch(url, data [, options]) -> response_table
    http.set_function(
        "json_patch",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::object data,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PATCH";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            options.body = lua_table_to_json(data.as<sol::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.upload(url, files [, fields] [, timeout]) -> response_table
    // files: array of {field_name, file_path, content_type}
    // fields: table of form field key-value pairs
    http.set_function(
        "upload",
        [&to_table](sol::this_state state, std::string url, sol::table files,
                    sol::optional<sol::table> fields,
                    sol::optional<int> timeout) -> sol::table {
            sol::state_view lua(state);

            std::vector<shield::net::HttpFileField> file_list;
            for (auto& [i, entry] : files) {
                if (entry.is<sol::table>()) {
                    sol::table f = entry.as<sol::table>();
                    shield::net::HttpFileField field;
                    field.field_name =
                        f.get_or<std::string>("field_name", "file");
                    field.file_path = f.get_or<std::string>("file_path", "");
                    field.content_type =
                        f.get_or<std::string>("content_type", "");
                    file_list.push_back(std::move(field));
                }
            }

            std::unordered_map<std::string, std::string> field_map;
            if (fields) {
                for (auto& [k, v] : *fields) {
                    if (k.is<std::string>() && v.is<std::string>()) {
                        field_map[k.as<std::string>()] = v.as<std::string>();
                    }
                }
            }

            auto res = shield::net::HttpClient::upload(
                url, file_list, field_map, timeout.value_or(60));
            return to_table(lua, res);
        });

    // shield.http.download(url, output_path [, timeout]) -> response_table
    http.set_function("download",
                      [&to_table](sol::this_state state, std::string url,
                                  std::string output_path,
                                  sol::optional<int> timeout) -> sol::table {
                          sol::state_view lua(state);
                          auto res = shield::net::HttpClient::download(
                              url, output_path, timeout.value_or(60));
                          return to_table(lua, res);
                      });

    // shield.http.post_form(url, fields [, timeout]) -> response_table
    // fields: table of key-value pairs for application/x-www-form-urlencoded
    http.set_function(
        "post_form",
        [&to_table](sol::this_state state, std::string url, sol::table fields,
                    sol::optional<int> timeout) -> sol::table {
            sol::state_view lua(state);

            std::unordered_map<std::string, std::string> field_map;
            for (auto& [k, v] : fields) {
                if (k.is<std::string>() && v.is<std::string>()) {
                    field_map[k.as<std::string>()] = v.as<std::string>();
                }
            }

            auto res = shield::net::HttpClient::post_form(url, field_map,
                                                          timeout.value_or(10));
            return to_table(lua, res);
        });

    shield["http"] = http;

    // =========================================================================
    // shield.httpd — HTTP 服务端（注册路由处理 incoming 请求）
    // =========================================================================
    auto httpd = lua.create_table();

    // Route registration stubs. Full integration passes the HttpServer
    // instance; these return true to indicate the call was accepted.
    auto register_route = [](sol::this_state state, std::string method,
                             std::string path, sol::function handler) {
        sol::state_view lua(state);
        // TODO: store route in HttpServer instance when integrated with
        // bootstrap
        return sol::make_object(lua, true);
    };

    httpd.set_function("get", [register_route](sol::this_state s, std::string p,
                                               sol::function h) {
        return register_route(s, "GET", std::move(p), std::move(h));
    });
    httpd.set_function(
        "post",
        [register_route](sol::this_state s, std::string p, sol::function h) {
            return register_route(s, "POST", std::move(p), std::move(h));
        });
    httpd.set_function("put", [register_route](sol::this_state s, std::string p,
                                               sol::function h) {
        return register_route(s, "PUT", std::move(p), std::move(h));
    });
    httpd.set_function(
        "delete",
        [register_route](sol::this_state s, std::string p, sol::function h) {
            return register_route(s, "DELETE", std::move(p), std::move(h));
        });
    httpd.set_function(
        "patch",
        [register_route](sol::this_state s, std::string p, sol::function h) {
            return register_route(s, "PATCH", std::move(p), std::move(h));
        });

    shield["httpd"] = httpd;
}

void register_plugin_api(sol::table& shield) {
    sol::state_view lua(shield.lua_state());
    auto plugin = lua.create_table();

    // shield.plugin.packages() -> array of {id, version, kind, provides}
    plugin.set_function("packages", [](sol::this_state state) -> sol::table {
        sol::state_view lua(state);
        auto t = lua.create_table();
        for (const auto& p : shield::plugin::global_host().list_packages()) {
            sol::table row = lua.create_table();
            row["id"] = p.id;
            row["version"] = p.version;
            row["kind"] = p.kind;
            row["docs_url"] = p.docs_url;
            row["docs_description"] = p.docs_description;
            sol::table prov = lua.create_table();
            for (size_t i = 0; i < p.provides.size(); ++i)
                prov[i + 1] = p.provides[i];
            row["provides"] = prov;
            t[t.size() + 1] = row;
        }
        return t;
    });

    // shield.plugin.instances() -> array of {id, package, state, required}
    plugin.set_function("instances", [](sol::this_state state) -> sol::table {
        sol::state_view lua(state);
        auto t = lua.create_table();
        for (const auto& in : shield::plugin::global_host().list_instances()) {
            sol::table row = lua.create_table();
            row["id"] = in.id;
            row["package"] = in.package;
            row["state"] = in.state;
            row["required"] = in.required;
            t[t.size() + 1] = row;
        }
        return t;
    });

    // shield.plugin.instance(id) -> table or nil
    plugin.set_function(
        "instance", [](sol::this_state state, std::string id) -> sol::object {
            sol::state_view lua(state);
            for (const auto& in :
                 shield::plugin::global_host().list_instances()) {
                if (in.id == id) {
                    sol::table row = lua.create_table();
                    row["id"] = in.id;
                    row["package"] = in.package;
                    row["state"] = in.state;
                    row["required"] = in.required;
                    return row;
                }
            }
            return sol::nil;
        });

    // shield.plugin.binding(name) -> {instance_id, interface} or nil
    plugin.set_function(
        "binding", [](sol::this_state state, std::string name) -> sol::object {
            sol::state_view lua(state);
            auto b = shield::plugin::global_host().get_binding(name);
            if (!b) return sol::nil;
            sol::table row = lua.create_table();
            row["instance_id"] = b->instance_id;
            row["interface"] = b->interface_name;
            return row;
        });

    shield["plugin"] = plugin;
}

void register_full_shield_api(sol::state& lua, LuaServiceManager* manager,
                              LuaRuntime* runtime) {
    // Initialize HTTP client (libcurl global state).
    shield::net::HttpClient::initialize();

    // Register usertypes
    ServiceHandle::register_usertype(lua);
    register_session_handle(lua);

    auto shield = lua.create_table();

    register_service_api(shield, manager);
    register_message_api(shield, manager, runtime);
    register_timer_api(shield, manager, runtime);
    register_task_api(shield, manager, runtime);
    register_config_api(shield);
    register_log_api(shield, manager);
    register_http_api(shield);
    register_plugin_api(shield);

#ifdef SHIELD_ENABLE_CLUSTER
    register_cluster_api(shield, manager);
#endif

    lua["shield"] = shield;

    // Coroutine dispatch helper used by
    // LuaRuntime::call_service_method_coroutine. It wraps a handler + args
    // table into a coroutine whose body returns the handler's results, so a
    // yield inside the handler (e.g. shield.sleep) suspends the whole coroutine
    // and a later resume continues transparently. Defined as a global function
    // directly so a script error can't take down register_api
    // (call_service_method_coroutine falls back to sync dispatch if the helper
    // is absent).
    lua.safe_script(
        "function __shield_run_handler(handler, args)\n"
        "  return coroutine.create(function()\n"
        "    return handler(table.unpack(args))\n"
        "  end)\n"
        "end",
        [](lua_State*, sol::protected_function_result pfr)
            -> sol::protected_function_result { return pfr; });
}

}  // namespace shield::lua
