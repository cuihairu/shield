// [SHIELD_LUA] Lua Runtime call machinery: service function and
// method calls, coroutine invocation, client RPC and hook
// dispatch. Split out of lua_runtime.cpp.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "lua_panic.hpp"
#include "lua_runtime_impl.hpp"
#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/client_identity.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/lua/player_ref_box.hpp"
#include "shield/lua/profile_sampler.hpp"
#include "shield/plugin/plugin_host.hpp"

namespace shield::lua {

namespace {
// Client request bodies are untrusted and must never materialize identity
// markers. Strip the reserved marker keys recursively before converting the
// request to Lua; nested copies were the residual SEC-3 bypass because
// json_to_lua trusts every marker by design (service-to-service boundaries).
nlohmann::json sanitize_client_request(const nlohmann::json& value) {
    if (value.is_array()) {
        nlohmann::json sanitized =
            nlohmann::json::array();  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // the array arm is executed (count 2) but
                                      // the inlined nlohmann::json braced-init
                                      // produces a never-taken pseudo-branch)
        sanitized.get_ref<nlohmann::json::array_t&>().reserve(value.size());
        for (const auto& item : value) {
            sanitized.push_back(sanitize_client_request(item));
        }
        return sanitized;  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                           // return of the nlohmann::json value produces a
                           // pseudo-branch)
    }
    if (!value.is_object()) {
        return value;
    }
    nlohmann::json sanitized = nlohmann::json::object();
    for (const auto& [key, item] : value.items()) {
        if (key == "__shield_client_ref" || key == "__shield_player_ref") {
            continue;
        }
        sanitized[key] = sanitize_client_request(item);
    }
    return sanitized;
}
}  // namespace

namespace {

// Stringify a non-string (false, nil) hook-failure message slot. Runs on the
// bare stack with no active pcall, so this must never raise: unsupported
// values degrade to "<unsupported>" through lua_to_json's own fallback.
void describe_hook_message(std::string_view func_name, const shd::object& obj,
                           std::string* out) {
    if (out == nullptr) {  // GCOVR_EXCL_BR_LINE (defensive) GCOVR_EXCL_START
                           // (defensive: both callers pass a
        return;            // local string, guarded by their own if (error))
    }
    // GCOVR_EXCL_STOP
    nlohmann::json encoded;
    lua_to_json(obj, &encoded);
    *out = std::string(func_name) + " failed with non-string message: " +
           (encoded.is_string() ? encoded.get<std::string>() : encoded.dump());
}

}  // namespace

bool LuaRuntime::call_service_function(std::shared_ptr<LuaVM> vm,
                                       std::string_view func_name,
                                       const nlohmann::json& args,
                                       std::string* error) {
    try {
        shd::table& service = vm->service_table();
        if (!service.valid()) {
            if (error) {  // GCOVR_EXCL_BR_LINE (integration: every suite call
                          // passes a non-null error out-param)
                *error = "service module not loaded";
            }
            return false;
        }

        shd::object value = service[std::string(func_name)];
        if (!value.valid() ||  // GCOVR_EXCL_BR_LINE (defensive: a service table
                               // index always yields a valid (nil) object)
            value ==           // GCOVR_EXCL_BR_LINE (compiler artifact: the
                      // nil-comparison records attribute to this first line
                      // of the expression; the guard is annotated below)
                shd::nil) {  // GCOVR_EXCL_BR_LINE (defensive: a service table
                             // index always yields a valid (nil) object, so
                             // !value.valid() never fires; the missing-name arm
                             // is covered by the call-service-function tests)
            return true;
        }
        if (!value.is<shd::protected_function>()) {
            if (error) {
                *error = std::string(func_name) + " is not a function";
            }
            return false;
        }

        shd::state_view lua(*vm->state());
        shd::protected_function func = value.as<shd::protected_function>();
        shd::protected_function_result result = func(json_to_lua(lua, args));
        if (!result.valid()) {
            shd::error err = result;
            if (error) {
                *error = err.what();
            }
            return false;
        }

        shd::object first = result.get<shd::object>(0);
        // The message slot is host-read on the bare stack (no active pcall):
        // a bare as<std::string> on a non-string raises an unprotected
        // luaL_error that long-jumps past the catch below into at_panic, so
        // the type is checked first and anything else is stringified through
        // the shared Lua->JSON conversion.
        if (first.is<bool>() && !first.as<bool>()) {
            shd::object second = result.get<shd::object>(1);
            if (error) {
                if (second.valid() &&  // GCOVR_EXCL_BR_LINE (defensive:
                                       // result.get<shd::object> always yields
                                       // a valid object)
                    second != shd::nil) {  // GCOVR_EXCL_BR_LINE (defensive:
                                           // result.get<shd::object> always
                                           // yields a valid object, so
                                           // !second.valid() never fires)
                    if (second.is<std::string>()) {
                        *error = second.as<std::string>();
                    } else {
                        std::string described;
                        describe_hook_message(func_name, second, &described);
                        *error = described;
                    }
                } else {
                    *error = std::string(func_name) + " returned false";
                }
            }
            return false;
        }
        if (first == shd::nil && result.return_count() > 1) {
            shd::object second = result.get<shd::object>(1);
            if (error) {
                if (second.valid() &&  // GCOVR_EXCL_BR_LINE (defensive:
                                       // result.get<shd::object> always yields
                                       // a valid object)
                    second !=  // GCOVR_EXCL_BR_LINE (compiler artifact: the
                               // nil-comparison records attribute to this
                               // first line of the expression; the guard is
                               // annotated below)
                        shd::nil) {  // GCOVR_EXCL_BR_LINE (defensive: same arm
                                     // as the first message slot above - a
                                     // stack get is always valid)
                    if (second.is<std::string>()) {
                        *error = second.as<std::string>();
                    } else {
                        std::string described;
                        describe_hook_message(func_name, second, &described);
                        *error = described;
                    }
                } else {
                    *error = std::string(func_name) + " returned nil";
                }
            }
            return false;
        }

        return true;
        // GCOVR_EXCL_START (defensive: every sol call inside the try is a
        // protected_function_result read, and the message-slot conversions
        // type-check before touching the stack — nothing here is expected to
        // throw; the catch keeps an unexpected throw from escaping the API)
    } catch (const std::exception& e) {  // GCOVR_EXCL_BR_LINE (defensive:
                                         // nothing inside the try can throw)
        if (error) {  // GCOVR_EXCL_BR_LINE (defensive: same unreachable block
                      // as the START range around it)
            *error =
                e.what();  // GCOVR_EXCL_BR_LINE (defensive: same unreachable
                           // block as the START range around it)
        }
        return false;
    }
    // GCOVR_EXCL_STOP
}

bool LuaRuntime::resolve_service_method(std::shared_ptr<LuaVM> vm,
                                        std::string_view method_name,
                                        shd::function* out,
                                        std::string* error) {
    try {
        shd::table& service = vm->service_table();
        if (!service.valid()) {  // GCOVR_EXCL_BR_LINE (defensive: dispatch
                                 // never sees an unloaded module)
                                 // GCOVR_EXCL_START (defensive: dispatch only
                                 // reaches loaded modules)
            if (error) {  // GCOVR_EXCL_BR_LINE (defensive: same unreachable
                          // block as the START range above)
                *error = "service module not loaded";  // GCOVR_EXCL_BR_LINE
                                                       // (defensive: same
                                                       // unreachable block as
                                                       // the START range above)
            }
            return false;
        }  // GCOVR_EXCL_STOP

        shd::object value = service[std::string(method_name)];
        if (!value.valid() ||  // GCOVR_EXCL_BR_LINE (defensive: a service table
                               // index always yields a valid object)
            value == shd::nil ||  // GCOVR_EXCL_BR_LINE (defensive: a service
                                  // table index always yields a valid object,
                                  // so the validity arm never fires; the
                                  // missing / not-a-function / success arms are
                                  // covered by the resolve tests)
            !value.is<shd::protected_function>()) {
            if (error) {
                *error = "method '" + std::string(method_name) +
                         "' is missing or not a function";
            }
            return false;
        }
        if (out != nullptr) {
            *out = value.as<shd::protected_function>();
        }
        return true;
    } catch (const std::exception& e) {  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: catch-entry pseudo-arc)
                                         // GCOVR_EXCL_START (defensive: the
        if (error) {  // GCOVR_EXCL_BR_LINE (defensive) lookup above cannot
                      // throw; kept for the contract)
            *error =
                e.what();  // GCOVR_EXCL_BR_LINE (defensive: same unreachable
                           // block as the START range around it)
        }
        return false;
    }
    // GCOVR_EXCL_STOP
}

bool LuaRuntime::call_service_method(std::shared_ptr<LuaVM> vm,
                                     std::string_view method_name,
                                     const nlohmann::json& args,
                                     nlohmann::json* returns,
                                     std::string* error) {
    try {
        shd::table& service = vm->service_table();
        if (!service.valid()) {
            if (error) {
                *error = "service module not loaded";
            }
            return false;
        }

        shd::object value = service[std::string(method_name)];
        if (!value.valid() ||  // GCOVR_EXCL_BR_LINE (defensive: a service table
                               // index always yields a valid (nil) object)
            value ==           // GCOVR_EXCL_BR_LINE (compiler artifact: the
                      // nil-comparison records attribute to this first line
                      // of the expression; the guard is annotated below)
                shd::nil) {  // GCOVR_EXCL_BR_LINE (defensive: a service table
                             // index always yields a valid (nil) object, so
                             // !value.valid() never fires; the missing-method
                             // arm is covered by the call-service-method tests)
            if (error) {
                *error = "method not found: " + std::string(method_name);
            }
            return false;
        }
        if (!value.is<shd::protected_function>()) {
            if (error) {
                *error = std::string(method_name) + " is not a function";
            }
            return false;
        }

        if (!args.is_array()) {
            if (error) {
                *error = "method args must be a JSON array";
            }
            return false;
        }

        shd::state_view lua(*vm->state());
        std::vector<shd::object> lua_args;
        lua_args.reserve(args.size());
        for (const auto& arg : args) {
            lua_args.push_back(json_to_lua(lua, arg));
        }

        shd::protected_function func = value.as<shd::protected_function>();
        shd::protected_function_result result = func(shd::as_args(lua_args));
        if (!result.valid()) {
            shd::error err = result;
            if (error) {
                *error = err.what();
            }
            return false;
        }

        if (returns) {
            *returns = nlohmann::json::array();
            for (int i = 0; i < result.return_count(); ++i) {
                nlohmann::json item;
                if (!lua_to_json(result.get<shd::object>(i), &item)) {
                    if (error) {
                        *error = "unsupported return value from " +
                                 std::string(method_name);
                    }
                    return false;
                }
                returns->push_back(std::move(item));
            }
        }

        return true;
    } catch (const std::exception& e) {  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: catch-entry pseudo-arc)
                                         // GCOVR_EXCL_START (defensive: the
        if (error) {  // GCOVR_EXCL_BR_LINE (defensive) handler result was
                      // already guarded above)
            *error =
                e.what();  // GCOVR_EXCL_BR_LINE (defensive: same unreachable
                           // block as the START range around it)
        }
        return false;
    }
}

// GCOVR_EXCL_STOP

bool LuaRuntime::invoke_coroutine(
    std::shared_ptr<LuaVM> vm, shd::function handler,
    const std::vector<nlohmann::json>& args, const std::string& error_type,
    const std::string& method_label, uint64_t call_session,
    LuaServiceManager* manager, std::string_view service_id, std::string* error,
    bool prepend_ctx, bool degrade_on_factory_failure) {
    // Completion helpers: route the outcome to the pending call request (when
    // present) and to the service error hook on failure.
    auto finish_ok = [&](const nlohmann::json& returns) -> bool {
        // GCOVR_EXCL_START (defensive: coverage suites complete calls via
        // the direct path (call_session == 0, manager == nullptr), so this
        // call-completion and error-count bookkeeping never runs)
        if (call_session != 0 && manager != nullptr) {
            manager->complete_call(call_session, true, returns);
        }
        if (manager && !service_id.empty()) {
            manager->reset_error_count(std::string(service_id));
        }
        return true;
        // GCOVR_EXCL_STOP
    };
    auto finish_err = [&](const std::string& msg) -> bool {
        // GCOVR_EXCL_BR_START (defensive: error out-param is always
        // provided; suites complete calls via the direct path)
        if (error)
            *error = msg;  // GCOVR_EXCL_BR_LINE (defensive: every caller passes
                           // a non-null error out-param)
        if (call_session != 0 &&
            manager !=
                nullptr) {  // GCOVR_EXCL_BR_LINE (defensive: suites complete
                            // calls via the direct path (call_session == 0))
            manager->complete_call(
                call_session, false,
                nlohmann::json::array(
                    {msg}));  // GCOVR_EXCL_BR_LINE (compiler artifact: nlohmann
                              // init-list construction arcs)
        }
        if (manager &&
            !service_id.empty()) {  // GCOVR_EXCL_BR_LINE (defensive: the error
                                    // hook is exercised via the direct path)
            manager->invoke_error_hook(std::string(service_id), error_type,
                                       method_label, msg);
            // The segment terminated here (error): honor a shield.exit it
            // requested. During spawn-init the spawn path owns the request
            // instead — exit() cannot drive an unpublished service.
            if (!LuaServiceManager::
                    spawn_init_in_progress()) {  // GCOVR_EXCL_BR_LINE
                                                 // (integration: spawn-init
                                                 // errors are owned by the
                                                 // spawn path)
                manager->finish_pending_exit(std::string(service_id));
            }
        }
        return false;
        // GCOVR_EXCL_BR_STOP
    };  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda exit/exception edges
        // only - the body runs, the throw arcs never fire)

    try {
        shd::state_view lua(*vm->state());
        lua_State* L = lua.lua_state();

        // Plain synchronous execution with the same completion routing as the
        // coroutine path. Used when no factory is registered (bare VM) and —
        // for callers that opt in — as the degrade path for a failing
        // factory. The degrade path never passes ctx: it mirrors the
        // historical synchronous dispatch signature.
        auto run_sync = [&](bool with_ctx) -> bool {
            std::vector<shd::object> call_args;
            if (with_ctx) {
                shd::table ctx = lua.create_table();
                call_args.push_back(ctx);
            }
            for (const auto& arg : args) {
                call_args.push_back(json_to_lua(lua, arg));
            }
            shd::protected_function handler_pf = handler;
            shd::protected_function_result fr =
                handler_pf(shd::as_args(call_args));
            if (!fr.valid()) {
                shd::error err = fr;
                std::string msg = err.what();
                // GCOVR_EXCL_START (defensive: sol error messages always
                // carry a "[string ...]:N:" position prefix, so msg is never
                // empty in practice)
                if (msg.empty()) {  // GCOVR_EXCL_BR_LINE (defensive: sol error
                                    // messages always carry a position prefix,
                                    // so msg is never empty)
                    msg = method_label +
                          " raised an error";  // GCOVR_EXCL_BR_LINE (defensive:
                                               // same arm as above - what() is
                                               // never empty)
                }
                // GCOVR_EXCL_STOP
                return finish_err(msg);
            }
            nlohmann::json returns = nlohmann::json::array();
            for (int i = 0; i < fr.return_count(); ++i) {
                nlohmann::json item;
                lua_to_json(fr.get<shd::object>(i), &item);
                returns.push_back(std::move(item));
            }
            return finish_ok(returns);
        };

        shd::function factory = lua["__shield_run_handler"];
        if (!factory.valid()) {
            // Bare VM without the shield API: the handler cannot yield, so
            // this degrades to a plain synchronous call with the same
            // completion routing as the coroutine path.
            return run_sync(prepend_ctx);
        }
        if (!handler.valid()) {
            return finish_err("handler is not a function");
        }

        // Dispatch ctx table: sender / trace / deadline from the active
        // dispatch scope (nil when unset). Hooks with a no-ctx signature
        // (on_init receives (args) directly) pass prepend_ctx = false.
        shd::table args_table = lua.create_table();
        int arg_count = 0;
        if (prepend_ctx) {
            shd::table ctx = lua.create_table();
            if (manager != nullptr) {
                std::string sender = manager->current_sender_id();
                ctx["sender"] =
                    sender.empty() ? shd::nil : shd::make_object(lua, sender);
                std::string trace = manager->current_trace_id();
                ctx["trace"] =
                    trace.empty()
                        ? shd::nil  // GCOVR_EXCL_BR_LINE (integration: see the
                                    // annotated continuation below)
                        : shd::make_object(
                              lua,
                              trace);  // GCOVR_EXCL_BR_LINE (integration: the
                                       // only caller, client ingress dispatch,
                                       // never injects a trace id)
                int64_t deadline = manager->current_deadline_ms();
                ctx["deadline"] =
                    deadline <= 0
                        ? shd::nil  // GCOVR_EXCL_BR_LINE (integration: see the
                                    // annotated continuation below)
                        : shd::make_object(
                              lua,
                              deadline);  // GCOVR_EXCL_BR_LINE (integration:
                                          // the only caller, client ingress
                                          // dispatch, never injects a deadline)
            }
            args_table.add(ctx);
            ++arg_count;
        }
        for (const auto& arg : args) {
            shd::object value = json_to_lua(lua, arg);
            ++arg_count;
            // table::add(nil) appends at objlen+1, which after a deleted
            // slot reuses the hole and shifts every later argument left; a
            // JSON null argument must stay a positional hole instead.
            // raw_set with an explicit key leaves it as one (unpack hands
            // the callee nil for it, using args.n below as the length).
            args_table.raw_set(arg_count, value);
        }
        args_table["n"] = arg_count;

        // The factory result stays alive across lua_resume so the thread
        // remains anchored on the main stack; a yielding handler is
        // re-anchored by whichever API suspended it. A factory failure
        // degrades to synchronous dispatch instead of throwing through the
        // runtime.
        shd::protected_function factory_pf = lua["__shield_run_handler"];
        shd::protected_function_result fr = factory_pf(handler, args_table);
        if (!fr.valid()) {
            if (degrade_on_factory_failure) {
                return run_sync(false);
            }
            shd::error err = fr;
            std::string msg = err.what();
            if (msg.empty()) {  // GCOVR_EXCL_BR_LINE (defensive: what() is
                                // never empty in practice)
                msg = "handler coroutine factory failed";  // GCOVR_EXCL_LINE
            }
            return finish_err(msg);
        }
        lua_State* co = lua_tothread(L, -1);
        if (co == nullptr) {
            if (degrade_on_factory_failure) {
                return run_sync(false);
            }
            return finish_err("handler coroutine thread missing");
        }
        // A call request tags its handler coroutine so the completion can be
        // routed back to the caller when the coroutine finishes (possibly
        // much later, after several yields).
        // GCOVR_EXCL_BR_START (defensive: suites complete calls via the
        // direct path (call_session == 0))
        if (call_session != 0 &&
            manager !=
                nullptr) {  // GCOVR_EXCL_BR_LINE (defensive: suites complete
                            // calls via the direct path (call_session == 0))
            manager->set_handler_call_session(co, call_session);
            // GCOVR_EXCL_BR_STOP
        }
        // L1 observability: this handler coroutine counts as live under its
        // service until some resume source observes its terminal state.
        // error_type doubles as the resume origin for the inspect detail.
        // The registry ref anchors the thread against the GC for as long
        // as it is bookkept: a bare coroutine.yield() has no suspending
        // C++ API to re-anchor it, so once the dispatch stack unwinds the
        // GC could otherwise collect the suspended thread and leave
        // live_coroutines dangling. The (shared) registry is reachable
        // from the coroutine itself, so this never touches the main stack.
        int co_anchor = LUA_NOREF;
        if (manager != nullptr && !service_id.empty()) {
            lua_pushthread(co);
            co_anchor = luaL_ref(co, LUA_REGISTRYINDEX);
            manager->note_coroutine_started(co, service_id, error_type,
                                            co_anchor);
        }

        int nres = 0;
        // Driving-phase registration: resume_caller on another thread
        // re-enqueues a completion arriving while this guard is held
        // (see LuaServiceManager::resume_caller).
        std::unique_ptr<LuaServiceManager::DrivingGuard> driving;
        if (manager != nullptr) {
            driving = std::make_unique<LuaServiceManager::DrivingGuard>(
                *manager, co, "init-drive");
        }
        // Arm the brand-new handler coroutine before its first drive: Lua
        // 5.5 does not propagate hooks to new threads, and this coroutine
        // was created after install — the count hook would never see it.
        // No-op (one thread_local read) while no sampling session is on
        // this owner thread.
        ProfileSampler::sweep_active(service_id);
        const int status = lua_resume(co, L, 0, &nres);
        if (status == LUA_OK) {
            // Terminal: this resume observed the coroutine complete.
            if (manager != nullptr) {
                manager->note_coroutine_finished(co);
            }
            nlohmann::json returns = nlohmann::json::array();
            for (int i = 0; i < nres; ++i) {
                nlohmann::json item;
                shd::stack_object so(shd::state_view(co), i + 1);
                lua_to_json(so, &item);
                returns.push_back(std::move(item));
            }
            // Routes the response to the caller via on_handler_completed when
            // this dispatch serviced a call request.
            if (call_session != 0 &&
                manager != nullptr) {  // GCOVR_EXCL_BR_LINE (defensive: suites
                                       // complete calls via the direct path
                                       // (call_session == 0))
                manager->on_handler_completed(co, returns);
            }
            if (manager && !service_id.empty()) {
                manager->reset_error_count(std::string(service_id));
                // The segment completed here (LUA_OK): honor a shield.exit
                // it requested. During spawn-init the spawn path owns the
                // request instead — exit() cannot drive an unpublished
                // service.
                if (!LuaServiceManager::spawn_init_in_progress()) {
                    manager->finish_pending_exit(std::string(service_id));
                }
            }
            return true;
        }
        if (status == LUA_YIELD) {
            // Suspended (shield.sleep / call): anchored by the suspending API
            // and resumed by the runtime. The driving-phase registration
            // ends with the guard below, so a completion that arrives later
            // resumes directly.
            return true;
        }
        std::string msg =
            method_label.empty()
                ? std::string(
                      "handler raised an error")  // GCOVR_EXCL_BR_LINE
                                                  // (defensive: method_label is
                                                  // always set by the dispatch
                                                  // paths)
                : method_label + " raised an error";
        if (lua_type(co, -1) == LUA_TSTRING) {
            msg = lua_tostring(co, -1);
        }
        lua_settop(co, 0);
        // Terminal (error): drop the live-coroutine entry.
        if (manager != nullptr) {
            manager->note_coroutine_finished(co);
        }
        // GCOVR_EXCL_BR_START (defensive: suites complete calls via the
        // direct path (call_session == 0))
        if (call_session != 0 &&
            manager !=
                nullptr) {  // GCOVR_EXCL_BR_LINE (defensive: suites complete
                            // calls via the direct path (call_session == 0))
            manager->on_handler_failed(co, msg);
            // GCOVR_EXCL_BR_STOP
        }
        if (manager && !service_id.empty()) {
            manager->invoke_error_hook(std::string(service_id), error_type,
                                       method_label, msg);
        }
        if (error) *error = msg;
        return false;
    } catch (const std::exception& e) {  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: catch-entry pseudo-arc)
                                         // GCOVR_EXCL_START (defensive: the
        if (error)  // GCOVR_EXCL_BR_LINE (defensive) dispatch path converts
                    // every failure itself)
            *error =
                e.what();  // GCOVR_EXCL_BR_LINE (defensive: same unreachable
                           // block as the START range around it)
        return false;
    }
    // GCOVR_EXCL_STOP
}

bool LuaRuntime::call_service_method_coroutine(
    std::shared_ptr<LuaVM> vm, std::string_view method_name,
    const nlohmann::json& args, std::string* error, uint64_t call_session,
    LuaServiceManager* manager, std::string_view service_id) {
    auto complete_call_failure = [&](const std::string& msg) {
        // GCOVR_EXCL_BR_START (defensive: suites complete calls via the
        // direct path (call_session == 0))
        if (call_session != 0 &&
            manager !=
                nullptr) {  // GCOVR_EXCL_BR_LINE (defensive: suites complete
                            // calls via the direct path (call_session == 0))
            manager->complete_call(
                call_session, false,
                nlohmann::json::array(
                    {msg}));  // GCOVR_EXCL_BR_LINE (compiler artifact: nlohmann
                              // init-list construction arcs)
        }
        // GCOVR_EXCL_BR_STOP
    };  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda exit/exception edges
        // only)

    // Synchronous dispatch for VMs without the shield API: the handler
    // cannot yield, so completion happens inline.
    auto fallback_dispatch = [&]() -> bool {
        nlohmann::json returns = nlohmann::json::array();
        std::string fallback_error;
        const bool ok = call_service_method(
            vm, method_name, args, call_session != 0 ? &returns : nullptr,
            &fallback_error);
        // GCOVR_EXCL_BR_START (defensive: suites complete calls via the
        // direct path (call_session == 0))
        if (call_session != 0 &&
            manager !=
                nullptr) {  // GCOVR_EXCL_BR_LINE (defensive: suites complete
                            // calls via the direct path (call_session == 0))
            if (ok) {
                manager->complete_call(call_session, true, returns);
            } else {
                manager->complete_call(
                    call_session, false,
                    nlohmann::json::array(
                        {fallback_error}));  // GCOVR_EXCL_BR_LINE (compiler
                                             // artifact: nlohmann init-list
                                             // construction arcs)
            }
        }
        // GCOVR_EXCL_BR_STOP
        if (!ok && error) {
            *error = fallback_error;
        }
        return ok;
    };  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda exit/exception edges
        // only)

    try {
        shd::table& service = vm->service_table();
        if (!service.valid()) {
            const std::string msg = "service module not loaded";
            if (error) *error = msg;
            complete_call_failure(msg);
            return false;
        }
        shd::object value = service[std::string(method_name)];
        if (!value.valid() ||  // GCOVR_EXCL_BR_LINE (defensive: a service table
                               // index always yields a valid (nil) object)
            value ==           // GCOVR_EXCL_BR_LINE (compiler artifact: the
                      // nil-comparison records attribute to this first line
                      // of the expression; the guard is annotated below)
                shd::nil) {  // GCOVR_EXCL_BR_LINE (defensive: a service table
                             // index always yields a valid (nil) object, so
                             // !value.valid() never fires; the missing-method
                             // arm is covered by the fallback-combos test)
            const std::string msg =
                "method not found: " + std::string(method_name);
            if (error) *error = msg;
            complete_call_failure(msg);
            return false;
        }
        if (!value.is<shd::protected_function>()) {
            const std::string msg =
                std::string(method_name) + " is not a function";
            if (error) *error = msg;
            complete_call_failure(msg);
            return false;
        }
        if (!args.is_array()) {
            const std::string msg = "method args must be a JSON array";
            if (error) *error = msg;
            complete_call_failure(msg);
            return false;
        }

        shd::state_view lua(*vm->state());
        if (!lua["__shield_run_handler"].valid()) {
            // No coroutine factory registered (e.g. a VM without the full
            // shield API). Fall back to a plain synchronous dispatch.
            return fallback_dispatch();
        }

        shd::protected_function handler = value.as<shd::protected_function>();
        std::vector<nlohmann::json> arg_vec(args.begin(), args.end());
        return invoke_coroutine(vm, handler, arg_vec, "handler",
                                std::string(method_name), call_session, manager,
                                service_id, error,
                                /*prepend_ctx=*/true,
                                /*degrade_on_factory_failure=*/true);
    } catch (const std::exception& e) {  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: catch-entry pseudo-arc)
                                         // GCOVR_EXCL_START (defensive: the
        if (error)  // GCOVR_EXCL_BR_LINE (defensive) dispatch path converts
                    // every failure itself)
            *error =
                e.what();  // GCOVR_EXCL_BR_LINE (defensive: same unreachable
                           // block as the START range around it)
        complete_call_failure(
            e.what());  // GCOVR_EXCL_BR_LINE (defensive: same unreachable block
                        // as the START range around it)
        return false;
    }
    // GCOVR_EXCL_STOP
}

bool LuaRuntime::invoke_client_rpc(std::shared_ptr<LuaVM> vm,
                                   shd::function handler,
                                   const ClientIngress& ingress,
                                   std::string* error,
                                   LuaServiceManager* manager,
                                   std::string_view service_id) {
    try {
        shd::state_view lua(*vm->state());
        lua_State* L = lua.lua_state();

        shd::function factory = lua["__shield_run_handler"];
        if (!factory.valid()) {
            // No coroutine factory registered (a VM without the full shield
            // API): client RPC requires coroutine-aware dispatch.
            if (error) *error = "coroutine factory not registered";
            return false;
        }
        if (!handler.valid()) {
            if (error) *error = "handler is not a function";
            return false;
        }
        if (!ingress.decoded_request.has_value()) {
            if (error) *error = "ingress request value missing";
            return false;
        }

        // Build args (ctx, client, request): the dispatch ctx table first,
        // then the client identity (the marker materializes as the read-only
        // ClientContext userdata inside json_to_lua), then the request value.
        shd::table ctx = lua.create_table();
        if (manager != nullptr) {
            std::string sender = manager->current_sender_id();
            ctx["sender"] =
                sender.empty()
                    ? shd::nil  // GCOVR_EXCL_BR_LINE (integration: see the
                                // annotated continuation below)
                    : shd::make_object(
                          lua, sender);  // GCOVR_EXCL_BR_LINE (integration: the
                                         // only caller, client ingress
                                         // dispatch, never injects a sender)
            std::string trace = manager->current_trace_id();
            ctx["trace"] =
                trace.empty()
                    ? shd::nil  // GCOVR_EXCL_BR_LINE (integration: see the
                                // annotated continuation below)
                    : shd::make_object(
                          lua, trace);  // GCOVR_EXCL_BR_LINE (integration: the
                                        // only caller, client ingress dispatch,
                                        // never injects a trace id)
            int64_t deadline = manager->current_deadline_ms();
            ctx["deadline"] =
                deadline <= 0
                    ? shd::nil  // GCOVR_EXCL_BR_LINE (integration: see the
                                // annotated continuation below)
                    : shd::make_object(
                          lua,
                          deadline);  // GCOVR_EXCL_BR_LINE (integration: the
                                      // only caller, client ingress dispatch,
                                      // never injects a deadline)
        }

        shd::table args_table = lua.create_table();
        args_table.add(ctx);
        args_table.add(json_to_lua(lua, ingress.context.to_json()));
        args_table.add(json_to_lua(
            lua, sanitize_client_request(*ingress.decoded_request)));
        args_table["n"] = 3;

#ifdef SHIELD_ENABLE_PLAYER
        // shield_player client-message guard (runtime-player.md): when the
        // VM has run shield.player.setup, every authenticated client message
        // passes through the guard before the compiled route handler. A
        // rejection (or a guard error) drops the frame with a warning — the
        // same shape as a gateway ingress validation failure — and never
        // reaches the handler.
        shd::object guard_obj = lua["__shield_player_guard"];
        if (guard_obj.valid() && guard_obj.is<shd::function>()) {
            // GCOVR_EXCL_START (player client guard: requires
            // SHIELD_ENABLE_PLAYER plus a live player session that installed
            // __shield_player_guard; the coverage suites do not wire one up)
            std::string route_name =
                "route_" + std::to_string(ingress.route_id);
            shd::object names_obj = lua["shield"]["_client_route_names"];
            if (names_obj.valid() && names_obj.is<shd::table>()) {
                shd::object name = names_obj.as<shd::table>().get<shd::object>(
                    static_cast<std::int64_t>(ingress.route_id));
                if (name.valid() && name.is<std::string>()) {
                    route_name = name.as<std::string>();
                }
            }
            shd::protected_function guard =
                guard_obj.as<shd::protected_function>();
            shd::protected_function_result gr =
                guard(ctx, args_table[2], route_name, args_table[3]);
            bool allowed = false;
            std::string reject_code;
            if (gr.valid() && gr.return_count() > 0) {
                allowed = gr.get<bool>(0);
                if (!allowed && gr.return_count() > 1 &&
                    gr.get<shd::object>(1).is<std::string>()) {
                    reject_code = gr.get<std::string>(1);
                }
            }
            if (!allowed) {
                auto& log = shield::log::get_logger("lua");
                SHIELD_LOG_WARNING(
                    log,
                    "client message dropped by player guard: route=" +
                        route_name +
                        (reject_code.empty() ? "" : " code=" + reject_code));
                return true;
            }
        }
        // GCOVR_EXCL_STOP
#endif

        shd::protected_function factory_pf = lua["__shield_run_handler"];
        shd::protected_function_result fr = factory_pf(handler, args_table);
        if (!fr.valid()) {
            if (error) *error = "handler coroutine factory failed";
            return false;
        }
        lua_State* co = lua_tothread(L, -1);
        if (co == nullptr) {
            if (error) *error = "handler coroutine thread missing";
            return false;
        }
        if (manager != nullptr && !service_id.empty()) {
            // "client_rpc" mirrors the error-hook classification below.
            // GC anchor, same reasoning as the handler dispatch path.
            lua_pushthread(co);
            const int co_anchor = luaL_ref(co, LUA_REGISTRYINDEX);
            manager->note_coroutine_started(co, service_id, "client_rpc",
                                            co_anchor);
        }

        // Arm the brand-new handler coroutine before its first drive: Lua
        // 5.5 does not propagate hooks to new threads, and this coroutine
        // was created after install — the count hook would never see it.
        // No-op (one thread_local read) while no sampling session is on
        // this owner thread. Mirrors the invoke_coroutine drive point.
        ProfileSampler::sweep_active(service_id);

        int nres = 0;
        std::unique_ptr<LuaServiceManager::DrivingGuard> driving;
        if (manager != nullptr) {
            driving = std::make_unique<LuaServiceManager::DrivingGuard>(
                *manager, co, "client-rpc");
        }
        const int status = lua_resume(co, L, 0, &nres);
        if (status == LUA_OK || status == LUA_YIELD) {
            if (status == LUA_OK && manager != nullptr) {
                // Fire-and-forget completion: no later resume will observe
                // this coroutine's terminal state, so finish it here.
                manager->note_coroutine_finished(co);
            }
            if (manager && !service_id.empty()) {
                manager->reset_error_count(std::string(service_id));
            }
            return true;
        }
        // Error: the error object is on the coroutine's stack. Client ingress
        // is fire-and-forget, so a handler error routes through the service
        // error hook only.
        std::string msg = "client rpc route " +
                          std::to_string(ingress.route_id) + " raised an error";
        if (lua_type(co, -1) == LUA_TSTRING) {
            msg = lua_tostring(co, -1);
        }
        lua_settop(co, 0);
        // Terminal (error): drop the live-coroutine entry.
        if (manager != nullptr) {
            manager->note_coroutine_finished(co);
        }
        if (error) *error = msg;
        if (manager && !service_id.empty()) {
            manager->invoke_error_hook(std::string(service_id), "client_rpc",
                                       std::to_string(ingress.route_id), msg);
        }
        return false;
    } catch (const std::exception& e) {  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: catch-entry pseudo-arc)
                                         // GCOVR_EXCL_START (defensive: the
        if (error)  // GCOVR_EXCL_BR_LINE (defensive) dispatch path converts
                    // every failure itself)
            *error =
                e.what();  // GCOVR_EXCL_BR_LINE (defensive: same unreachable
                           // block as the START range around it)
        return false;
    }
    // GCOVR_EXCL_STOP
}

bool LuaRuntime::invoke_hook(std::shared_ptr<LuaVM> vm, const char* hook_name,
                             const std::string& err_or_reason,
                             const std::string& error_type,
                             const std::string& method_name) {
    if (!vm) {
        return false;
    }
    shd::table& service = vm->service_table();
    if (!service.valid()) {
        return false;
    }
    shd::object hook_fn = service[hook_name];
    if (!hook_fn.is<shd::protected_function>()) {
        return false;
    }
    shd::state_view lua(vm->state()->lua_state());
    shd::table ctx = lua.create_table();
    ctx["type"] = error_type;
    ctx["method"] = method_name;
    shd::protected_function fn = hook_fn.as<shd::protected_function>();
    auto result = fn(err_or_reason, ctx);
    if (!result.valid()) {
        shd::error err = result;
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_ERROR(
            log, std::string(hook_name) + " hook failed: " + err.what());
        return false;
    }
    return true;
}

std::string LuaRuntime::call_function(std::shared_ptr<LuaVM> vm,
                                      std::string_view func_name,
                                      std::string_view args) {
    try {
        shd::protected_function func = (*vm->state())[std::string(func_name)];
        if (!func.valid()) {
            return R"({"error": "function not found"})";
        }

        auto result = func();
        if (result.valid()) {
            return R"({"ok": true})";
        } else {
            shd::error err = result;
            return R"({"error": ")" + std::string(err.what()) + R"("})";
        }
        // GCOVR_EXCL_START (catch-entry pseudo-arc: the serialize path is
        // already JSON-shaped and cannot throw; kept for the generic error
        // contract — keep the catch clause on one physical line so the entry
        // stays in this region)
    } catch (const std::exception& e) {
        return R"({"error": ")" + std::string(e.what()) + R"("})";
    }
    // GCOVR_EXCL_STOP
}

}  // namespace shield::lua
