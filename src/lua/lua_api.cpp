// [SHIELD_LUA] Lua API registration
#include "shield/lua/lua_api.hpp"

#include "shield/config/config.hpp"
#ifdef SHIELD_ENABLE_CLUSTER
#include "shield/cluster/cluster_manager.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <caf/actor.hpp>
#include <caf/send.hpp>
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

#include "shield/core/service_message.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/client_identity.hpp"
#include "shield/lua/lua_constants.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/http_client.hpp"
#include "shield/net/session.hpp"
#include "shield/plugin/plugin_host.hpp"

namespace shield::lua {

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

// Map a raw call-failure message to the stable call error code (shared by
// the coroutine call wrapper's string-shaping and the _coro_call pre-dispatch
// failures).
std::string call_error_code_for(const std::string& msg) {
    if (msg.find("service not found") != std::string::npos)
        return "service_not_found";
    if (msg.find("service dead") != std::string::npos) return "service_dead";
    if (msg.find("method not found") != std::string::npos)
        return "method_not_found";
    if (msg.find("runtime is stopping") != std::string::npos)
        return "runtime_stopping";
    if (msg.find("invalid method") != std::string::npos)
        return "invalid_method";
    if (msg.find("coroutine limit") != std::string::npos)
        return "coroutine_limit";
    return "handler_error";
}

// -- Client identity userdata (ClientContext / ClientRef) ---------------------
//
// Both wrap the same trusted identity snapshot (ClientContextData, see
// client_identity.hpp). A ClientContext materializes whenever a
// __shield_client_ref marker arrives in a message payload (gateway ingress,
// on_connect/on_disconnect); a ClientRef is what shield.client.bind returns.
// Lua cannot construct them (sol::no_constructor) and cannot mutate them.
// Passing either as a message argument serializes back to the marker form.

namespace {

// Shared read-only property binding for ClientContext and ClientRef: the
// concrete Box parameter keeps sol's wrapper happy (a generic lambda is not
// convertible to a single function pointer).
template <typename Box>
void bind_identity_properties(sol::usertype<Box>& type) {
    type.set("player_id", [](const Box& box) { return box.data.player_id; });
    type.set("session_id", [](const Box& box) { return box.data.session_id; });
    type.set("session_epoch",
             [](const Box& box) { return box.data.session_epoch; });
    type.set("protocol_profile_id",
             [](const Box& box) { return box.data.protocol_profile_id; });
    type.set("gateway",
             [](const Box& box) { return box.data.gateway_address; });
}

bool extract_client_data(const sol::object& object, ClientContextData* out) {
    if (object.is<ClientContextBox>()) {
        *out = object.as<const ClientContextBox&>().data;
        return true;
    }
    if (object.is<ClientRefBox>()) {
        *out = object.as<const ClientRefBox&>().data;
        return true;
    }
    return false;
}

}  // namespace

// Reads the client identity out of a ClientContext/ClientRef argument (or a
// __shield_client_ref marker table that travelled through a path without
// materialization). Used by shield.client.bind/close and the client_rpc
// egress helpers.
static bool client_arg_to_data(const sol::object& object,
                               ClientContextData* out) {
    if (extract_client_data(object, out)) {
        return true;
    }
    if (object.is<sol::table>()) {
        auto data = ClientContextData::from_json(lua_to_json(object));
        if (data.has_value()) {
            *out = std::move(*data);
            return true;
        }
    }
    return false;
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
        // A trusted client-identity marker materializes as the read-only
        // ClientContext userdata. from_json does all field validation, so a
        // malformed field degrades to its default instead of throwing.
        if (auto ctx = ClientContextData::from_json(value)) {
            sol::object maybe_ud = lua["__shield_make_client_context"];
            if (maybe_ud.valid() && maybe_ud.is<sol::protected_function>()) {
                sol::protected_function make_context =
                    maybe_ud.as<sol::protected_function>();
                auto result = make_context(ctx->session_id, ctx->session_epoch,
                                           ctx->player_id, ctx->gateway_address,
                                           ctx->protocol_profile_id);
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

nlohmann::json variadic_to_json_array(sol::variadic_args args) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& arg : args) {
        values.push_back(lua_to_json(arg));
    }
    return values;
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
    // Synchronous spawn primitive: runs VM creation + on_init on the calling
    // thread. The public shield.spawn wrapper (below) uses this on the main
    // thread, inside a spawn's on_init, and as the fallback when the
    // coroutine path cannot suspend.
    shield.set_function(
        "_sync_spawn",
        [manager](sol::this_state state, std::string module,
                  sol::optional<sol::table> opts) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;

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
        manager->request_current_exit(reason.value_or("normal"));
    });

    // True while a spawn's on_init runs its initial segment on the spawning
    // thread: shield.spawn resolves synchronously there (the child's init
    // blocks the spawner, never a service actor).
    shield.set_function("_in_on_init", [] {
        return LuaServiceManager::spawn_init_in_progress();
    });

    shield.set_function(
        "self", [manager](sol::this_state state) -> sol::object {
            sol::state_view lua(state);
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

    // Coroutine-aware spawn primitive. Suspends the caller's coroutine and
    // queues the blocking part (VM creation + module load + on_init) onto the
    // manager's spawn worker thread; the caller is resumed with
    // [true, service_id] or [false, error_table] when the spawn completes
    // (or on timeout). Returns the session id, or 0 when the coroutine path
    // is unavailable (no dispatch context / runtime stopping) and the caller
    // must fall back to _sync_spawn.
    shield.set_function(
        "_coro_spawn",
        [manager](sol::this_state state, std::string module,
                  sol::optional<sol::table> opts, int timeout_ms) -> uint64_t {
            if (manager->current_service_id().empty()) {
                return 0;
            }
            nlohmann::json options =
                opts ? lua_table_to_json(*opts) : nlohmann::json::object();
            if (!options.is_object()) {
                options = nlohmann::json::object();
            }

            lua_State* co = state;
            const uint64_t session = manager->suspend_for_call(co, timeout_ms);
            if (!manager->enqueue_async_spawn(session, std::move(module),
                                              options.dump())) {
                nlohmann::json err = "runtime is stopping";
                manager->resume_caller(session, false,
                                       nlohmann::json::array({err}));
                return 0;
            }
            return session;
        });

    // Rebuild a ServiceHandle userdata from a service id. Used by the
    // shield.spawn wrapper after a coroutine resume (the response channel
    // carries JSON, not userdata).
    shield.set_function(
        "_make_handle",
        [](sol::this_state state, std::string service_id) -> sol::object {
            sol::state_view lua(state);
            ServiceHandle handle(std::move(service_id));
            return sol::make_object(lua, handle);
        });

    // Business-triggered panic: invoke on_panic(reason, {type="explicit"})
    // and exit the current service with reason "panic".
    shield.set_function("panic", [manager](sol::optional<std::string> reason) {
        manager->panic_current(reason.value_or("explicit panic"));
    });

    // Public shield.spawn: suspend inside handler coroutines (the spawn
    // worker runs on_init off-actor), stay synchronous on the main thread and
    // inside a spawn's on_init (on the spawning thread).
    sol::state_view lua(shield.lua_state());
    lua["shield"] = shield;
    lua.safe_script(
        "shield.spawn = function(module, opts)\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain or shield._in_on_init() then return "
        "shield._sync_spawn(module, opts) end\n"
        "  local timeout = (type(opts) == 'table' and opts.timeout) or 10000\n"
        "  local session = shield._coro_spawn(module, opts, timeout)\n"
        "  if session == 0 then return shield._sync_spawn(module, opts) end\n"
        "  local r = table.pack(coroutine.yield())\n"
        "  if not r[1] then\n"
        "    if type(r[2]) == 'table' and r[2].code == 'timeout' then\n"
        "      r[2].code = 'spawn_timeout'\n"
        "      r[2].message = 'spawn timeout'\n"
        "    end\n"
        "    return nil, r[2]\n"
        "  end\n"
        "  return shield._make_handle(r[2]), nil\n"
        "end\n",
        // The two header lines below are a gcov artifact: the identity adapter
        // body runs (its closing brace is covered) but the opening arc is
        // emitted only into an outlined clone that is never called.
        [](lua_State*, sol::protected_function_result
                           pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
}

#ifdef SHIELD_ENABLE_CLUSTER
namespace {

// Remote-target resolution for shield.send/call (M4). Local names always
// win: "room.public" resolves locally even if a remote route shares the
// name, and a colon-name that hits locally is never reinterpreted as
// "node:service".
struct RemoteResolution {
    bool is_remote = false;
    // Meaningful when is_remote: envelope destination.
    std::string node;
    std::string service_id;
    // Non-empty when the remote target failed pre-flight (reachability /
    // route lookup): the caller fails the send/call with this stable code
    // instead of dispatching.
    std::string error_code;
    std::string error_message;
};

// Node-level failures recover on reconnect: surface them as retryable.
bool remote_error_retryable(const std::string& code) {
    return code == "node_offline" || code == "node_suspect";
}

// Stable code for a send_remote failure string (e.g. the transport's
// "node_offline" out-param).
std::string remote_send_error_code(const std::string& msg) {
    if (msg.find("node_suspect") != std::string::npos) return "node_suspect";
    if (msg.find("node_offline") != std::string::npos) return "node_offline";
    return "transport_failed";
}

RemoteResolution resolve_remote_target(LuaServiceManager* manager,
                                       const std::string& target_id) {
    RemoteResolution res;
    // Local hit wins, never reinterpreted as "node:service".
    if (!manager->query_service(target_id).empty()) {
        return res;
    }
    std::string node, name;
    if (!shield::cluster::ClusterManager::parse_remote_target(target_id, node,
                                                              name)) {
        return res;
    }
    auto* cm = shield::cluster::global_cluster_manager();
    if (!cm) {
        // Cluster not initialized: no node namespace exists; treat as an
        // ordinary local miss so the local path produces service_not_found.
        return res;
    }
    if (node == cm->node_id()) {
        // Self-qualified name: plain local service (already missed above).
        return res;
    }
    res.is_remote = true;
    res.node = node;
    if (const std::string reachable = cm->check_node_reachable(node);
        !reachable.empty()) {
        res.error_code = reachable;
        res.error_message = "cluster node is not reachable: " + node;
        return res;
    }
    res.service_id = cm->query_remote(node, name);
    if (res.service_id.empty()) {
        res.error_code = "service_not_found";
        res.error_message = "remote service not found: " + name + " on " + node;
    }
    return res;
}

}  // namespace
#endif

void register_message_api(sol::table& shield, LuaServiceManager* manager,
                          LuaRuntime* runtime) {
    shield.set_function(
        "send",
        [manager](sol::this_state state, sol::object target, std::string method,
                  sol::variadic_args args) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;

            std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(
                    make_error(state, "invalid_target",
                               "target must be ServiceHandle or string"));
                return results;
            }

#ifdef SHIELD_ENABLE_CLUSTER
            // Cross-node fire-and-forget (M4): "node:service" leaves as an
            // envelope; local names keep the in-process path below.
            const auto remote = resolve_remote_target(manager, target_id);
            if (remote.is_remote) {
                if (!remote.error_code.empty()) {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error(
                        state, remote.error_code, remote.error_message,
                        remote_error_retryable(remote.error_code)));
                    return results;
                }
                std::string send_error;
                auto* cm = shield::cluster::global_cluster_manager();
                if (cm->send_remote(remote.node, remote.service_id, method,
                                    variadic_to_json_array(args).dump(), 0, 0,
                                    &send_error)) {
                    results.push_back(sol::make_object(lua, true));
                    results.push_back(sol::make_object(lua, sol::nil));
                    return results;
                }
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(
                    state, remote_send_error_code(send_error), send_error,
                    remote_error_retryable(
                        remote_send_error_code(send_error))));
                return results;
            }
#endif

            std::string error;
            if (!manager->send(target_id, method, variadic_to_json_array(args),
                               &error)) {
                // Map error message to stable error code.
                std::string code = "service_not_found";
                bool retryable = false;
                if (error.find("runtime is stopping") != std::string::npos) {
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

    // Coroutine-aware call primitive. Suspends the caller's coroutine and
    // sends a call-request message to the target; the caller is resumed with
    // [ok, values...] when the callee completes (or on timeout). Returns the
    // session id (0 if the caller is not inside a coroutine).
    shield.set_function(
        "_coro_call",
        [manager](sol::this_state state, sol::object target, std::string method,
                  sol::table args, int timeout_ms) -> uint64_t {
            // Refuse to suspend the main thread: the Lua wrapper never
            // dispatches off-coroutine, and anchoring the main thread here
            // would leave it suspended with no resume source.
            if (lua_pushthread(state) == 1) {
                lua_pop(state, 1);
                return 0;
            }
            lua_pop(state, 1);
            const std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                // Invalid target shape: suspend, then complete asynchronously
                // so the yielded wrapper gets the stable error table.
                lua_State* co = state;
                const uint64_t session =
                    manager->suspend_for_call(co, timeout_ms);
                manager->complete_call(
                    session, false,
                    nlohmann::json::array({nlohmann::json::object(
                        {{"code", "invalid_target"},
                         {"message", "target must be ServiceHandle or string"},
                         {"retryable", false}})}));
                return session;
            }

            // Pack the arguments once for both dispatch paths.
            std::size_t arg_count = args.size();
            sol::object packed_count = args["n"];
            if (packed_count.valid() && packed_count.is<int>()) {
                const int n = packed_count.as<int>();
                arg_count = n > 0 ? static_cast<std::size_t>(n) : 0;
            }
            nlohmann::json json_args = nlohmann::json::array();
            for (std::size_t i = 1; i <= arg_count; ++i) {
                json_args.push_back(lua_to_json(args[static_cast<int>(i)]));
            }

#ifdef SHIELD_ENABLE_CLUSTER
            // Cross-node call (M4): suspend first, then pre-flight and send;
            // every failure completes the session asynchronously (response
            // message lands after the caller yields) so the wrapper's
            // coroutine.yield() always gets exactly one resume.
            const auto remote = resolve_remote_target(manager, target_id);
            if (remote.is_remote) {
                lua_State* co = state;
                const uint64_t session =
                    manager->suspend_for_call(co, timeout_ms);
                if (!remote.error_code.empty()) {
                    manager->complete_call(
                        session, false,
                        nlohmann::json::array({nlohmann::json::object(
                            {{"code", remote.error_code},
                             {"message", remote.error_message},
                             {"retryable",
                              remote_error_retryable(remote.error_code)}})}));
                    return session;
                }
                std::string send_error;
                auto* cm = shield::cluster::global_cluster_manager();
                if (!cm->send_remote(remote.node, remote.service_id, method,
                                     json_args.dump(), session, timeout_ms,
                                     &send_error)) {
                    const std::string code = remote_send_error_code(send_error);
                    manager->complete_call(
                        session, false,
                        nlohmann::json::array({nlohmann::json::object(
                            {{"code", code},
                             {"message", send_error},
                             {"retryable", remote_error_retryable(code)}})}));
                }
                // Non-zero session: the wrapper yields and the completion
                // (success or the failure above) resumes it.
                return session;
            }
#endif

            const std::string service_id = manager->query_service(target_id);
            if (service_id.empty()) {
                // Unknown local target: suspend, then complete asynchronously
                // (via the caller's own actor mailbox) so the wrapper's
                // coroutine.yield() still gets exactly one resume with the
                // stable error table.
                lua_State* co = state;
                const uint64_t session =
                    manager->suspend_for_call(co, timeout_ms);
                manager->complete_call(
                    session, false,
                    nlohmann::json::array({nlohmann::json::object(
                        {{"code", "service_not_found"},
                         {"message", "service not found: " + target_id},
                         {"retryable", false}})}));
                return session;
            }
            lua_State* co = state;
            const uint64_t session = manager->suspend_for_call(co, timeout_ms);

            // Build and queue the call-request message.
            std::string send_error;
            // Send carries call_session so the callee's dispatch can route the
            // response back to the caller.
            if (!manager->send_call_request(service_id, method, json_args,
                                            session, &send_error)) {
                // Could not queue (e.g. runtime stopping): complete the
                // session through the same async channel so the caller
                // resumes with a stable error table.
                // GCOVR_EXCL_START (race window: the target's actor is gone
                // between the lookup above and this send)
                manager->complete_call(
                    session, false,
                    nlohmann::json::array({nlohmann::json::object(
                        {{"code", call_error_code_for(send_error)},
                         {"message", send_error},
                         {"retryable", false}})}));
                return session;
                // GCOVR_EXCL_STOP
            }
            return session;
        });

    shield.set_function("_is_in_exit",
                        [manager]() -> bool { return manager->is_in_exit(); });

    // Target-shape check for the call wrappers: returns the normalized
    // service id, or nil when the target is neither a ServiceHandle nor a
    // string. Shape errors are programming errors and are reported with
    // invalid_target regardless of the calling context.
    shield.set_function("_call_target_id",
                        [](sol::this_state state,
                           sol::object target) -> sol::optional<std::string> {
                            const std::string id = extract_service_id(target);
                            if (id.empty()) {
                                return sol::nullopt;
                            }
                            return id;
                        });

    // Stable error code for a raw call-failure message (used by the call
    // wrapper to shape non-table resume payloads into {code, message}).
    shield.set_function("_call_error_code",
                        [](sol::optional<std::string> msg) -> std::string {
                            return call_error_code_for(msg.value_or(""));
                        });

    // DEPRECATED: Use ctx.sender instead. Kept for backward compatibility.
    // In new code, prefer: function M.handler(ctx, ...) local src = ctx.sender
    // end
    shield.set_function("sender", [manager]() -> sol::optional<std::string> {
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

    // DEPRECATED: Use ctx.trace instead. Kept for backward compatibility.
    shield.set_function("trace", [manager]() -> sol::optional<std::string> {
        const auto trace = manager->current_trace_id();
        if (trace.empty()) return sol::nullopt;
        return trace;
    });

    // DEPRECATED: Use ctx.deadline instead. Kept for backward compatibility.
    shield.set_function("deadline", [manager]() -> sol::optional<int64_t> {
        const auto dl = manager->current_deadline_ms();
        if (dl <= 0) return sol::nullopt;
        return dl;
    });

    // shield.call / shield.call_timeout are coroutine-only: inside a handler
    // coroutine (handler, timer/fork callback, on_init) the caller suspends
    // until the callee completes; on the main thread — module-level code —
    // they are rejected with a stable error code instead of blocking the
    // worker.
    sol::state_view lua(shield.lua_state());
    lua["shield"] = shield;
    lua.safe_script(
        "shield.call = function(target, method, ...)\n"
        "  if shield._is_in_exit() then\n"
        "    return false, {code='api_not_allowed_in_exit', "
        "message='shield.call is not allowed in on_exit'}\n"
        "  end\n"
        "  if shield._call_target_id(target) == nil then\n"
        "    return false, {code='invalid_target', "
        "message='target must be ServiceHandle or string', retryable=false}\n"
        "  end\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then\n"
        "    return false, {code='call_not_allowed_off_coroutine', "
        "message='shield.call is only allowed inside a coroutine'}\n"
        "  end\n"
        "  local session = shield._coro_call(target, method, table.pack(...), "
        "5000)\n"
        "  if session == 0 then\n"
        "    return false, {code='call_not_allowed_off_coroutine', "
        "message='shield.call is only allowed inside a coroutine'}\n"
        "  end\n"
        "  local r = table.pack(coroutine.yield())\n"
        "  if not r[1] then\n"
        "    if type(r[2]) == 'table' then return false, r[2] end\n"
        "    local msg = tostring(r[2])\n"
        "    return false, { code = shield._call_error_code(msg), message = "
        "msg "
        "}\n"
        "  end\n"
        "  return true, table.unpack(r, 2, r.n)\n"
        "end\n"
        "shield.call_timeout = function(timeout_ms, target, method, ...)\n"
        "  if shield._is_in_exit() then\n"
        "    return false, {code='api_not_allowed_in_exit', "
        "message='shield.call_timeout is not allowed in on_exit'}\n"
        "  end\n"
        "  if shield._call_target_id(target) == nil then\n"
        "    return false, {code='invalid_target', "
        "message='target must be ServiceHandle or string', retryable=false}\n"
        "  end\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then\n"
        "    return false, {code='call_not_allowed_off_coroutine', "
        "message='shield.call_timeout is only allowed inside a coroutine'}\n"
        "  end\n"
        "  local session = shield._coro_call(target, method, table.pack(...), "
        "timeout_ms)\n"
        "  if session == 0 then\n"
        "    return false, {code='call_not_allowed_off_coroutine', "
        "message='shield.call_timeout is only allowed inside a coroutine'}\n"
        "  end\n"
        "  local r = table.pack(coroutine.yield())\n"
        "  if not r[1] then\n"
        "    if type(r[2]) == 'table' then return false, r[2] end\n"
        "    local msg = tostring(r[2])\n"
        "    return false, { code = shield._call_error_code(msg), message = "
        "msg "
        "}\n"
        "  end\n"
        "  return true, table.unpack(r, 2, r.n)\n"
        "end",
        [](lua_State*, sol::protected_function_result
                           pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
}

void register_timer_api(sol::table& shield, LuaServiceManager* manager,
                        LuaRuntime* runtime) {
    shield.set_function(
        "now", [manager]() -> int64_t { return manager->clock_now_ms(); });

    // Real monotonic milliseconds — NOT adjustable, for relative timing
    // (network backoff, heartbeat intervals) in Lua that must stay real.
    shield.set_function("monotonic", []() -> int64_t {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now)
            .count();
    });

    shield.set_function(
        "timer_once",
        [manager](int delay_ms,
                  sol::function callback) -> sol::variadic_results {
            sol::variadic_results results;
            sol::state_view lua(callback.lua_state());

            // Get current service ID
            const std::string service_id = manager->current_service_id();

            // Check timer limit.
            const auto timer_count = manager->active_actor_timer_count();
            if (timer_count >= kTimerLimit) {
                results.push_back(sol::make_object(lua, sol::nil));
                sol::this_state ts(callback.lua_state());
                results.push_back(
                    make_error(ts, "timer_limit", "timer limit reached"));
                return results;
            }

            const uint64_t id = service_id.empty()
                                    ? 0
                                    : manager->schedule_actor_timer_once(
                                          delay_ms, callback, service_id);
            results.push_back(sol::make_object(lua, id));
            return results;
        });

    shield.set_function(
        "timer",
        [manager](int interval_ms,
                  sol::function callback) -> sol::variadic_results {
            sol::variadic_results results;
            sol::state_view lua(callback.lua_state());

            // Get current service ID
            const std::string service_id = manager->current_service_id();

            // Check timer limit.
            const auto timer_count = manager->active_actor_timer_count();
            if (timer_count >= kTimerLimit) {
                results.push_back(sol::make_object(lua, sol::nil));
                sol::this_state ts(callback.lua_state());
                results.push_back(
                    make_error(ts, "timer_limit", "timer limit reached"));
                return results;
            }

            const uint64_t id = service_id.empty()
                                    ? 0
                                    : manager->schedule_actor_timer_fixed_delay(
                                          interval_ms, callback, service_id);
            results.push_back(sol::make_object(lua, id));
            return results;
        });

    shield.set_function(
        "cancel_timer",
        [manager](sol::this_state state, uint64_t id) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;

            const bool cancelled = manager->cancel_actor_timer(id);
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
        "_resume_after", [manager](sol::this_state state, int delay_ms) {
            if (delay_ms < 0) {
                delay_ms = 0;
            }
            lua_State* co = state;  // current coroutine thread
            // Anchor the thread so it survives GC while suspended.
            lua_pushthread(co);
            const int ref = luaL_ref(co, LUA_REGISTRYINDEX);
            const std::string service_id = manager->current_service_id();
            auto resume_fn = [co, ref, manager, service_id]() {
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
                if (status == LUA_OK) {
                    nlohmann::json returns = nlohmann::json::array();
                    for (int i = 0; i < nres; ++i) {
                        sol::stack_object so(sol::state_view(co), i + 1);
                        returns.push_back(lua_to_json(so));
                    }
                    manager->on_handler_completed(co, returns);
                }
                // Terminal segment (LUA_OK or error): honor a shield.exit
                // the continuation requested. During spawn-init the spawn
                // path owns the request instead.
                if (!service_id.empty() &&
                    !LuaServiceManager::spawn_init_in_progress()) {
                    manager->finish_pending_exit(service_id);
                }
                // LUA_OK (completed) or an error: release the anchor.
                luaL_unref(co, LUA_REGISTRYINDEX, ref);
            };
            if (!service_id.empty()) {
                (void)manager->schedule_actor_timer_once_fn(
                    delay_ms, std::move(resume_fn), service_id);
            }
        });

    // Blocking sleep used when shield.sleep is invoked outside any coroutine
    // (e.g. from module-level code). The coroutine-aware branch below handles
    // the yieldable case inside service handlers.
    shield.set_function("_block_sleep", [](int delay_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    });

    sol::state_view lua(shield.lua_state());
    lua["shield"] = shield;
    // Define shield.sleep in Lua so it can use coroutine.yield natively. When
    // not running inside a coroutine, block the calling thread so the call
    // still completes.
    lua.safe_script(
        "shield.sleep = function(ms)\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then\n"
        "    shield._block_sleep(ms)\n"
        "  else\n"
        "    shield._resume_after(ms); coroutine.yield()\n"
        "  end\n"
        "end",
        [](lua_State*, sol::protected_function_result
                           pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
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
            // is dispatched to the owning service actor via fork_task_atom.
            // The task outlives the registering coroutine (on_init / any
            // handler runs as one), so re-anchor the function onto the main
            // thread's lua_State — otherwise the coroutine's lua_State may be
            // collected by the GC and every later use of the captured
            // reference would dangle (mirror of anchor_to_main_thread in
            // lua_service.cpp).
            lua_State* fn_state = fn.lua_state();
            lua_State* fn_main =
                fn_state == nullptr ? nullptr : sol::main_thread(fn_state);
            if (fn_main != nullptr && fn_main != fn_state &&
                fn.registry_index() != LUA_NOREF) {
                fn =
                    sol::function(fn_main, sol::ref_index(fn.registry_index()));
            }
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

// -- Client identity registration ---------------------------------------------
//
// ClientContext / ClientRef usertypes plus the __shield_make_client_context
// materializer used by json_to_lua and by the coroutine resume path. No
// constructor is exported: identity userdata is created by the runtime only.
void register_client_identity_api(sol::state& lua) {
    sol::usertype<ClientContextBox> context_type =
        lua.new_usertype<ClientContextBox>("ClientContext",
                                           sol::no_constructor);
    bind_identity_properties(context_type);
    context_type.set("ref",  // GCOVR_EXCL_LINE (gcov clone artifact)
                     [](const ClientContextBox& box, sol::this_state s) {
                         return sol::make_object(s, ClientRefBox{box.data});
                     });
    sol::usertype<ClientRefBox> ref_type =
        lua.new_usertype<ClientRefBox>("ClientRef", sol::no_constructor);
    bind_identity_properties(ref_type);

    lua.set_function(
        "__shield_make_client_context",
        [](sol::this_state s, std::uint64_t session_id,
           std::uint32_t session_epoch, std::string player_id,
           std::string gateway_address, std::string protocol_profile_id) {
            return sol::make_object(
                s, ClientContextBox{ClientContextData{
                       std::move(gateway_address), session_id, session_epoch,
                       std::move(player_id), std::move(protocol_profile_id)}});
        });
}

// shield.client.* (bind / close) and the _client_* primitives behind them.
// The bind primitive suspends the caller coroutine exactly like _coro_call:
// the gateway actor completes the session through complete_call, and the
// Lua wrapper resumes with (true, client_ref) or (false, error_table).
void register_client_api(sol::table& shield, LuaServiceManager* manager) {
    shield.set_function(
        "_client_bind",
        [manager](sol::this_state state, sol::object client,
                  std::string player_id, std::string target_service,
                  int timeout_ms) -> uint64_t {
            ClientContextData data;
            if (!client_arg_to_data(client, &data) || player_id.empty() ||
                target_service.empty()) {
                return 0;
            }
            // Suspend first: every failure below completes the session
            // asynchronously so the wrapper's coroutine.yield() always gets
            // exactly one resume.
            const uint64_t session =
                manager->suspend_for_call(state, timeout_ms);
            caf::actor gateway = manager->gateway_actor(data.gateway_address);
            if (gateway == nullptr) {
                manager->complete_call(
                    session, false,
                    nlohmann::json::array({nlohmann::json::object(
                        {{"code", "client_rpc.epoch_expired"},
                         {"message",
                          "gateway not found: " + data.gateway_address}})}));
                return session;
            }
            ClientBindRequest request;
            request.call_session = session;
            request.sender_service = manager->current_service_id();
            request.context = data;
            request.player_id = std::move(player_id);
            request.target_service = std::move(target_service);
            caf::anon_send(gateway, std::move(request));
            return session;
        });

    shield.set_function(
        "_client_close",
        [manager](sol::object client, std::string reason) -> bool {
            ClientContextData data;
            if (!client_arg_to_data(client, &data)) {
                return false;
            }
            caf::actor gateway = manager->gateway_actor(data.gateway_address);
            if (gateway == nullptr) {
                return false;
            }
            ClientCloseRequest request;
            request.context = data;
            request.reason = std::move(reason);
            caf::anon_send(gateway, std::move(request));
            return true;
        });

    shield.set_function(
        "_client_egress",
        [manager](sol::object client, uint32_t route_id,
                  sol::object payload) -> bool {
            ClientContextData data;
            if (!client_arg_to_data(client, &data)) {
                return false;
            }
            caf::actor gateway = manager->gateway_actor(data.gateway_address);
            if (gateway == nullptr) {
                return false;
            }
            ClientEgress egress;
            egress.context = data;
            egress.route_id = route_id;
            if (payload.is<sol::table>()) {
                egress.message = lua_to_json(payload);
            } else if (payload.is<std::string>()) {
                const auto value = payload.as<std::string>();
                egress.body_bytes.assign(value.begin(), value.end());
            } else {
                return false;
            }
            caf::anon_send(gateway, std::move(egress));
            return true;
        });

    sol::state_view lua(shield.lua_state());
    sol::table client = lua.create_table();
    shield["client"] = client;
    // The wrapper bodies resolve the shield table as a global at CALL time
    // (register_full_shield_api assigns lua["shield"] afterwards), so the
    // chunk only returns the functions instead of touching the global.
    sol::function bind_fn = lua.safe_script(
        "return function(client, player_id, target)\n"
        "  if shield._is_in_exit() then\n"
        "    return false, {code='api_not_allowed_in_exit', "
        "message='shield.client.bind is not allowed in on_exit'}\n"
        "  end\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then\n"
        "    return false, {code='call_not_allowed_off_coroutine', "
        "message='shield.client.bind requires a handler coroutine'}\n"
        "  end\n"
        "  local session = shield._client_bind(client, player_id, target, "
        "5000)\n"
        "  if session == 0 then\n"
        "    return false, {code='invalid_client_reference', "
        "message='bind requires a ClientContext or ClientRef and a "
        "non-empty player_id and target'}\n"
        "  end\n"
        "  local r = table.pack(coroutine.yield())\n"
        "  if not r[1] then return false, r[2] end\n"
        "  return true, r[2]\n"
        "end\n",
        [](lua_State*, sol::protected_function_result
                           pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
    client["bind"] = bind_fn;
    sol::function close_fn = lua.safe_script(
        "return function(client, reason)\n"
        "  return shield._client_close(client, reason or 'kicked')\n"
        "end\n",
        [](lua_State*, sol::protected_function_result
                           pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
    client["close"] = close_fn;
}

// Registers one shield.client_rpc.<name> helper bound to a server-to-client
// descriptor route. Called per service VM at spawn time (after the
// descriptor table is compiled).
void register_client_rpc_helper(sol::state& lua, LuaServiceManager* manager,
                                std::string_view name, uint32_t route_id) {
    sol::table shield = lua["shield"];
    sol::table client_rpc = shield["client_rpc"];
    client_rpc.set_function(
        std::string(name),
        [manager, route_id](sol::object client, sol::object payload) -> bool {
            ClientContextData data;
            if (!client_arg_to_data(client, &data)) {
                return false;
            }
            caf::actor gateway = manager->gateway_actor(data.gateway_address);
            if (gateway == nullptr) {
                return false;
            }
            ClientEgress egress;
            egress.context = data;
            egress.route_id = route_id;
            if (payload.is<sol::table>()) {
                egress.message = lua_to_json(payload);
            } else if (payload.is<std::string>()) {
                const auto value = payload.as<std::string>();
                egress.body_bytes.assign(value.begin(), value.end());
            } else {
                return false;
            }
            caf::anon_send(gateway, std::move(egress));
            return true;
        });
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
            // Decimal string: see the node_epoch() binding above.
            entry["epoch"] = std::to_string(node.epoch);
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

    cluster.set_function("node_epoch", []() -> sol::optional<std::string> {
        auto* cluster_manager = shield::cluster::global_cluster_manager();
        if (!cluster_manager) {
            return sol::nullopt;
        }
        // Serialized as a decimal string: uint64 does not survive the
        // Lua number (double, 53-bit mantissa) round-trip.
        return std::to_string(cluster_manager->node_epoch());
    });

    shield["cluster"] = cluster;
}
#endif

void register_http_api(sol::table& shield, LuaServiceManager* manager,
                       LuaRuntime* runtime) {
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

    // Routes are stored in the runtime's registration table. The bootstrap
    // HTTP bridge (LuaHttpBridge) mirrors them into the HttpServer and
    // dispatches requests back onto the registering service's actor thread.
    auto register_route = [manager, runtime](
                              sol::this_state state, std::string method,
                              std::string path, sol::function handler) {
        if (!manager || !runtime) {
            throw sol::error("shield.httpd is not available in this context");
        }
        const std::string service_id = manager->current_service_id();
        if (service_id.empty()) {
            throw sol::error(
                "shield.httpd routes must be registered from a running "
                "service");
        }
        auto vm = runtime->vm_for_state(state);
        if (!vm) {
            throw sol::error("registering VM is not managed by the runtime");
        }
        std::string error;
        if (!runtime->register_http_route(vm, service_id, method, path,
                                          std::move(handler), &error)) {
            throw sol::error("shield.httpd registration failed: " + error);
        }
        sol::state_view lua(state);
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
    register_client_identity_api(lua);

    auto shield = lua.create_table();

    register_service_api(shield, manager);
    register_message_api(shield, manager, runtime);
    register_timer_api(shield, manager, runtime);
    register_task_api(shield, manager, runtime);
    register_config_api(shield);
    register_log_api(shield, manager);
    register_client_api(shield, manager);
    register_http_api(shield, manager, runtime);
    register_plugin_api(shield);

#ifdef SHIELD_ENABLE_CLUSTER
    register_cluster_api(shield, manager);
#endif

    lua["shield"] = shield;

    // Client RPC namespace (architecture-correction M1). Populated by the
    // gateway actor integration: shield.client.bind/close (M2) and one
    // helper per s2c descriptor (M2). Existing as an empty table now keeps
    // the API surface stable for scripts written against the new contract.
    lua["shield"]["client_rpc"] = lua.create_table();

    // AD-07: hook os.time / os.date so Lua business code also reads the
    // business-time clock (adjustable in tests). Only the no-arg forms are
    // redirected; os.time(table) and os.date(fmt, t) keep their original
    // semantics (pure conversion). os.clock() is NOT touched — it is used
    // for real CPU-time measurement (e.g. busy-wait loops).
    {
        sol::table os_t = lua["os"];
        sol::function orig_time = os_t["time"];
        sol::function orig_date = os_t["date"];
        // os.time(): no-arg → business clock seconds; with table → original.
        os_t.set_function("time",
                          [manager, orig_time = std::move(orig_time)](
                              sol::optional<sol::table> t) -> sol::object {
                              sol::state_view lua(orig_time.lua_state());
                              if (t.has_value()) {
                                  return orig_time(t.value());
                              }
                              return sol::make_object(
                                  lua, manager->clock_now_seconds());
                          });
        // os.date(fmt): no time → business clock; with time → original.
        os_t.set_function(
            "date",
            [manager, orig_date = std::move(orig_date)](
                sol::optional<std::string> fmt,
                sol::optional<double> t) -> sol::object {
                double when =
                    t.has_value()
                        ? t.value()
                        : static_cast<double>(manager->clock_now_seconds());
                return orig_date(fmt, when);
            });
    }

    // Coroutine dispatch helper used by
    // GCOVR_EXCL_START (gcov clone artifact; attribution drift under reformat)
    // LuaRuntime::call_service_method_coroutine. It wraps a handler + args
    // table into a coroutine whose body returns the handler's results, so a
    // yield inside the handler (e.g. shield.sleep) suspends the whole coroutine
    // and a later resume continues transparently. Defined as a global function
    // directly so a script error can't take down register_api
    // (call_service_method_coroutine falls back to sync dispatch if the helper
    // is absent).
    lua.safe_script(
        "function __shield_run_handler(handler, args)\n"
        // GCOVR_EXCL_STOP
        "  return coroutine.create(function()\n"
        "    return handler(table.unpack(args, 1, args.n or #args))\n"
        "  end)\n"
        "end",
        [](lua_State*, sol::protected_function_result
                           pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
}

}  // namespace shield::lua
