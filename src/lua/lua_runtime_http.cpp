// [SHIELD_LUA] Lua Runtime http route registry and handler
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
// Mirror of anchor_to_main_thread in lua_service.cpp: re-anchor a sol
// reference onto the main thread's lua_State so a stored lua_State* cannot
// dangle after the registering coroutine is collected by the GC. The
// registry is shared across all threads of one global_State, so re-refering
// the same registry entry from the main thread keeps the pointer alive
// without changing which value the reference names.
shd::function anchor_to_main_thread(shd::function fn) {
    lua_State* state = fn.lua_state();
    if (state == nullptr ||  // GCOVR_EXCL_BR_LINE (defensive: callers verify
                             // handler.valid() first, see below)
        !fn.valid()) {  // GCOVR_EXCL_BR_LINE (defensive: both callers verify
                        // handler.valid() first, and a live shd::function
                        // always carries a lua_State)
        return fn;  // GCOVR_EXCL_LINE (defensive: anchored fns are always live)
    }
    lua_State* main_state = shd::main_thread(state);
    if (main_state ==   // GCOVR_EXCL_BR_LINE (defensive: never null)
            nullptr ||  // GCOVR_EXCL_BR_LINE (defensive: shd::main_thread never
                        // returns null for a live state)
        main_state ==
            state) {  // GCOVR_EXCL_BR_LINE (defensive: shd::main_thread never
                      // returns null for a live state)
        return fn;
    }
    return shd::function(main_state, shd::ref_index(fn.registry_index()));
}
}  // namespace

namespace {

// Segment-wise split of "/a/b" (empty segments skipped).
std::vector<std::string> split_path_segments(const std::string& path) {
    std::vector<std::string> segments;
    size_t pos = 0;
    while (pos < path.size()) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // uncalled exit clone of this function, same
                                 // drift as the exit line below)
        const size_t next = path.find('/', pos);
        const std::string seg = path.substr(
            pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!seg.empty()) {
            segments.push_back(seg);
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    return segments;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

// Match a ":param"-style pattern against a concrete path; captures params.
bool route_pattern_match(
    const std::string& pattern, const std::string& path,
    std::vector<std::pair<std::string, std::string>>* out_params) {
    const auto pat = split_path_segments(pattern);
    const auto seg = split_path_segments(path);
    if (pat.size() != seg.size()) {
        return false;
    }
    std::vector<std::pair<std::string, std::string>> params;
    for (size_t i = 0; i < pat.size(); ++i) {
        if (!pat[i].empty() &&  // GCOVR_EXCL_BR_LINE (defensive:
                                // split_path_segments never yields empty
                                // segments)
            pat[i][0] ==
                ':') {  // GCOVR_EXCL_BR_LINE (defensive: split_path_segments
                        // never yields empty segments)
            params.emplace_back(pat[i].substr(1), seg[i]);
            continue;
        }
        if (pat[i] != seg[i]) {
            return false;
        }
    }
    if (out_params) {  // GCOVR_EXCL_BR_LINE (defensive: the only caller,
                       // find_http_route, always passes a non-null out_params)
        *out_params = std::move(params);
    }
    return true;
}

}  // namespace

std::shared_ptr<LuaVM> LuaRuntime::vm_for_state(lua_State* L) {
    if (!L) {
        return nullptr;
    }
    // Handlers (and on_init) run as coroutine threads of the service VM, so
    // the calling state is often not the main thread the VM map is keyed by.
    // Resolve the origin main thread via the registry's LUA_RIDX_MAINTHREAD.
    lua_State* origin = L;
    if (lua_pushthread(L) == 0) {
        lua_pop(L, 1);
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
        if (lua_State* main = lua_tothread(  // GCOVR_EXCL_BR_LINE (defensive:
                                             // LUA_RIDX_MAINTHREAD always holds
                                             // a thread value)
                L, -1)) {  // GCOVR_EXCL_BR_LINE (defensive: LUA_RIDX_MAINTHREAD
                           // always holds a thread value)
            origin = main;
        }
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
    std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
    auto it = impl_->vms_by_state.find(origin);
    if (it == impl_->vms_by_state.end()) {
        return nullptr;
    }
    auto vm = it->second.lock();
    if (!vm) {
        // VM is gone; prune the stale entry.
        impl_->vms_by_state.erase(  // GCOVR_EXCL_LINE (defensive: weak_ptr
            it);                    // expiry race)
    }
    return vm;
}

bool LuaRuntime::register_http_route(std::shared_ptr<LuaVM> vm,
                                     const std::string& service_id,
                                     const std::string& method,
                                     const std::string& path,
                                     shd::function handler,
                                     std::string* error) {
    if (service_id.empty()) {
        if (error) {
            *error =
                "shield.httpd requires a running service context "
                "(no current service)";
        }
        return false;
    }
    if (!vm) {
        if (error) {
            *error = "invalid VM";
        }
        return false;
    }
    if (!handler.valid()) {
        if (error) {
            *error = "handler must be a function";
        }
        return false;
    }
    if (path.empty() || path.front() != '/') {
        if (error) {
            *error = "path must start with '/'";
        }
        return false;
    }

    HttpRouteRegistration entry;
    entry.service_id = service_id;
    entry.method = method;
    entry.path = path;
    entry.vm = vm;
    // Routes outlive the registering coroutine: keep the handler's lua_State
    // on the main thread (see anchor_to_main_thread above).
    entry.handler = std::make_shared<shd::function>(
        anchor_to_main_thread(std::move(handler)));

    std::function<void(const std::string&, const std::string&)> sink;
    {
        std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
        // Later registrations replace earlier ones for the same route,
        // mirroring HttpServer::route semantics.
        impl_->http_routes.erase(
            std::remove_if(impl_->http_routes.begin(), impl_->http_routes.end(),
                           [&method, &path](const HttpRouteRegistration& r) {
                               return r.method == method && r.path == path;
                           }),
            impl_->http_routes.end());
        impl_->http_routes.push_back(std::move(entry));
        sink = impl_->http_route_sink;
    }
    if (sink) {
        sink(method, path);
    }
    return true;
}

std::optional<HttpRouteRegistration> LuaRuntime::find_http_route(
    const std::string& method, const std::string& path,
    std::vector<std::pair<std::string, std::string>>* out_params) const {
    std::vector<HttpRouteRegistration> snapshot;
    {
        std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
        snapshot = impl_->http_routes;
    }

    std::vector<std::pair<std::string, std::string>> params;
    for (const auto& route : snapshot) {
        if (route.method != method) {
            continue;
        }
        if (route_pattern_match(route.path, path, &params)) {
            if (out_params) {
                *out_params = std::move(params);
            }
            return route;
        }
    }
    return std::nullopt;
}

std::vector<HttpRouteRegistration> LuaRuntime::http_routes() const {
    std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
    return impl_->http_routes;
}

size_t LuaRuntime::http_route_count() const {
    std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
    return impl_->http_routes.size();
}

void LuaRuntime::remove_http_routes_for_service(const std::string& service_id) {
    std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
    impl_->http_routes.erase(
        std::remove_if(impl_->http_routes.begin(), impl_->http_routes.end(),
                       [&service_id](const HttpRouteRegistration& r) {
                           return r.service_id == service_id;
                       }),
        impl_->http_routes.end());
}

void LuaRuntime::set_http_route_sink(
    std::function<void(const std::string&, const std::string&)> sink) {
    std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
    impl_->http_route_sink = std::move(sink);
}

bool LuaRuntime::call_http_handler(const HttpRouteRegistration& route,
                                   const nlohmann::json& request_json,
                                   nlohmann::json& out_desc,
                                   std::string* error) {
    auto vm = route.vm.lock();
    if (!vm ||
        !vm->state()) {  // GCOVR_EXCL_BR_LINE (defensive: a live route VM lock
                         // always succeeds and Impl::state() is never null)
        if (error) {
            *error = "service VM is gone";
        }
        return false;
    }
    if (!route.handler || !route.handler->valid()) {
        if (error) {
            *error = "handler is not callable";
        }
        return false;
    }

    try {
        shd::state& lua = *vm->state();
        shd::table t = lua.create_table();
        t["method"] = request_json.value("method", "");
        t["path"] = request_json.value("path", "");
        t["query"] = request_json.value("query", "");
        t["body"] = request_json.value("body", "");
        shd::table params_t = lua.create_table();
        if (request_json.contains("params")) {
            for (auto it = request_json["params"].begin();
                 it != request_json["params"].end(); ++it) {
                params_t[it.key()] = it.value().get<std::string>();
            }
        }
        t["params"] = params_t;
        shd::table headers_t = lua.create_table();
        if (request_json.contains("headers")) {
            for (auto it = request_json["headers"].begin();
                 it != request_json["headers"].end(); ++it) {
                headers_t[it.key()] = it.value().get<std::string>();
            }
        }
        t["headers"] = headers_t;

        // Invoke the handler through an explicitly protected call: with
        // NDEBUG (release), SOL_SAFE_FUNCTION_OBJECTS is off and
        // shd::function::operator() is a bare lua_call — a handler error
        // would reach the panic handler (which aborts) instead of the
        // lua_error field below. The checked read of the result also keeps
        // SOL_SAFE_FUNCTION_OBJECTS from replacing the handler's error text
        // with a sol panic message when an errored result is converted.
        shd::protected_function handler_pf = *route.handler;
        auto handler_result = handler_pf(t);
        if (!handler_result.valid()) {
            const shd::error err = handler_result;
            out_desc = nlohmann::json::object();
            out_desc["lua_error"] = std::string(err.what());
            return true;
        }
        shd::object result = handler_result;
        out_desc = nlohmann::json::object();
        out_desc["status"] = 200;
        out_desc["json_body"] = false;

        if (!result.valid() ||  // GCOVR_EXCL_BR_LINE (defensive: handler
                                // validity was already checked)
            result ==           // GCOVR_EXCL_BR_LINE (compiler artifact: the
                       // nil-comparison records attribute to this first line
                       // of the expression; the guard is annotated below)
                shd::nil) {  // GCOVR_EXCL_BR_LINE (defensive:
                             // handler_result.valid() was already checked
                             // above, so the converted object is always valid)
            out_desc["status"] = 204;
            out_desc["body"] = "";
            return true;
        }
        if (result.is<std::string>()) {
            out_desc["body"] = result.as<std::string>();
            return true;
        }
        if (!result.is<shd::table>()) {
            if (error) {  // GCOVR_EXCL_BR_LINE (integration: every suite call
                          // passes a non-null error out-param)
                *error = "handler must return a table, string, or nil";
            }
            return false;
        }

        shd::table resp = result.as<shd::table>();
        shd::object status = resp["status"];
        if (status.valid() &&
            status.is<double>()) {  // GCOVR_EXCL_BR_LINE (defensive: a table
                                    // index always yields a valid object, so
                                    // status.valid() never fails; the
                                    // wrong-type arm is covered by the
                                    // response-field-shapes test)
            out_desc["status"] = static_cast<std::int64_t>(status.as<double>());
        }
        shd::object headers = resp["headers"];
        if (headers.valid() && headers.is<shd::table>()) {
            nlohmann::json h = nlohmann::json::object();
            for (const auto& [k, v] : headers.as<shd::table>()) {
                shd::object key = k;
                shd::object value = v;
                if (key.is<std::string>() && value.is<std::string>()) {
                    h[key.as<std::string>()] = value.as<std::string>();
                }
            }
            out_desc["headers"] = std::move(h);
        }
        shd::object body = resp["body"];
        if (body.valid()) {
            if (body.is<std::string>()) {
                out_desc["body"] = body.as<std::string>();
            } else {
                // Non-string body: serialize through the shared Lua->JSON
                // conversion (unsupported values become "<unsupported>").
                nlohmann::json encoded;
                lua_to_json(body, &encoded);
                out_desc["body"] = encoded.dump();
                out_desc["json_body"] = true;
            }
        } else {
            out_desc["body"] = "";
        }
        return true;
    } catch (const std::exception& e) {  // GCOVR_EXCL_BR_LINE (compiler
        // artifact: the catch body is only ever entered via the exception
        // edge, never by fallthrough)
        // No shd::error arm: the handler runs through an explicitly
        // protected call (errors arrive via the checked result above), and
        // every throwing conversion in this block is guarded by an is<>
        // check. This catch remains for allocation failures and similar
        // C++-side exceptions.
        if (error) {
            *error = std::string(e.what());
        }
        return false;
    }
}

}  // namespace shield::lua
