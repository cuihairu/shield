// [SHIELD_CONSOLE] HTTP ops endpoints implementation (core): route
// registration, the bearer-token auth gate and the JSON response
// helpers. Domain handlers live in the sibling translation units.
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

OpsHttpHandler::OpsHttpHandler(shield::lua::LuaServiceManager& lua_mgr,
                               shield::lua::LuaRuntime& lua_rt)
    : lua_mgr_(lua_mgr), lua_rt_(lua_rt) {}

void OpsHttpHandler::register_routes(shield::net::HttpServer& server) {
    // Ops-wide auth gate. Default-deny: every /ops/* endpoint below (except
    // the k8s-probe-exempt /ops/health) is denied to requests without a
    // matching "Authorization: Bearer <token>", whether or not
    // http.ops_auth_token is configured. The only way to leave the gate off
    // is an explicit opt-out: http.ops_allow_unauthenticated=true (a trusted
    // local deployment that deliberately exposes the read-only endpoints
    // without a bearer token). This is the reverse of the previous legacy
    // behaviour, where an empty token silently left every endpoint open.
    auth_token_ = shield::config::get("http.ops_auth_token", "");
    allow_unauthenticated_ =
        shield::config::get("http.ops_allow_unauthenticated", "false") ==
        "true";

    // Static frontend root (FIX-WEB-04). When configured, the built console
    // is served from this directory; unmatched non-API GETs fall through to
    // the SPA index.html (handle_default). Empty means the embedded
    // raw-string single page remains the fallback (no filesystem access).
    static_root_ = shield::config::get("http.static_root", "");
    if (!static_root_.empty()) {
        server.set_static_root(static_root_);
        // Static assets and the SPA shell must be loadable by a normal
        // browser navigation, which cannot attach an Authorization header.
        // They contain no process data; the data APIs below remain behind
        // the ops token, and the SPA renders LoginGate on its first 401.
    }

    // Wrap a read-only handler with the ops-wide auth check. The gate is on
    // by default; an explicit http.ops_allow_unauthenticated=true is the only
    // way to bypass it. When a token is configured it must match; with no
    // token the gate still denies (default-deny) so an accidentally blank
    // config never re-opens the endpoint.
    auto authed = [this](shield::net::HttpHandler inner) {
        if (allow_unauthenticated_) {
            return inner;
        }
        return shield::net::HttpHandler{[this, inner](
                                            const shield::net::HttpRequest& req)
                                            -> shield::net::HttpResponse {
            if (auth_token_.empty() || !request_authorized(req, auth_token_)) {
                return make_error_response(401, "unauthorized");
            }
            return inner(req);
        }};
    };

    // Control handlers (/ops/eval, /ops/profile) are explicit opt-ins with
    // their own independent token gates (http.eval_token / http.profile_token).
    // They are NOT part of the read-only ops surface, so the ops-wide gate
    // must not be layered on top: a single Authorization header can only carry
    // one token, and requiring it to equal BOTH the ops token and a different
    // endpoint token would make the endpoint permanently 401 when the two
    // tokens differ. The endpoint's own token is the authorization for these
    // dangerous operations.

    // /ops/health is exempt from the ops-wide gate: k8s liveness/readiness
    // probes are unauthenticated by design. It only exposes process-local
    // health state, never secrets or configuration.
    server.get("/ops/health",
               [this](const auto& req) { return handle_health(req); });
    server.get("/ops/status",
               authed([this](const auto& req) { return handle_status(req); }));
    server.get("/ops/metrics",
               authed([this](const auto& req) { return handle_metrics(req); }));
    server.get("/ops/services", authed([this](const auto& req) {
                   return handle_services(req);
               }));
    // ":name" pattern matches only when the exact "/ops/services" route
    // missed, so list vs. detail never shadow each other.
    server.get("/ops/services/:name", authed([this](const auto& req) {
                   return handle_service_detail(req);
               }));
    server.get("/ops/plugins",
               authed([this](const auto& req) { return handle_plugins(req); }));
    server.get("/ops/config",
               authed([this](const auto& req) { return handle_config(req); }));

    // Embedded single-page dashboard: one self-contained HTML document (no
    // external assets, no build step, no new dependencies) that polls the
    // read-only /ops/* endpoints above and renders health, per-service
    // state, metric curves and profile results client-side. Registered on
    // both "/" and "/ops/dashboard" only when no static root is configured;
    // with a static root the default handler serves the built console
    // instead (same-origin, FIX-WEB-04).
    if (static_root_.empty()) {
        server.get("/", authed([this](const auto& req) {
                       return handle_dashboard(req);
                   }));
        server.get("/ops/dashboard", authed([this](const auto& req) {
                       return handle_dashboard(req);
                   }));
    } else {
        // With a filesystem SPA, these shell routes must be public so the
        // browser can load the app and its LoginGate. API requests still
        // require the Bearer token.
        server.get("/",
                   [this](const auto& req) { return handle_default(req); });
        server.get("/ops/dashboard",
                   [this](const auto& req) { return handle_default(req); });
    }

    // SPA fallback: unknown non-API paths serve the console index.html so a
    // deep link refreshes correctly; unknown /ops/* API paths get a clear
    // JSON 404 instead of HTML. Only the default handler sees unmatched
    // routes, so it runs after static serving (see HttpServer::dispatch).
    // With a static root the shell is public (browser navigation cannot
    // attach an Authorization header); with the embedded dashboard it stays
    // behind the ops-wide gate.
    if (static_root_.empty()) {
        server.set_default_handler(
            authed([this](const auto& req) { return handle_default(req); }));
    } else {
        server.set_default_handler(
            [this](const auto& req) { return handle_default(req); });
    }

    // /ops/eval is a remote-code-entry point: opt-in (http.eval_enabled),
    // token-gated (http.eval_token), and served in a restricted VM. The
    // route simply stays unregistered otherwise, so the endpoint 404s.
    if (shield::config::get("http.eval_enabled", "false") != "true") {
        auto& log = shield::log::get_logger("ops");
        SHIELD_LOG_INFO(log,
                        "/ops/eval disabled (opt in via http.eval_enabled=true "
                        "+ http.eval_token)");
    } else {
        eval_token_ = shield::config::get("http.eval_token", "");
        if (eval_token_.empty()) {
            auto& log = shield::log::get_logger("ops");
            SHIELD_LOG_ERROR(log,
                             "/ops/eval NOT registered: http.eval_enabled=true "
                             "requires a non-empty http.eval_token");
        } else {
            server.post("/ops/eval",
                        [this](const auto& req) { return handle_eval(req); });
        }
    }

    // /ops/profile exposes code locations and hit counts only (no payload,
    // no keys), but it still drives service VMs: same opt-in + token
    // discipline as /ops/eval, with a start cooldown on top.
    if (shield::config::get("http.profile_enabled", "false") != "true") {
        auto& log = shield::log::get_logger("ops");
        SHIELD_LOG_INFO(log,
                        "/ops/profile disabled (opt in via "
                        "http.profile_enabled=true + http.profile_token)");
        return;
    }
    profile_token_ = shield::config::get("http.profile_token", "");
    if (profile_token_.empty()) {
        auto& log = shield::log::get_logger("ops");
        SHIELD_LOG_ERROR(
            log,
            "/ops/profile NOT registered: http.profile_enabled=true requires "
            "a non-empty http.profile_token");
        return;
    }
    server.post("/ops/profile",
                [this](const auto& req) { return handle_profile(req); });

    // Phase B slow-call metering arms with the endpoint (the only reader
    // of the ring), at a fixed threshold for the process lifetime: the
    // gate is sticky by design — a call suspended while armed must always
    // find its completion path still metering, so it never disarms (see
    // SlowCallRing's class note). Zero config keeps the gate closed.
    const std::string slow_ms =
        shield::config::get("http.slow_call_threshold_ms", "0");
    if (slow_ms != "0") {
        try {
            shield::lua::SlowCallRing::instance().enable(
                static_cast<uint64_t>(std::stoull(slow_ms)));
        } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (compiler
                                        // artifact: catch-entry pseudo-arc)
                     e) {
            auto& log = shield::log::get_logger("ops");
            SHIELD_LOG_ERROR(log, "http.slow_call_threshold_ms not a number: " +
                                      slow_ms + " (slow-call metering off)");
        }
    }
}

bool OpsHttpHandler::token_matches(const std::string& provided,
                                   const std::string& expected) {
    // A length mismatch only leaks the length, never token content; equal
    // lengths compare over every byte so timing reveals nothing.
    if (provided.size() != expected.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        diff |= static_cast<unsigned char>(provided[i]) ^
                static_cast<unsigned char>(expected[i]);
    }
    return diff == 0;
}

std::string OpsHttpHandler::bearer_token(const shield::net::HttpRequest& req) {
    auto auth_it = req.find(boost::beast::http::field::authorization);
    std::string provided;
    if (auth_it != req.end()) {
        provided = std::string(auth_it->value());
    }
    constexpr char kBearerPrefix[] = "Bearer ";
    if (provided.rfind(kBearerPrefix, 0) == 0) {
        provided = provided.substr(sizeof(kBearerPrefix) - 1);
    }
    return provided;
}

bool OpsHttpHandler::request_authorized(const shield::net::HttpRequest& req,
                                        const std::string& expected) {
    // Constant-time compare: length mismatch returns immediately (only
    // leaks the length, never content); equal lengths compare over every
    // byte so timing reveals nothing about the expected token.
    return token_matches(bearer_token(req), expected);
}

shield::net::HttpResponse OpsHttpHandler::make_json_response(
    int status_code, const nlohmann::json& data) {
    shield::net::HttpResponse resp;
    resp.result(static_cast<boost::beast::http::status>(status_code));
    resp.set(boost::beast::http::field::content_type, "application/json");
    resp.body() = data.dump(2);
    resp.prepare_payload();
    return resp;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

shield::net::HttpResponse OpsHttpHandler::make_error_response(
    int status_code, const std::string& message) {
    return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined nlohmann::json braced-init branches)
        status_code,  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                      // nlohmann::json braced-init branches)
        {{"type", "error"}, {"message", message}});
}

}  // namespace shield::console
