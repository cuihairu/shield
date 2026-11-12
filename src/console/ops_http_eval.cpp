// [SHIELD_CONSOLE] Ops /ops/eval and profiler control endpoints.
// Split out of ops_http_handler.cpp.
#include "shield/console/ops_http_handler.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <psapi.h>
#include <tlhelp32.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/slow_calls.hpp"
#include "shield/net/listener.hpp"
#include "shield/plugin/plugin_host.hpp"

#ifdef SHIELD_ENABLE_CLUSTER
#include "cluster_status.hpp"
#include "shield/cluster/cluster_manager.hpp"
#include "shield/cluster/cluster_transport.hpp"
#endif

#ifdef SHIELD_ENABLE_SERVER
#include "server_status.hpp"
#include "shield/server/server_manager.hpp"
#endif

#ifdef SHIELD_ENABLE_GLOBAL
#include "global_status.hpp"
#include "shield/global/global_manager.hpp"
#endif

namespace shield::console {

namespace {
/// The slow_calls section attached to /ops/profile status and report:
/// up to 16 newest samples plus the cumulative count that survives ring
/// wrap. Metering is decoupled from the sampling session, so this is
/// meaningful even while no session is live.
nlohmann::json slow_calls_section() {
    const auto snap = shield::lua::SlowCallRing::instance().snapshot();
    nlohmann::json recent = nlohmann::json::array();
    const std::size_t take = std::min<std::size_t>(snap.recent.size(), 16);
    for (std::size_t i = 0; i < take; ++i) {
        const auto& rec = snap.recent[i];
        recent.push_back(  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                           // nlohmann::json braced-init branches)
            {{"caller", rec.caller},
             {"callee", rec.callee},
             {"elapsed_ms", rec.elapsed_ms},
             {"ok", rec.ok},
             {"at", rec.at}});
    }
    // GCOVR_EXCL_START (compiler artifact: inlined nlohmann::json
    // braced-init branches)
    nlohmann::json section = {{"recent", recent},
                              {"total_recorded", snap.total_recorded}};
    // GCOVR_EXCL_STOP
    return section;
}
}  // namespace

shield::net::HttpResponse OpsHttpHandler::handle_eval(
    const shield::net::HttpRequest& req) {
    // Bearer token gate. Accept only "Authorization: Bearer <token>".
    // Written as a plain if instead of a ternary: the inlined string
    // constructions of the ternary arms produced pseudo line entries the
    // counters never landed on (coverage artifact).
    if (!token_matches(bearer_token(req), eval_token_)) {
        return make_error_response(401, "unauthorized");
    }

    // Parse JSON body
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body());
    } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // catch-entry pseudo-arc)
                 e) {               // (compiler artifact: catch-entry
                                    // covered by the invalid-JSON
                                    // case, missed fallthrough edge
                                    // is unwind bookkeeping)
        return make_error_response(400, "invalid JSON body");
    }

    if (!body.contains("code") || !body["code"].is_string()) {
        return make_error_response(400, "missing 'code' field");
    }

    std::string code = body["code"].get<std::string>();

    // Execute in a restricted VM: the token gate is the first line of
    // defense, the VM strip (no os.execute/io/require) the second.
    auto vm = lua_rt_.create_vm();
    lua_rt_.register_api(vm);
    lua_rt_.restrict_vm(vm);

    nlohmann::json result;
    std::string error;
    // Instruction budget on: /ops/eval is machine-facing code entry and must
    // not hang the ops io thread on a snippet that never returns.
    bool ok = lua_rt_.exec_lua(vm, code, &result, &error, true);

    // VM will be automatically destroyed when shared_ptr goes out of scope
    vm.reset();

    if (ok) {
        return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // inlined nlohmann::json braced-init
                                    // branches)
            200, {{"type", "result"},
                  {"data",
                   result}});  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                               // nlohmann::json braced-init branches)
    }
    return make_error_response(400, error);
}

shield::net::HttpResponse OpsHttpHandler::handle_profile(
    const shield::net::HttpRequest& req) {
    // Bearer token gate (same shape as /ops/eval, separate token).
    if (!token_matches(bearer_token(req), profile_token_)) {
        return make_error_response(401, "unauthorized");
    }

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body());
    } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // catch-entry pseudo-arc)
                 e) {
        return make_error_response(400, "invalid JSON body");
    }
    if (!body.contains("action") || !body["action"].is_string()) {
        return make_error_response(400, "missing 'action' field");
    }
    const std::string action = body["action"].get<std::string>();

    // status: pure registry read, always answers.
    if (action == "status") {
        nlohmann::json data;
        if (auto info = lua_mgr_.profile_status()) {
            data = {{"active", true},
                    {"service", info->service_id},
                    {"elapsed_ms", info->elapsed_ms},
                    {"duration_ms",
                     info->duration_ms}};  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: braced-init arcs)
        } else {
            data = {{"active", false}};  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: braced-init arcs)
        }
        data["slow_calls"] = slow_calls_section();
        return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler
                                    // artifact: inline call throw arc)
            200, {{"type", "result"},
                  {"data", data}});  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                     // inline throw arc)
    }  // GCOVR_EXCL_LINE (compiler artifact: block-end attribution)

    if (action == "start") {
        if (!body.contains("service") || !body["service"].is_string()) {
            return make_error_response(400, "missing 'service' field");
        }
        const std::string service = body["service"].get<std::string>();

        shield::lua::ProfileSessionConfig config;
        config.service = service;
        if (body.contains("duration_ms")) {
            if (!body["duration_ms"].is_number_unsigned() ||
                body["duration_ms"] == 0 ||
                body["duration_ms"].get<uint64_t>() > 60000) {
                return make_error_response(400, "duration_ms must be 1..60000");
            }
            config.duration_ms = body["duration_ms"].get<uint64_t>();
        }
        if (body.contains("interval")) {
            if (!body["interval"].is_number_unsigned() ||
                body["interval"] == 0) {
                return make_error_response(400, "interval must be >= 1");
            }
            config.interval = body["interval"].get<uint64_t>();
        }

        // Start cooldown (http.profile_cooldown_seconds, default 10):
        // rate-limits sessions beyond the manager's single-session 409.
        int cooldown_s = 10;
        try {
            cooldown_s = std::stoi(
                shield::config::get("http.profile_cooldown_seconds", "10"));
        } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (compiler
                                        // artifact: catch-entry pseudo-arc)
                     e) {
            cooldown_s = 10;  // non-numeric config: keep the default
        }
        if (cooldown_s < 0) {
            cooldown_s = 0;
        }
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(profile_mu_);
            if (profile_started_once_ &&
                now - last_profile_start_ < std::chrono::seconds(cooldown_s)) {
                return make_error_response(429,
                                           "profile start cooldown active");
            }
        }

        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        auto report = promise->get_future().share();
        const auto result = lua_mgr_.profile_start(service, config, promise);
        switch (result) {  // GCOVR_EXCL_BR_LINE (race: jump-table arc into the
                           // DispatchLost rollback (actor-gone window
                           // untestable end-to-end))
            case shield::lua::LuaServiceManager::ProfileStartResult::
                kServiceNotFound:
            case shield::lua::LuaServiceManager::ProfileStartResult::
                kDispatchLost:  // GCOVR_EXCL_BR_LINE (race: the manager's
                                // enqueue can only fail if the service actor
                                // vanished between the manager's own check and
                                // the dispatch)
                return make_error_response(404,
                                           "service not found: " + service);
            case shield::lua::LuaServiceManager::ProfileStartResult::
                kSessionActive:
                return make_error_response(409, "profile session active");
            case shield::lua::LuaServiceManager::ProfileStartResult::kStarted:
                break;
        }
        {
            std::lock_guard lock(profile_mu_);
            profile_report_ = std::move(report);
            last_profile_start_ = now;
            profile_started_once_ = true;
        }
        return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // inlined nlohmann::json braced-init
                                    // branches)
            200, {{"type", "result"},
                  {"data",
                   {{"started", true},
                    {"service", service},
                    {"duration_ms", config.duration_ms},
                    {"interval", config.interval}}}});
    }

    if (action == "stop" || action == "report") {
        // Both end the session and return the report (report is the
        // read-named alias; a later start re-arms from scratch).
        auto info = lua_mgr_.profile_status();
        if (!info) {
            // Slow-call metering is decoupled from the sampling session:
            // with no session live, report still answers — session meta
            // plus whatever the ring holds — instead of 409ing.
            return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler
                                        // artifact: inlined nlohmann::json
                                        // braced-init branches)
                200,
                {{"type", "result"},
                 {"data",
                  {{"active", false}, {"slow_calls", slow_calls_section()}}}});
        }
        std::string stop_error;
        if (!lua_mgr_.profile_stop(  // GCOVR_EXCL_BR_LINE (race: the session
                                     // can only end between the status read and
                                     // this stop via exit cleanup / duration
                                     // expiry; no test races them)
                info->service_id, &stop_error)) {
            // GCOVR_EXCL_START (race: the session can only end between the
            // status read and this stop via exit cleanup / duration expiry;
            // no test races them)
            // The session ended between the status read and the stop (exit
            // cleanup or duration expiry owns the promise now).
            return make_error_response(409, "no active profile session");
            // GCOVR_EXCL_STOP
        }
        std::shared_future<nlohmann::json> report;
        {
            std::lock_guard lock(profile_mu_);
            report = profile_report_;
        }
        if (report.valid() &&  // GCOVR_EXCL_BR_LINE (defensive: the stop
                               // path only runs after a start on this handler,
                               // which always stores a valid shared_future)
            report.wait_for(std::chrono::seconds(2)) ==
                std::future_status::ready) {
            auto data = report.get();
            data["active"] = true;
            data["slow_calls"] = slow_calls_section();
            return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler
                                        // artifact: inlined nlohmann::json
                                        // braced-init branches)
                200, {{"type", "result"}, {"data", data}});
        }  // GCOVR_EXCL_LINE (compiler artifact: block-end attribution)
        return make_error_response(504,
                                   "profile dispatch timeout (owner busy)");
    }

    return make_error_response(400, "unknown action: " + action);
}

}  // namespace shield::console
