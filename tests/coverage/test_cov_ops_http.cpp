#define BOOST_TEST_MODULE CovOpsHttp
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "shield/caf_initializer.hpp"
#ifdef SHIELD_ENABLE_CLUSTER
#include "shield/cluster/cluster_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_SERVER
#include "shield/server/server_manager.hpp"
#endif
#include "shield/config/config.hpp"
#include "shield/console/ops_http_handler.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/http_server.hpp"
#include "shield/plugin/plugin_host.hpp"

namespace {

namespace fs = std::filesystem;

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

uint16_t free_port() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acc(
        io, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0));
    return acc.local_endpoint().port();
}

// Opt-in per-request forensics (SHIELD_OPS_HTTP_TRACE=1). A hung request
// used to leave nothing in the boost log (its output only flushes when the
// 600s ctest kill lands), so the raw client annotates each phase straight
// to stderr — unbuffered, and therefore visible even mid-hang.
bool http_trace() {
    static const bool on = std::getenv("SHIELD_OPS_HTTP_TRACE") != nullptr;
    return on;
}

void trace_phase(const char* what, long long ms, const char* detail = "") {
    if (http_trace()) {
        std::fprintf(stderr, "[http-trace] %-12s %6lldms %s\n", what, ms,
                     detail);
        std::fflush(stderr);
    }
}

// Minimal HTTP/1.0 client: one fresh connection per request, read to EOF.
struct RawHttpClient {
    std::string host;
    uint16_t port = 0;

    void connect_target(const std::string& h, uint16_t p) {
        host = h;
        port = p;
    }

    std::string request(const std::string& raw,
                        std::chrono::milliseconds timeout) {
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket socket(io);
        const auto t0 = std::chrono::steady_clock::now();
        auto since = [t0] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                .count();
        };
        try {
            socket.connect(boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address(host), port));
        } catch (const std::exception& e) {
            trace_phase("connect", since(), e.what());
            return {};
        }
        trace_phase("connect", since());
        boost::system::error_code ec;
        boost::asio::write(socket, boost::asio::buffer(raw), ec);
        if (ec) {
            trace_phase("write", since(), ec.message().c_str());
            return {};
        }
        trace_phase("write", since());

        std::string response;
        char buf[4096];
        auto deadline = std::chrono::steady_clock::now() + timeout;
        // Non-blocking reads with an explicit would_block retry: the peer's
        // FIN (HTTP/1.0 close) leaves socket.available() == 0, so gating on
        // available() would spin until the deadline instead of detecting EOF.
        socket.non_blocking(true, ec);
        bool got_first = false;
        while (std::chrono::steady_clock::now() < deadline) {
            std::size_t n = socket.read_some(boost::asio::buffer(buf), ec);
            if (ec == boost::asio::error::would_block ||
                ec == boost::asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (ec || n == 0) {
                trace_phase(ec ? "read-err" : "eof", since(),
                            ec ? ec.message().c_str() : "");
                break;
            }
            if (!got_first) {
                got_first = true;
                trace_phase("first-byte", since());
            }
            response.append(buf, n);
        }
        if (!got_first) {
            trace_phase("DEADLINE", since(), "no bytes at all");
        }
        boost::system::error_code ignore;
        socket.close(ignore);
        trace_phase("done", since(), std::to_string(response.size()).c_str());
        return response;
    }

    std::string get(
        const std::string& path,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(8000)) {
        return request("GET " + path + " HTTP/1.0\r\nHost: cov\r\n\r\n",
                       timeout);
    }

    std::string post(
        const std::string& path, const std::string& body,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(8000)) {
        return request(
            "POST " + path +
                " HTTP/1.0\r\nHost: cov\r\n"
                "Content-Type: application/json\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" + body,
            timeout);
    }

    // POST with an Authorization: Bearer header; empty token sends none.
    std::string post_auth(
        const std::string& path, const std::string& body,
        const std::string& token,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(8000)) {
        std::string auth =
            token.empty() ? "" : "Authorization: Bearer " + token + "\r\n";
        return request(
            "POST " + path + " HTTP/1.0\r\nHost: cov\r\n" + auth +
                "Content-Type: application/json\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" + body,
            timeout);
    }

    // POST /ops/profile with the fixture token. Profile requests get a
    // generous client-side budget: the ops server dispatches synchronously
    // on its io thread, so on a starved two-core runner a slow dispatch
    // can push the response past the 8s default — and the aborted case
    // then unwinds its fixture with a mid-session teardown, the exact
    // shape behind the CI-only hang this suite once hit.
    std::string post_profile(const std::string& body) {
        return post_auth("/ops/profile", body, "prof-token",
                         std::chrono::milliseconds(30000));
    }

    static int status_code(const std::string& response) {
        // "HTTP/1.0 200 OK"
        auto pos = response.find(' ');
        if (pos == std::string::npos) return -1;
        return std::atoi(response.c_str() + pos + 1);
    }

    static std::string body(const std::string& response) {
        auto pos = response.find("\r\n\r\n");
        if (pos == std::string::npos) return {};
        return response.substr(pos + 4);
    }
};

struct OpsFixture {
    caf::actor_system_config caf_cfg;
    std::unique_ptr<caf::actor_system> system;
    std::unique_ptr<shield::lua::LuaRuntime> runtime;
    std::unique_ptr<shield::lua::LuaServiceManager> manager;
    std::unique_ptr<shield::net::HttpServer> server;
    std::unique_ptr<shield::console::OpsHttpHandler> handler;
    uint16_t port = 0;

    OpsFixture() {
        system = std::make_unique<caf::actor_system>(caf_cfg);
        runtime = std::make_unique<shield::lua::LuaRuntime>();
        manager =
            std::make_unique<shield::lua::LuaServiceManager>(*runtime, *system);

        // Give the plugins endpoint at least one instance to list.
        prepare_one_instance();

        shield::config::global_config().set("cov.ops.str",
                                            std::string("value"));

        // /ops/eval is opt-in + token-gated; the fixture enables it so the
        // eval cases can exercise the endpoint itself.
        shield::config::global_config().set("http.eval_enabled",
                                            std::string("true"));
        shield::config::global_config().set("http.eval_token",
                                            std::string("cov-token"));

        // /ops/profile follows the same opt-in discipline; the default
        // cooldown of 10s is switched off here so consecutive-case sessions
        // don't rate-limit each other (the 429 case flips it back on).
        shield::config::global_config().set("http.profile_enabled",
                                            std::string("true"));
        shield::config::global_config().set("http.profile_token",
                                            std::string("prof-token"));
        shield::config::global_config().set("http.profile_cooldown_seconds",
                                            std::string("0"));

        port = free_port();
        shield::net::HttpServerConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = port;
        server = std::make_unique<shield::net::HttpServer>(cfg);
        handler = std::make_unique<shield::console::OpsHttpHandler>(*manager,
                                                                    *runtime);
        handler->register_routes(*server);
        server->start();
        BOOST_REQUIRE(server->is_running());
    }

    ~OpsFixture() {
        if (server) server->stop();
    }

    void prepare_one_instance() {
        fs::path dir = fs::temp_directory_path() / "shield_cov_ops_plugins";
        fs::remove_all(dir);
        fs::create_directories(dir);
        auto& host = shield::plugin::global_host();
        std::string err;
        host.scan(dir.string());
        BOOST_REQUIRE_MESSAGE(host.catalog(err), err);
        shield::plugin::PluginConfig pc;
        pc.directory = dir.string();
        shield::plugin::InstanceDecl decl;
        decl.id = "cov_opt";
        decl.package = "absent.pkg";
        decl.required = false;
        pc.instances.push_back(decl);
        BOOST_REQUIRE_MESSAGE(host.plan_and_resolve(pc, err), err);
    }
};

}  // namespace

BOOST_FIXTURE_TEST_SUITE(OpsHttpTests, OpsFixture)

BOOST_AUTO_TEST_CASE(StatusEndpoint) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response =
        client.get("/ops/status", std::chrono::milliseconds(9000));
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    // No live service exists to host the ""-id task, so services times out.
    BOOST_CHECK(resp["data"]["services"] == "timeout");
    BOOST_CHECK(resp["data"]["plugins"].is_array());
    BOOST_CHECK(resp["data"]["plugins"].size() == 1);
}

BOOST_AUTO_TEST_CASE(ServicesListedWhenServiceIsAlive) {
    // With at least one live service, the ""-id ops task borrows its actor
    // and the services list resolves instead of timing out.
    const fs::path dir = fs::temp_directory_path() / "shield_cov_ops_svc";
    fs::create_directories(dir);
    std::ofstream(dir / "svc.lua") << "return { on_init = function() end }\n";
    auto spawned =
        manager->spawn((dir / "svc.lua").string(),
                       R"({"name":"cov_ops_svc","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);

    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response =
        client.get("/ops/services", std::chrono::milliseconds(9000));
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"].is_array());
    BOOST_CHECK(resp["data"].size() >= 1);

    manager->shutdown_all("done");
}

BOOST_AUTO_TEST_CASE(ServiceDetailEndpoint) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    // Spawn a real service first: the ""-id ops task needs a live service
    // actor to run on, so the 404 case below must also run after this.
    const fs::path dir = fs::temp_directory_path() / "shield_cov_ops_svc";
    fs::create_directories(dir);
    std::ofstream(dir / "detail_svc.lua")
        << "return { on_init = function() end }\n";
    auto spawned =
        manager->spawn((dir / "detail_svc.lua").string(),
                       R"({"name":"cov_detail_svc","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);

    // Unknown name under a live mesh -> 404 (route matched, service absent).
    std::string response =
        client.get("/ops/services/nope.svc", std::chrono::milliseconds(9000));
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 404);

    // Known name -> running snapshot with name/script/rpc_routes.
    response = client.get("/ops/services/cov_detail_svc",
                          std::chrono::milliseconds(9000));
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["name"], "cov_detail_svc");
    BOOST_CHECK_EQUAL(resp["data"]["state"], "running");
    // script is recorded only for config-defined runtime actors; a service
    // spawned here may legitimately omit it.
    BOOST_CHECK(!resp["data"].contains("script") ||
                resp["data"]["script"].is_string());
    BOOST_CHECK(resp["data"]["rpc_routes"].is_number_unsigned());
    // Per-incarnation stats: monotonic uptime since publish and the active
    // actor timer count (this service never schedules one).
    BOOST_CHECK(resp["data"]["uptime_seconds"].is_number());
    BOOST_CHECK(resp["data"]["uptime_seconds"].get<double>() >= 0.0);
    BOOST_CHECK_EQUAL(resp["data"]["timers"], 0u);

    manager->shutdown_all("done");
}

BOOST_AUTO_TEST_CASE(ServiceStatsMetrics) {
    // Per-service stats (traffic + uptime + active timers) ride the same
    // dispatch path as every message (send/call/system); drive the traffic
    // from C++ via send_system and observe them both through /ops/metrics
    // and the detail snapshot.
    const fs::path dir = fs::temp_directory_path() / "shield_cov_ops_svc";
    fs::create_directories(dir);
    std::ofstream(dir / "traffic_svc.lua")
        << "local M = {}\n"
           "function M.on_ping() return 'pong' end\n"
           "function M.on_boom() error('boom') end\n"
           "return M\n";
    auto spawned =
        manager->spawn((dir / "traffic_svc.lua").string(),
                       R"({"name":"cov_traffic_svc","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);

    // send_system only enqueues — the dispatch (and its counters) complete
    // asynchronously on the service actor, so the assertions below poll.
    std::string err;
    for (int i = 0; i < 3; ++i) {
        BOOST_REQUIRE(manager->send_system("cov_traffic_svc", "on_ping",
                                           nlohmann::json::array(), &err));
    }
    for (int i = 0; i < 2; ++i) {
        BOOST_REQUIRE(manager->send_system("cov_traffic_svc", "on_boom",
                                           nlohmann::json::array(), &err));
    }

    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    // /ops/services/:name carries the same counters plus uptime/timers.
    bool detail_ok = false;
    for (int i = 0; i < 100 && !detail_ok; ++i) {
        std::string response = client.get("/ops/services/cov_traffic_svc",
                                          std::chrono::milliseconds(9000));
        if (RawHttpClient::status_code(response) == 200) {
            auto resp = nlohmann::json::parse(RawHttpClient::body(response));
            detail_ok = resp["data"]["requests"] == 5 &&
                        resp["data"]["errors"] == 2 &&
                        resp["data"]["uptime_seconds"].is_number() &&
                        resp["data"]["uptime_seconds"].get<double>() >= 0.0 &&
                        resp["data"]["timers"] == 0;
        }
        if (!detail_ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    BOOST_CHECK(detail_ok);

    // /ops/metrics exposes the counters and the per-incarnation gauges.
    bool metrics_ok = false;
    std::string metrics_body;
    for (int i = 0; i < 100 && !metrics_ok; ++i) {
        std::string response =
            client.get("/ops/metrics", std::chrono::milliseconds(9000));
        metrics_body = RawHttpClient::body(response);
        metrics_ok = metrics_body.find(
                         "shield_service_requests_total{service="
                         "\"cov_traffic_svc\"} 5") != std::string::npos &&
                     metrics_body.find(
                         "shield_service_errors_total{service="
                         "\"cov_traffic_svc\"} 2") != std::string::npos &&
                     metrics_body.find(
                         "shield_service_timers{service="
                         "\"cov_traffic_svc\"} 0") != std::string::npos &&
                     metrics_body.find(
                         "shield_service_uptime_seconds{"
                         "service=\"cov_traffic_svc\"} ") != std::string::npos;
        if (!metrics_ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    BOOST_CHECK(metrics_ok);
    BOOST_CHECK(metrics_body.find("# TYPE shield_service_requests_total "
                                  "counter") != std::string::npos);
    BOOST_CHECK(metrics_body.find("# TYPE shield_service_uptime_seconds "
                                  "gauge") != std::string::npos);
    BOOST_CHECK(metrics_body.find("# TYPE shield_service_timers "
                                  "gauge") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ServiceTimersReported) {
    // A repeating shield.timer registered in on_init shows up as one active
    // timer in both the detail snapshot and the metrics export.
    const fs::path dir = fs::temp_directory_path() / "shield_cov_ops_svc";
    fs::create_directories(dir);
    std::ofstream(dir / "timer_svc.lua")
        << "return { on_init = function()\n"
           "  shield.timer(50, function() end)\n"
           "end }\n";
    auto spawned =
        manager->spawn((dir / "timer_svc.lua").string(),
                       R"({"name":"cov_timer_svc","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);

    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    bool detail_ok = false;
    for (int i = 0; i < 100 && !detail_ok; ++i) {
        std::string response = client.get("/ops/services/cov_timer_svc",
                                          std::chrono::milliseconds(9000));
        if (RawHttpClient::status_code(response) == 200) {
            auto resp = nlohmann::json::parse(RawHttpClient::body(response));
            detail_ok = resp["data"]["timers"] == 1 &&
                        resp["data"]["uptime_seconds"].is_number() &&
                        resp["data"]["uptime_seconds"].get<double>() >= 0.0;
        }
        if (!detail_ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    BOOST_CHECK(detail_ok);

    bool metrics_ok = false;
    for (int i = 0; i < 100 && !metrics_ok; ++i) {
        std::string response =
            client.get("/ops/metrics", std::chrono::milliseconds(9000));
        metrics_ok = RawHttpClient::body(response).find(
                         "shield_service_timers{service=\"cov_timer_svc\"} "
                         "1") != std::string::npos;
        if (!metrics_ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    BOOST_CHECK(metrics_ok);

    manager->shutdown_all("done");

    manager->shutdown_all("done");
}

BOOST_AUTO_TEST_CASE(PendingCallsAndTasksReported) {
    // The instantaneous gauges: a published handler suspended in a
    // coroutine-aware call shows up on the caller's entry (pending_calls,
    // observed live through service_stats/detail/metrics), and both gauge
    // families are constant fixtures of the export with a labeled sample
    // per service — pending_tasks included, whose nonzero window only
    // exists behind a busy actor and cannot be widened deterministically
    // from outside. The live observation polls the manager directly from
    // this thread: the HTTP detail route rides the same snapshot, but a
    // request-per-poll loop here measurably starves the forked-task lane
    // it observes (11s responses with both actors idle), so the endpoints
    // are asserted once, after the suspension has come and gone.
    const fs::path dir = fs::temp_directory_path() / "shield_cov_ops_svc";
    fs::create_directories(dir);
    std::ofstream(dir / "pending_slow.lua") << "return { on_work = function()\n"
                                               "  shield.sleep(800)\n"
                                               "  return 'done'\n"
                                               "end }\n";
    std::ofstream(dir / "pending_caller.lua")
        << "return { on_kick = function()\n"
           "  shield.call('cov_pending_slow', 'on_work')\n"
           "end }\n";
    auto spawned =
        manager->spawn((dir / "pending_slow.lua").string(),
                       R"({"name":"cov_pending_slow","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    spawned = manager->spawn(
        (dir / "pending_caller.lua").string(),
        R"({"name":"cov_pending_caller","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);

    // Wait for both incarnations to publish.
    bool published = false;
    for (int i = 0; i < 200 && !published; ++i) {
        auto stats = manager->service_stats();
        published = stats.count("cov_pending_caller") > 0 &&
                    stats.count("cov_pending_slow") > 0;
        if (!published) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(published);

    // Suspend the caller's handler in a coroutine-aware call and poll the
    // stats snapshot (registry-locked read, no traffic generated) until the
    // caller-side entry counts it.
    std::string err;
    BOOST_REQUIRE(manager->send_system("cov_pending_caller", "on_kick",
                                       nlohmann::json::array(), &err));
    bool saw_pending_call = false;
    for (int i = 0; i < 800 && !saw_pending_call; ++i) {
        auto stats = manager->service_stats();
        auto it = stats.find("cov_pending_caller");
        saw_pending_call = it != stats.end() && it->second.pending_calls == 1;
        if (!saw_pending_call) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    BOOST_CHECK(saw_pending_call);
    // While the call is suspended the caller holds exactly one entry; the
    // callee holds none (it is executing, not calling out).
    if (saw_pending_call) {
        auto stats = manager->service_stats();
        BOOST_CHECK_EQUAL(stats.at("cov_pending_slow").pending_calls, 0u);
        // The detail snapshot reads the same locked registry, so inside the
        // suspension window it reports the same live gauge (one shot from
        // this thread — no request loop, see the case comment above).
        auto detail = manager->service_detail("cov_pending_caller");
        BOOST_REQUIRE(detail.has_value());
        BOOST_CHECK_EQUAL((*detail)["pending_calls"], 1u);
    }
    // The entry is transient: once the callee answers, the caller is back
    // at zero (bounded wait — the callee sleeps 800ms total).
    bool drained = false;
    for (int i = 0; i < 1500 && !drained; ++i) {
        auto stats = manager->service_stats();
        auto it = stats.find("cov_pending_caller");
        drained = it != stats.end() && it->second.pending_calls == 0;
        if (!drained) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    BOOST_CHECK(drained);

    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    // Both fields ride the detail snapshot.
    bool detail_ok = false;
    for (int i = 0; i < 20 && !detail_ok; ++i) {
        std::string response = client.get("/ops/services/cov_pending_caller",
                                          std::chrono::milliseconds(9000));
        if (RawHttpClient::status_code(response) == 200) {
            auto resp = nlohmann::json::parse(RawHttpClient::body(response));
            detail_ok = resp["data"]["pending_calls"] == 0 &&
                        resp["data"]["pending_tasks"] == 0;
        }
        if (!detail_ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    BOOST_CHECK(detail_ok);

    // ... and the metrics export carries both families with one labeled
    // sample line per service (zero included).
    bool metrics_ok = false;
    std::string metrics_body;
    for (int i = 0; i < 20 && !metrics_ok; ++i) {
        std::string response =
            client.get("/ops/metrics", std::chrono::milliseconds(9000));
        metrics_body = RawHttpClient::body(response);
        metrics_ok =
            metrics_body.find(
                "# TYPE shield_service_pending_calls "
                "gauge") != std::string::npos &&
            metrics_body.find(
                "# TYPE shield_service_pending_tasks "
                "gauge") != std::string::npos &&
            metrics_body.find("shield_service_pending_calls{service=") !=
                std::string::npos &&
            metrics_body.find("shield_service_pending_tasks{service=") !=
                std::string::npos;
        if (!metrics_ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    BOOST_CHECK(metrics_ok);

    manager->shutdown_all("done");
}

BOOST_AUTO_TEST_CASE(CoroutinesAndMemoryReported) {
    // L1 gauges: a handler suspended in shield.sleep is exactly one live
    // coroutine on its service (registered at factory start, erased at the
    // terminal resume), and every dispatch exit samples the VM's Lua heap
    // into memory_kb (an empty Lua VM already reports a nonzero GCCOUNT).
    // Same observation discipline as PendingCallsAndTasksReported: poll the
    // manager from this thread, assert each HTTP endpoint once.
    const fs::path dir = fs::temp_directory_path() / "shield_cov_ops_svc";
    fs::create_directories(dir);
    std::ofstream(dir / "live_svc.lua") << "return { on_nap = function()\n"
                                           "  shield.sleep(700)\n"
                                           "  return 'awake'\n"
                                           "end }\n";
    auto spawned =
        manager->spawn((dir / "live_svc.lua").string(),
                       R"({"name":"cov_live_svc","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    bool published = false;
    for (int i = 0; i < 200 && !published; ++i) {
        published = manager->service_stats().count("cov_live_svc") > 0;
        if (!published) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(published);

    std::string err;
    BOOST_REQUIRE(manager->send_system("cov_live_svc", "on_nap",
                                       nlohmann::json::array(), &err));
    // Inside the suspension window the service holds exactly one live
    // coroutine; the sampling heap gauge is already populated by the same
    // dispatch's scope exit.
    bool saw_live = false;
    for (int i = 0; i < 700 && !saw_live; ++i) {
        auto stats = manager->service_stats();
        auto it = stats.find("cov_live_svc");
        saw_live = it != stats.end() && it->second.coroutines == 1 &&
                   it->second.memory_kb > 0;
        if (!saw_live) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    BOOST_CHECK(saw_live);
    if (saw_live) {
        // The detail snapshot reads the same locked registry.
        auto detail = manager->service_detail("cov_live_svc");
        BOOST_REQUIRE(detail.has_value());
        BOOST_CHECK_EQUAL((*detail)["coroutines"], 1u);
    }
    // The sleep resolves on its timer resume and the coroutine completes:
    // the gauge drains back to zero (bounded wait — the nap is 700ms).
    bool drained = false;
    for (int i = 0; i < 1500 && !drained; ++i) {
        auto stats = manager->service_stats();
        auto it = stats.find("cov_live_svc");
        drained = it != stats.end() && it->second.coroutines == 0;
        if (!drained) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    BOOST_CHECK(drained);

    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    bool detail_ok = false;
    for (int i = 0; i < 20 && !detail_ok; ++i) {
        std::string response = client.get("/ops/services/cov_live_svc",
                                          std::chrono::milliseconds(9000));
        if (RawHttpClient::status_code(response) == 200) {
            auto resp = nlohmann::json::parse(RawHttpClient::body(response));
            detail_ok = resp["data"]["coroutines"] == 0 &&
                        resp["data"]["memory_kb"].is_number_unsigned() &&
                        resp["data"]["memory_kb"].get<std::uint64_t>() > 0;
        }
        if (!detail_ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    BOOST_CHECK(detail_ok);

    // Both families ride the export with one labeled sample per service.
    bool metrics_ok = false;
    std::string metrics_body;
    for (int i = 0; i < 20 && !metrics_ok; ++i) {
        std::string response =
            client.get("/ops/metrics", std::chrono::milliseconds(9000));
        metrics_body = RawHttpClient::body(response);
        metrics_ok = metrics_body.find(
                         "# TYPE shield_service_coroutines "
                         "gauge") != std::string::npos &&
                     metrics_body.find(
                         "# TYPE shield_service_memory_kb "
                         "gauge") != std::string::npos &&
                     metrics_body.find(
                         "shield_service_coroutines{service="
                         "\"cov_live_svc\"} 0") != std::string::npos &&
                     metrics_body.find(
                         "shield_service_memory_kb{service="
                         "\"cov_live_svc\"} ") != std::string::npos;
        if (!metrics_ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    BOOST_CHECK(metrics_ok);

    manager->shutdown_all("done");
}

BOOST_AUTO_TEST_CASE(ServicesEndpointTimesOut) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response =
        client.get("/ops/services", std::chrono::milliseconds(9000));
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 504);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "error");
}

BOOST_AUTO_TEST_CASE(ServiceDetailEdgePaths) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    // No live service in this fixture: the ""-id ops task has no actor to
    // borrow, so a well-formed detail request times out into 504.
    std::string response =
        client.get("/ops/services/cov_absent", std::chrono::milliseconds(9000));
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 504);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "error");

    // With a live service the same route resolves; the query string must be
    // stripped before the ":name" segment is interpreted, so it rides along.
    const fs::path dir = fs::temp_directory_path() / "shield_cov_ops_svc";
    fs::create_directories(dir);
    std::ofstream(dir / "edge_svc.lua")
        << "return { on_init = function() end }\n";
    auto spawned =
        manager->spawn((dir / "edge_svc.lua").string(),
                       R"({"name":"cov_edge_svc","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    bool published = false;
    for (int i = 0; i < 200 && !published; ++i) {
        published = manager->service_stats().count("cov_edge_svc") > 0;
        if (!published) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(published);

    response = client.get("/ops/services/cov_edge_svc?verbose=1",
                          std::chrono::milliseconds(9000));
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["name"], "cov_edge_svc");

    manager->shutdown_all("done");
}

BOOST_AUTO_TEST_CASE(PluginsEndpoint) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response = client.get("/ops/plugins");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"].is_array());
    BOOST_CHECK(resp["data"].size() == 1);
    BOOST_CHECK(resp["data"][0]["id"] == "cov_opt");
}

BOOST_AUTO_TEST_CASE(ConfigEndpointVariants) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    // No key -> hint text.
    std::string response = client.get("/ops/config");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"].get<std::string>().find("key") !=
                std::string::npos);

    // Unknown key -> null data.
    response = client.get("/ops/config?key=nope.key");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"].is_null());

    // Known key -> value, trailing parameters are stripped.
    response = client.get("/ops/config?key=cov.ops.str&other=1");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"] == "value");
}

BOOST_AUTO_TEST_CASE(EvalEndpointVariants) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    // Missing Authorization header -> 401.
    std::string response =
        client.post_auth("/ops/eval", R"({"code": "return 1"})", "");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 401);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "error");

    // Wrong token -> 401 (and never executes the code).
    response = client.post_auth("/ops/eval", R"({"code": "return 1"})", "nope");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 401);

    // Invalid JSON body (authorized).
    response = client.post_auth("/ops/eval", "this is not json", "cov-token");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 400);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "error");

    // Missing 'code' field (authorized).
    response = client.post_auth("/ops/eval", R"({"nope": 1})", "cov-token");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 400);

    // Valid code -> 200 with data.
    response = client.post_auth("/ops/eval", R"({"code": "return 6 * 7"})",
                                "cov-token");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"] == nlohmann::json::array({42}));

    // Failing code -> 400 with error message.
    response = client.post_auth(
        "/ops/eval", R"json({"code": "error('bad code')"})json", "cov-token");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 400);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "error");
}

BOOST_AUTO_TEST_CASE(EvalVMIsRestricted) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    // The eval VM must not expose host-control libraries, even with a
    // valid token (defense in depth behind the auth gate).
    std::string response = client.post_auth(
        "/ops/eval",
        R"json({"code": "return type(os), type(os.execute), type(io), type(require), type(package), type(os.time)"})json",
        "cov-token");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"] ==
                nlohmann::json::array(
                    {"table", "nil", "nil", "nil", "nil", "function"}));
}

BOOST_AUTO_TEST_CASE(EvalDisabledByDefault) {
    // Flip the config off and build an independent server+handler: the eval
    // route must not be registered at all (404), while read endpoints stay.
    auto& cfg = shield::config::global_config();
    cfg.set("http.eval_enabled", std::string("false"));

    uint16_t p2 = free_port();
    shield::net::HttpServerConfig scfg;
    scfg.host = "127.0.0.1";
    scfg.port = p2;
    shield::net::HttpServer server2(scfg);
    shield::console::OpsHttpHandler handler2(*manager, *runtime);
    handler2.register_routes(server2);
    server2.start();

    RawHttpClient client;
    client.connect_target("127.0.0.1", p2);
    std::string response =
        client.post_auth("/ops/eval", R"({"code": "return 1"})", "cov-token");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 404);

    // Restore for later cases in this suite.
    cfg.set("http.eval_enabled", std::string("true"));
}

BOOST_AUTO_TEST_CASE(EvalWithoutTokenNotRegistered) {
    // eval_enabled=true without a token must refuse to register the route.
    auto& cfg = shield::config::global_config();
    cfg.set("http.eval_token", std::string(""));

    uint16_t p3 = free_port();
    shield::net::HttpServerConfig scfg;
    scfg.host = "127.0.0.1";
    scfg.port = p3;
    shield::net::HttpServer server3(scfg);
    shield::console::OpsHttpHandler handler3(*manager, *runtime);
    handler3.register_routes(server3);
    server3.start();

    RawHttpClient client;
    client.connect_target("127.0.0.1", p3);
    // No token configured -> route absent regardless of what is sent.
    std::string response =
        client.post_auth("/ops/eval", R"({"code": "return 1"})", "");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 404);
    response =
        client.post_auth("/ops/eval", R"({"code": "return 1"})", "anything");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 404);

    // Restore for later cases in this suite.
    cfg.set("http.eval_token", std::string("cov-token"));
}

BOOST_AUTO_TEST_CASE(ProfileDisabledByDefault) {
    // Same independent-server pattern as EvalDisabledByDefault: flipping
    // http.profile_enabled off must leave the route unregistered (404).
    auto& cfg = shield::config::global_config();
    cfg.set("http.profile_enabled", std::string("false"));

    uint16_t p2 = free_port();
    shield::net::HttpServerConfig scfg;
    scfg.host = "127.0.0.1";
    scfg.port = p2;
    shield::net::HttpServer server2(scfg);
    shield::console::OpsHttpHandler handler2(*manager, *runtime);
    handler2.register_routes(server2);
    server2.start();

    RawHttpClient client;
    client.connect_target("127.0.0.1", p2);
    std::string response = client.post_profile(R"({"action":"status"})");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 404);

    cfg.set("http.profile_enabled", std::string("true"));
}

BOOST_AUTO_TEST_CASE(ProfileWithoutTokenNotRegistered) {
    // profile_enabled=true with an empty token refuses to register.
    auto& cfg = shield::config::global_config();
    cfg.set("http.profile_token", std::string(""));

    uint16_t p3 = free_port();
    shield::net::HttpServerConfig scfg;
    scfg.host = "127.0.0.1";
    scfg.port = p3;
    shield::net::HttpServer server3(scfg);
    shield::console::OpsHttpHandler handler3(*manager, *runtime);
    handler3.register_routes(server3);
    server3.start();

    RawHttpClient client;
    client.connect_target("127.0.0.1", p3);
    std::string response =
        client.post_auth("/ops/profile", R"({"action":"status"})", "");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 404);
    response =
        client.post_auth("/ops/profile", R"({"action":"status"})", "anything");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 404);

    cfg.set("http.profile_token", std::string("prof-token"));
}

BOOST_AUTO_TEST_CASE(ProfileAuthGate) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response =
        client.post_auth("/ops/profile", R"({"action":"status"})", "");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 401);
    response = client.post_auth("/ops/profile", R"({"action":"status"})",
                                "wrong-token");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 401);
}

BOOST_AUTO_TEST_CASE(ProfileRequestValidation) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    const auto post = [&](const std::string& body) {
        return RawHttpClient::status_code(client.post_profile(body));
    };
    BOOST_CHECK_EQUAL(post("this is not json"), 400);
    BOOST_CHECK_EQUAL(post(R"({})"), 400);                  // no action
    BOOST_CHECK_EQUAL(post(R"({"action":123})"), 400);      // action not string
    BOOST_CHECK_EQUAL(post(R"({"action":"nope"})"), 400);   // unknown
    BOOST_CHECK_EQUAL(post(R"({"action":"start"})"), 400);  // no service
    BOOST_CHECK_EQUAL(post(R"({"action":"start","service":123})"),
                      400);  // not a string
    BOOST_CHECK_EQUAL(
        post(R"({"action":"start","service":"s","duration_ms":0})"), 400);
    BOOST_CHECK_EQUAL(
        post(R"({"action":"start","service":"s","duration_ms":60001})"), 400);
    BOOST_CHECK_EQUAL(
        post(R"({"action":"start","service":"s","duration_ms":"x"})"), 400);
    BOOST_CHECK_EQUAL(post(R"({"action":"start","service":"s","interval":0})"),
                      400);
    BOOST_CHECK_EQUAL(
        post(R"({"action":"start","service":"s","interval":"x"})"), 400);
}

BOOST_AUTO_TEST_CASE(ProfileStopWithoutSessionReportsSlowCalls) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    // stop/report are decoupled from the sampling session: with none
    // live they still answer — session meta plus the slow-call ring —
    // instead of 409ing.
    std::string response = client.post_profile(R"({"action":"stop"})");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    response = client.post_profile(R"({"action":"report"})");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"]["active"] == false);
    BOOST_CHECK(resp["data"]["slow_calls"]["recent"].is_array());
    BOOST_CHECK(resp["data"]["slow_calls"].contains("total_recorded"));
    // status always answers, also with no session.
    response = client.post_profile(R"({"action":"status"})");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"]["active"] == false);
    BOOST_CHECK(resp["data"]["slow_calls"]["recent"].is_array());
}

BOOST_AUTO_TEST_CASE(ProfileUnknownService404) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response =
        client.post_profile(R"({"action":"start","service":"nope.svc"})");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 404);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "error");
}

// A service whose on_burn method runs a long count loop: sampling and
// uninstall fork tasks serialize after it (fork-task FIFO), so a start ->
// burn -> stop sequence samples the loop deterministically.
// Directory name kept short: luaO_chunkname truncates short_src at
// LUA_IDSIZE (60), and the service name must stay inside that budget on
// long temp dirs (Windows runner temp alone is ~39 chars).
static const fs::path kBurnSvcDir = fs::temp_directory_path() / "phs";
static const char* kBurnScript =
    "local M = {}\n"
    "function M.on_init() end\n"
    "function M.on_burn()\n"
    "  local s = 0\n"
    "  for i = 1, 10000000 do s = s + i end\n"
    "  return s\n"
    "end\n"
    "return M\n";

static std::string spawn_burn_service(shield::lua::LuaServiceManager& manager,
                                      const std::string& name) {
    fs::create_directories(kBurnSvcDir);
    std::ofstream(kBurnSvcDir / (name + ".lua")) << kBurnScript;
    auto spawned =
        manager.spawn((kBurnSvcDir / (name + ".lua")).string(),
                      R"({"name":")" + name + R"(","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    return name;
}

BOOST_AUTO_TEST_CASE(ProfileHappyPathStartStatusStop) {
    const std::string svc = spawn_burn_service(*manager, "prof_happy_svc");
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    std::string response = client.post_profile(
        R"({"action":"start","service":"prof_happy_svc","duration_ms":60000,)"
        R"("interval":1000})");
    BOOST_REQUIRE(!response.empty());
    BOOST_REQUIRE_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"]["started"] == true);
    BOOST_CHECK_EQUAL(resp["data"]["service"], "prof_happy_svc");
    BOOST_CHECK_EQUAL(resp["data"]["duration_ms"], 60000);

    response = client.post_profile(R"({"action":"status"})");
    BOOST_REQUIRE_EQUAL(RawHttpClient::status_code(response), 200);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"]["active"] == true);
    BOOST_CHECK_EQUAL(resp["data"]["service"], "prof_happy_svc");

    // Burn while the session is armed: the on_burn message is queued after
    // the install fork task, so the loop runs with the count hook in place.
    std::string err;
    BOOST_REQUIRE(manager->send_system("prof_happy_svc", "on_burn",
                                       nlohmann::json::array(), &err));

    // Wait until the burn actually finished (a fork task queued behind it
    // only runs once the handler returned). The stop request waits at most
    // 2s for the uninstall — on a coverage build the interpreter needs
    // longer than that to chew the loop, so stopping mid-burn would flake
    // into a 504.
    std::promise<void> burned;
    BOOST_REQUIRE(manager->enqueue_forked_task("prof_happy_svc", [&burned] {
        burned.set_value();
    }) != 0);
    BOOST_CHECK(burned.get_future().wait_for(std::chrono::seconds(60)) ==
                std::future_status::ready);

    response = client.post_profile(R"({"action":"stop"})");
    BOOST_REQUIRE_EQUAL(RawHttpClient::status_code(response), 200);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["total_samples"].is_number_unsigned());
    BOOST_CHECK(resp["data"]["total_samples"] > 0U);
    BOOST_CHECK(resp["data"]["frames"].is_array());
    BOOST_CHECK(!resp["data"]["frames"].empty());

    // Hotspot attribution: the top-ranked frame must land in the fixture
    // script's busy loop (on_burn), and each level is ranked by hits.
    const auto& top = resp["data"]["frames"][0];
    // short_src is wrapped as [string "..."] and chunkid-truncated, so the
    // match stops before the ".lua" suffix.
    BOOST_CHECK(top["source"].get<std::string>().find("prof_happy_svc") !=
                std::string::npos);
    const auto& frames = resp["data"]["frames"];
    for (std::size_t i = 1; i < frames.size(); ++i) {
        BOOST_CHECK(frames[i - 1]["hits"].get<uint64_t>() >=
                    frames[i]["hits"].get<uint64_t>());
    }

    response = client.post_profile(R"({"action":"status"})");
    BOOST_REQUIRE_EQUAL(RawHttpClient::status_code(response), 200);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"]["active"] == false);

    manager->shutdown_all("done");
}

BOOST_AUTO_TEST_CASE(ProfileDuplicateStart409) {
    spawn_burn_service(*manager, "prof_dup_svc");
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response =
        client.post_profile(R"({"action":"start","service":"prof_dup_svc"})");
    BOOST_REQUIRE_EQUAL(RawHttpClient::status_code(response), 200);

    // Any second start (same or other service) hits the single-session 409.
    response =
        client.post_profile(R"({"action":"start","service":"prof_dup_svc"})");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 409);

    // The first session still works: status names it, stop ends it.
    response = client.post_profile(R"({"action":"status"})");
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"]["active"] == true);
    response = client.post_profile(R"({"action":"stop"})");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);

    manager->shutdown_all("done");
}

BOOST_AUTO_TEST_CASE(ProfileStartCooldown429) {
    auto& cfg = shield::config::global_config();
    cfg.set("http.profile_cooldown_seconds", std::string("60"));
    spawn_burn_service(*manager, "prof_cd_svc");
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    std::string response =
        client.post_profile(R"({"action":"start","service":"prof_cd_svc"})");
    BOOST_REQUIRE_EQUAL(RawHttpClient::status_code(response), 200);
    // The cooldown is checked before the manager arbitration: an immediate
    // second start is 429, not 409.
    response =
        client.post_profile(R"({"action":"start","service":"prof_cd_svc"})");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 429);

    cfg.set("http.profile_cooldown_seconds", std::string("0"));
    response = client.post_profile(R"({"action":"stop"})");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);

    manager->shutdown_all("done");
}

BOOST_AUTO_TEST_CASE(ProfileNegativeCooldownClampedToZero) {
    // A negative http.profile_cooldown_seconds is clamped to 0 (no
    // cooldown), so an immediate second start reaches the manager's
    // single-session arbitration (409) instead of the 429 gate.
    auto& cfg = shield::config::global_config();
    cfg.set("http.profile_cooldown_seconds", std::string("-5"));
    spawn_burn_service(*manager, "prof_negcd_svc");
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    std::string response =
        client.post_profile(R"({"action":"start","service":"prof_negcd_svc"})");
    BOOST_REQUIRE_EQUAL(RawHttpClient::status_code(response), 200);
    response =
        client.post_profile(R"({"action":"start","service":"prof_negcd_svc"})");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 409);

    cfg.set("http.profile_cooldown_seconds", std::string("0"));
    response = client.post_profile(R"({"action":"stop"})");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);

    manager->shutdown_all("done");
}

// Manager-level profile API paths the HTTP surface cannot reach: the null
// promise guard, stopping a service that owns no session (with and without
// an error out-param), and the stop racing an install task that has not
// run yet (sampler still absent -> the session settles as abandoned).
BOOST_AUTO_TEST_CASE(ProfileManagerDirectCallPaths) {
    // Null promise is rejected up front.
    BOOST_CHECK(
        manager->profile_start("svc", {}, nullptr) ==
        shield::lua::LuaServiceManager::ProfileStartResult::kDispatchLost);

    // No session anywhere: stopping any name fails, with or without an
    // error out-param.
    BOOST_CHECK(!manager->profile_stop("prof_none_svc", nullptr));
    std::string err;
    BOOST_CHECK(!manager->profile_stop("prof_none_svc", &err));
    BOOST_CHECK(!err.empty());

    spawn_burn_service(*manager, "prof_mdl_svc");
    shield::lua::ProfileSessionConfig config;
    config.service = "prof_mdl_svc";
    // Occupy the owner BEFORE starting: the install fork task (enqueued by
    // profile_start) queues behind the sleeper, so the stop below
    // deterministically sees no sampler yet and settles the start's
    // promise as abandoned instead of leaving the caller waiting on a
    // session that never armed.
    manager->enqueue_forked_task("prof_mdl_svc", [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    });
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto report = promise->get_future().share();
    BOOST_REQUIRE(manager->profile_start("prof_mdl_svc", config, promise) ==
                  shield::lua::LuaServiceManager::ProfileStartResult::kStarted);

    // With a session live, stopping a different service still fails.
    BOOST_CHECK(!manager->profile_stop("prof_other_svc", nullptr));

    // A second start while the first session is live (its install task
    // still queued behind the sleeper) is rejected by the arbitration.
    auto promise2 = std::make_shared<std::promise<nlohmann::json>>();
    BOOST_REQUIRE(
        manager->profile_start("prof_mdl_svc", config, promise2) ==
        shield::lua::LuaServiceManager::ProfileStartResult::kSessionActive);
    BOOST_CHECK(manager->profile_stop("prof_mdl_svc", nullptr));
    BOOST_REQUIRE(report.wait_for(std::chrono::seconds(10)) ==
                  std::future_status::ready);
    auto data = report.get();
    BOOST_CHECK(data["abandoned"] == true);
    BOOST_CHECK_EQUAL(data["service"], "prof_mdl_svc");

    // The session is gone; a repeat stop fails again. The sleeper and the
    // (now stale, no-op) install task keep the actor busy for a while; the
    // next section uses a different actor and shutdown_all waits without a
    // budget, so the leftover drain costs wall time but not correctness.
    BOOST_CHECK(!manager->profile_stop("prof_mdl_svc", nullptr));
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    // A service exit while a session is live fulfills the promise as
    // abandoned instead of hanging the caller's wait.
    spawn_burn_service(*manager, "prof_ab_svc");
    config.service = "prof_ab_svc";
    auto promise3 = std::make_shared<std::promise<nlohmann::json>>();
    auto report3 = promise3->get_future().share();
    BOOST_REQUIRE(manager->profile_start("prof_ab_svc", config, promise3) ==
                  shield::lua::LuaServiceManager::ProfileStartResult::kStarted);
    manager->shutdown_all("done");
    BOOST_REQUIRE(report3.wait_for(std::chrono::seconds(10)) ==
                  std::future_status::ready);
    auto abandoned = report3.get();
    BOOST_CHECK(abandoned["abandoned"] == true);
    BOOST_CHECK_EQUAL(abandoned["service"], "prof_ab_svc");
}

// The manager dying with a live sampling session must take the session's
// duration driver down with it: the fixture's actor system outlives the
// manager, and a driver left idling until its delayed tick would stall
// that teardown for the whole session duration (the CI-only hang shape —
// a case aborted mid-session used to unwind its fixture exactly so). The
// dtor collects the driver into its stop list and fulfills the start
// promise as abandoned.
BOOST_AUTO_TEST_CASE(ProfileManagerDtorAbandonsActiveSession) {
    spawn_burn_service(*manager, "prof_dtor_svc");
    shield::lua::ProfileSessionConfig config;
    config.duration_ms = 60000;  // a live bug would stall the dtor this long
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto report = promise->get_future();
    BOOST_REQUIRE(manager->profile_start("prof_dtor_svc", config, promise) ==
                  shield::lua::LuaServiceManager::ProfileStartResult::kStarted);

    const auto teardown_start = std::chrono::steady_clock::now();
    manager.reset();  // session still armed: the dtor must abandon it
    const auto teardown = std::chrono::steady_clock::now() - teardown_start;

    BOOST_REQUIRE(report.wait_for(std::chrono::seconds(5)) ==
                  std::future_status::ready);
    const auto result = report.get();
    BOOST_CHECK(result["abandoned"] == true);
    BOOST_CHECK_EQUAL(result["service"], "prof_dtor_svc");

    // Normal teardown is sub-second; a driver left behind makes the 60s
    // tick dominate. The margin stays clear of runner noise.
    BOOST_CHECK(teardown < std::chrono::seconds(20));
}

BOOST_AUTO_TEST_CASE(ProfileStopAfterNaturalExpiry409) {
    spawn_burn_service(*manager, "prof_exp_svc");
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response = client.post_profile(
        R"({"action":"start","service":"prof_exp_svc","duration_ms":200})");
    BOOST_REQUIRE_EQUAL(RawHttpClient::status_code(response), 200);

    // Let the duration driver expire the session, then a manual stop finds
    // nothing: it answers with session meta plus the ring (decoupled from
    // the sampling session), and status agrees.
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    response = client.post_profile(R"({"action":"stop"})");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto expired = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(expired["data"]["active"] == false);
    BOOST_CHECK(expired["data"]["slow_calls"]["recent"].is_array());
    response = client.post_profile(R"({"action":"status"})");
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"]["active"] == false);

    manager->shutdown_all("done");
}

BOOST_AUTO_TEST_CASE(ProfileIdleServiceZeroSamples) {
    // Semantics anchor: sampling is hook-driven, so an idle service (no
    // message, no bytecode) accumulates nothing — a suspended/not-running
    // VM is invisible to the sampler, not sampled as "in some default
    // frame".
    spawn_burn_service(*manager, "prof_idle_svc");
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response = client.post_profile(
        R"({"action":"start","service":"prof_idle_svc","duration_ms":60000})");
    BOOST_REQUIRE_EQUAL(RawHttpClient::status_code(response), 200);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    response = client.post_profile(R"({"action":"stop"})");
    BOOST_REQUIRE_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"]["total_samples"] == 0U);
    BOOST_CHECK(resp["data"]["frames"].empty());

    manager->shutdown_all("done");
}

BOOST_AUTO_TEST_CASE(ProfileOwnerBusy504) {
    spawn_burn_service(*manager, "prof_busy_svc");
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response = client.post_profile(
        R"({"action":"start","service":"prof_busy_svc","duration_ms":60000,
            "interval":100000})");
    BOOST_REQUIRE_EQUAL(RawHttpClient::status_code(response), 200);

    // Occupy the owner with a slow fork task: the uninstall (queued behind
    // it) cannot run within the HTTP bounded wait -> 504.
    manager->enqueue_forked_task("prof_busy_svc", [] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
    });
    response = client.post_profile(R"({"action":"stop"})");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 504);

    // Let the queue drain; the uninstall lands after the sleeper and the
    // report promise is fulfilled (dropped here) — a second stop then sees
    // no session and answers with session meta plus the ring.
    std::this_thread::sleep_for(std::chrono::seconds(4));
    response = client.post_profile(R"({"action":"stop"})");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto drained = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(drained["data"]["active"] == false);

    manager->shutdown_all("done");
}

// ---------------------------------------------------------------------------
// Branch-coverage additions (purely additive).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(StatusEndpointWithLiveServiceAndInstance) {
    // With a live service the /ops/status services task actually runs (its
    // lambda body executes) instead of timing out.
    const fs::path dir = fs::temp_directory_path() / "shield_cov_ops_svc2";
    fs::create_directories(dir);
    std::ofstream(dir / "svc.lua") << "return { on_init = function() end }\n";
    auto spawned =
        manager->spawn((dir / "svc.lua").string(),
                       R"({"name":"cov_ops_svc2","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);

    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response =
        client.get("/ops/status", std::chrono::milliseconds(2500));
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["services"].is_array());
    BOOST_CHECK(resp["data"]["services"].size() >= 1);
    BOOST_CHECK(resp["data"]["plugins"].is_array());
    BOOST_CHECK(resp["data"]["plugins"].size() == 1);

    manager->shutdown_all("done");
}

BOOST_AUTO_TEST_CASE(ConfigEndpointValueShapes) {
    auto& cfg = shield::config::global_config();
    cfg.set("cov3.esc",
            std::string("quote\" back\\slash new\nline uni\xc3\x97"));
    cfg.set("cov3.int", static_cast<int64_t>(-42));
    cfg.set("cov3.dbl", 2.5);
    cfg.set("cov3.flag", true);

    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    auto get_data = [&](const std::string& path) -> nlohmann::json {
        std::string response =
            client.get(path, std::chrono::milliseconds(1500));
        BOOST_REQUIRE(!response.empty());
        BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
        return nlohmann::json::parse(RawHttpClient::body(response))["data"];
    };

    // Values are returned through config::get() as strings.
    BOOST_CHECK_EQUAL(get_data("/ops/config?key=cov3.esc")
                          .get<std::string>()
                          .find("quote\" back\\slash"),
                      0u);
    BOOST_CHECK_EQUAL(get_data("/ops/config?key=cov3.int"), "-42");
    BOOST_CHECK_EQUAL(get_data("/ops/config?key=cov3.dbl"), "2.500000");
    BOOST_CHECK_EQUAL(get_data("/ops/config?key=cov3.flag"), "true");

    // A query string without any key= parameter falls back to the hint.
    BOOST_CHECK(
        get_data("/ops/config?other=1").get<std::string>().find("key") !=
        std::string::npos);
    // An empty key value also yields the hint.
    BOOST_CHECK(get_data("/ops/config?key=").get<std::string>().find("key") !=
                std::string::npos);
    // A key whose value serializes to an empty string yields null data.
    cfg.set("cov3.empty", std::string(""));
    BOOST_CHECK(get_data("/ops/config?key=cov3.empty").is_null());
}

BOOST_AUTO_TEST_CASE(EvalEndpointBodyDiversity) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    // Error messages containing escapable characters flow through the
    // error-response dump.
    std::string response = client.post_auth(
        "/ops/eval",
        R"json({"code": "error('boom \"quoted\" back\\slash new\nline ok'"})json",
        "cov-token");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 400);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("boom") !=
                std::string::npos);

    // Successful values with the same diversity.
    response = client.post_auth(
        "/ops/eval",
        R"json({"code": "return 'val \"q\" 123456789012345678901234567890'"})json",
        "cov-token");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"].size() == 1);
    BOOST_CHECK(resp["data"][0].get<std::string>().find("val \"q\"") == 0u);
}

#ifdef SHIELD_ENABLE_CLUSTER
// With a cluster manager installed, /ops/status carries the cluster block
// (node id, epoch, per-peer snapshot). Phantom manager: no transport.
BOOST_AUTO_TEST_CASE(StatusEndpointIncludesClusterBlock) {
    shield::cluster::ClusterConfig config;
    config.enabled = true;
    config.node_id = "cov-ops";
    config.listen_address = "127.0.0.1:0";
    config.peers = {"127.0.0.1:59995"};
    shield::cluster::ClusterManager cluster(config);
    cluster.start();
    cluster.on_handshake("127.0.0.1:59995", "node-b", 12);
    shield::cluster::set_global_cluster_manager(&cluster);

    {
        RawHttpClient client;
        client.connect_target("127.0.0.1", port);
        std::string response =
            client.get("/ops/status", std::chrono::milliseconds(9000));
        BOOST_REQUIRE(!response.empty());
        BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
        auto resp = nlohmann::json::parse(RawHttpClient::body(response));
        BOOST_CHECK(resp["type"] == "result");
        BOOST_CHECK_EQUAL(resp["data"]["cluster"]["node_id"], "cov-ops");
        BOOST_CHECK(resp["data"]["cluster"].contains("node_epoch"));
        // No transport registered in this fixture: the M5 counter block is
        // absent, and the adopted peer reports a numeric heartbeat age.
        BOOST_CHECK(!resp["data"]["cluster"].contains("connections"));
        BOOST_REQUIRE_EQUAL(resp["data"]["cluster"]["nodes"].size(), 1u);
        BOOST_CHECK_EQUAL(resp["data"]["cluster"]["nodes"][0]["node_id"],
                          "node-b");
        BOOST_CHECK_EQUAL(resp["data"]["cluster"]["nodes"][0]["state"],
                          "online");
        BOOST_CHECK(resp["data"]["cluster"]["nodes"][0]["heartbeat_age_ms"]
                        .is_number());
    }

    shield::cluster::set_global_cluster_manager(nullptr);
    cluster.stop();
}
#endif

BOOST_AUTO_TEST_CASE(HealthEndpoint) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response = client.get("/ops/health");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    const auto& data = resp["data"];
    // The fixture only carries an optional unavailable instance, so the
    // process reports ok; the probe never does a Lua actor round trip.
    BOOST_CHECK_EQUAL(data["status"], "ok");
    BOOST_CHECK(data["uptime"].is_number());
    BOOST_CHECK(data["uptime"].get<double>() >= 0.0);
    BOOST_CHECK_EQUAL(data["checks"]["core"]["status"], "ok");
    BOOST_CHECK_EQUAL(data["checks"]["plugins"]["status"], "ok");
    BOOST_CHECK_EQUAL(data["checks"]["plugins"]["required_down"], 0u);
    BOOST_CHECK_EQUAL(data["checks"]["plugins"]["started"], 0u);
}

BOOST_AUTO_TEST_CASE(MetricsEndpoint) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response = client.get("/ops/metrics");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    // Prometheus exposition format, not JSON.
    BOOST_CHECK(response.find("text/plain; version=0.0.4") !=
                std::string::npos);
    const std::string& body = RawHttpClient::body(response);
    BOOST_CHECK(body.find("# HELP shield_uptime_seconds") != std::string::npos);
    BOOST_CHECK(body.find("# TYPE shield_uptime_seconds gauge") !=
                std::string::npos);
    BOOST_CHECK(body.find("shield_plugin_instances{state=\"unavailable\"} 1") !=
                std::string::npos);
    BOOST_CHECK(body.find("# TYPE shield_plugin_instances gauge") !=
                std::string::npos);
}

#ifdef SHIELD_ENABLE_SERVER
// With a server manager installed the health probe carries the server
// check (running -> ok) and metrics expose the state machine gauge.
BOOST_AUTO_TEST_CASE(HealthAndMetricsIncludeServerBlock) {
    shield::server::ServerConfig config;
    config.name = "cov-ops-health";
    shield::server::ServerManager sm(config);
    sm.mark_ready();
    shield::server::ServerManager::set_global(&sm);

    {
        RawHttpClient client;
        client.connect_target("127.0.0.1", port);
        std::string response = client.get("/ops/health");
        BOOST_REQUIRE(!response.empty());
        auto resp = nlohmann::json::parse(RawHttpClient::body(response));
        BOOST_CHECK_EQUAL(resp["data"]["checks"]["server"]["status"], "ok");
        BOOST_CHECK_EQUAL(resp["data"]["checks"]["server"]["state"], "running");
        BOOST_CHECK_EQUAL(resp["data"]["status"], "ok");

        response = client.get("/ops/metrics");
        BOOST_REQUIRE(!response.empty());
        BOOST_CHECK(RawHttpClient::body(response).find(
                        "shield_server_state{state=\"running\"} 1") !=
                    std::string::npos);
    }

    shield::server::ServerManager::set_global(nullptr);
}
#endif

#ifdef SHIELD_ENABLE_CLUSTER
// Phantom cluster manager (no transport): health reports the online peer,
// metrics expose the node gauge without the transport counters.
BOOST_AUTO_TEST_CASE(HealthAndMetricsIncludeClusterBlock) {
    shield::cluster::ClusterConfig config;
    config.enabled = true;
    config.node_id = "cov-ops-health";
    config.listen_address = "127.0.0.1:0";
    config.peers = {"127.0.0.1:59995"};
    shield::cluster::ClusterManager cluster(config);
    cluster.start();
    cluster.on_handshake("127.0.0.1:59995", "node-b", 12);
    shield::cluster::set_global_cluster_manager(&cluster);

    {
        RawHttpClient client;
        client.connect_target("127.0.0.1", port);
        std::string response = client.get("/ops/health");
        BOOST_REQUIRE(!response.empty());
        auto resp = nlohmann::json::parse(RawHttpClient::body(response));
        BOOST_CHECK_EQUAL(resp["data"]["checks"]["cluster"]["status"], "ok");
        BOOST_CHECK_EQUAL(resp["data"]["checks"]["cluster"]["nodes_online"],
                          1u);
        BOOST_CHECK_EQUAL(resp["data"]["checks"]["cluster"]["nodes_down"], 0u);
        BOOST_CHECK_EQUAL(resp["data"]["status"], "ok");

        response = client.get("/ops/metrics");
        BOOST_REQUIRE(!response.empty());
        const std::string& body = RawHttpClient::body(response);
        BOOST_CHECK(body.find("shield_cluster_nodes{state=\"online\"} 1") !=
                    std::string::npos);
        // No transport registered in this fixture: counter block absent.
        BOOST_CHECK(body.find("shield_cluster_transport_messages_total") ==
                    std::string::npos);
    }

    shield::cluster::set_global_cluster_manager(nullptr);
    cluster.stop();
}
#endif

BOOST_AUTO_TEST_CASE(RequiredPluginDegradesHealth) {
    // A required instance whose package is present but library file missing
    // fails the load stage, stays in the instance table as "failed", and
    // flips the overall health verdict to degraded. (A required instance
    // whose package is missing entirely fails plan_and_resolve before
    // entering the table, so the load-stage failure is the reachable
    // degradation path.) Kept last: plan_and_resolve resets the shared
    // instance table, dropping the fixture's cov_opt instance.
    auto& host = shield::plugin::global_host();
    fs::path dir = fs::temp_directory_path() / "shield_cov_ops_plugins_req";
    fs::remove_all(dir);
    fs::create_directories(dir / "broken.req");
    std::ofstream(dir / "broken.req" / "manifest.yaml")
        << "schema_version: 1\n"
           "id: broken.req\n"
           "name: Broken\n"
           "version: 1.0.0\n"
           "kind: test\n"
           "entry: shield_plugin_get_v1\n"
           "library:\n"
           "  linux: bin/libdoes_not_exist.so\n"
           "  macos: bin/libdoes_not_exist.dylib\n"
           "  windows: bin/libdoes_not_exist.dll\n"
           "provides:\n"
           "  - interface: broken.req.iface\n"
           "requires: []\n"
           "config_schema:\n"
           "  type: object\n";
    std::string err;
    host.scan(dir.string());
    BOOST_REQUIRE_MESSAGE(host.catalog(err), err);
    shield::plugin::PluginConfig pc;
    pc.directory = dir.string();
    shield::plugin::InstanceDecl decl;
    decl.id = "cov_req";
    decl.package = "broken.req";
    decl.required = true;
    pc.instances.push_back(decl);
    BOOST_REQUIRE_MESSAGE(host.plan_and_resolve(pc, err), err);
    // The load stage fails on the missing library; the required instance is
    // marked failed but stays in the table for introspection.
    BOOST_CHECK_MESSAGE(!host.load_all(err), "load_all must fail");

    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response = client.get("/ops/health");
    BOOST_REQUIRE(!response.empty());
    // The degraded verdict maps onto the status code (ok -> 200,
    // degraded -> 503); the body envelope is unchanged either way.
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 503);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK_EQUAL(resp["data"]["status"], "degraded");
    BOOST_CHECK_EQUAL(resp["data"]["checks"]["plugins"]["status"], "degraded");
    BOOST_CHECK_EQUAL(resp["data"]["checks"]["plugins"]["required_down"], 1u);
    // Metrics reflect the same instance in its failed state.
    response = client.get("/ops/metrics");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK(RawHttpClient::body(response).find(
                    "shield_plugin_instances{state=\"failed\"} 1") !=
                std::string::npos);
}

#ifdef SHIELD_ENABLE_SERVER
// With a server manager installed, /ops/status carries the server block
// (state machine snapshot, read-only; OD-017 keeps /ops/server out).
BOOST_AUTO_TEST_CASE(StatusEndpointIncludesServerBlock) {
    shield::server::ServerConfig config;
    config.name = "cov-ops-node";
    config.info_name = "Cov Ops";
    config.info_version = "4.5.6";
    config.info_region = "cn-test";
    shield::server::ServerManager sm(config);
    sm.mark_ready();
    shield::server::ServerManager::set_global(&sm);

    {
        RawHttpClient client;
        client.connect_target("127.0.0.1", port);
        std::string response =
            client.get("/ops/status", std::chrono::milliseconds(9000));
        BOOST_REQUIRE(!response.empty());
        BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
        auto resp = nlohmann::json::parse(RawHttpClient::body(response));
        BOOST_CHECK(resp["type"] == "result");
        BOOST_CHECK_EQUAL(resp["data"]["server"]["state"], "running");
        BOOST_CHECK_EQUAL(resp["data"]["server"]["name"], "cov-ops-node");
        BOOST_CHECK_EQUAL(resp["data"]["server"]["version"], "4.5.6");
        BOOST_CHECK_EQUAL(resp["data"]["server"]["info"]["region"], "cn-test");
        BOOST_CHECK(resp["data"]["server"]["uptime_seconds"].is_number());
        BOOST_CHECK(resp["data"]["server"]["started_at_ms"].is_number());
        BOOST_CHECK_EQUAL(resp["data"]["server"]["watchers"], 0u);
        BOOST_CHECK(resp["data"]["server"]["shutdown_scheduled"] == false);
    }

    shield::server::ServerManager::set_global(nullptr);
}
#endif

BOOST_AUTO_TEST_CASE(ConfigDefinedActorCarriesScriptInDetail) {
    // service_detail's "script" field exists only for config-defined
    // runtime actors: the manager snapshots config's actor table into its
    // module_scripts at construction (mirroring bootstrap's per-actor
    // script resolution), so a manager built after injecting an actors
    // section reports the resolved path for a service spawned from it.
    // Config is process-global; the trailing reset keeps the injection
    // case-local.
    const fs::path dir = fs::temp_directory_path() / "shield_cov_ops_cfgactor";
    fs::create_directories(dir);
    const fs::path script = dir / "cfg_actor.lua";
    std::ofstream(script) << "return { on_init = function() end }\n";

    shield::config::reset_config();
    auto& g = shield::config::global_config();
    BOOST_REQUIRE(g.load_yaml_string(
        std::string("app:\n  name: covops\nnet:\n  threads: 1\nactors:\n") +
        "  - name: cov_cfg_actor\n    script: " + script.string()));
    {
        shield::lua::LuaServiceManager cfg_mgr(*runtime, *system);
        // Same spawn shape bootstrap uses for config actors: the resolved
        // script path plus {name, args, config} options.
        auto spawned =
            cfg_mgr.spawn(script.string(),
                          R"({"name":"cov_cfg_actor","args":{},"config":{}})");
        BOOST_REQUIRE(spawned.success);
        bool published = false;
        for (int i = 0; i < 200 && !published; ++i) {
            published = cfg_mgr.service_stats().count("cov_cfg_actor") > 0;
            if (!published) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        BOOST_REQUIRE(published);

        auto detail = cfg_mgr.service_detail("cov_cfg_actor");
        BOOST_REQUIRE(detail.has_value());
        BOOST_CHECK_EQUAL((*detail)["name"], "cov_cfg_actor");
        BOOST_CHECK_EQUAL((*detail)["script"], script.string());

        cfg_mgr.shutdown_all("done");
    }
    shield::config::reset_config();
}

BOOST_AUTO_TEST_CASE(StartedPluginInstanceInHealthAndMetrics) {
    // minimal.test is a build-tree fixture package (prebuilt .so, no
    // test-time compiler): drive it through global_host's staged pipeline
    // to "started" — the only state the health probe counts — then shut
    // the host down and scrape again with an empty instance table: the
    // grouped plugin emitter skips the whole family instead of emitting
    // headers with no samples. Kept last alongside the required-plugin
    // case: the scan/plan here resets the shared instance table.
    BOOST_REQUIRE(fs::exists("test_plugins/minimal.test/manifest.yaml"));
    auto& host = shield::plugin::global_host();
    std::string err;
    host.scan("test_plugins");
    BOOST_REQUIRE_MESSAGE(host.catalog(err), err);
    shield::plugin::PluginConfig pc;
    pc.directory = "test_plugins";
    shield::plugin::InstanceDecl decl;
    decl.id = "cov_started";
    decl.package = "minimal.test";
    decl.required = false;
    pc.instances.push_back(decl);
    BOOST_REQUIRE_MESSAGE(host.plan_and_resolve(pc, err), err);
    BOOST_REQUIRE_MESSAGE(host.load_all(err), err);
    BOOST_REQUIRE_MESSAGE(host.create_all(err), err);
    BOOST_REQUIRE_MESSAGE(host.start_all(err), err);

    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response = client.get("/ops/health");
    BOOST_REQUIRE(!response.empty());
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK_EQUAL(resp["data"]["status"], "ok");
    BOOST_CHECK_EQUAL(resp["data"]["checks"]["plugins"]["started"], 1u);
    BOOST_CHECK_EQUAL(resp["data"]["checks"]["plugins"]["required_down"], 0u);

    response = client.get("/ops/metrics");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK(RawHttpClient::body(response).find(
                    "shield_plugin_instances{state=\"started\"} 1") !=
                std::string::npos);

    // shutdown() marks instances stopped but keeps them in the table
    // (resolved vtables must stay valid until process exit), so the stopped
    // state still shows up as a sample...
    host.shutdown(100);
    response = client.get("/ops/metrics");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK(RawHttpClient::body(response).find(
                    "shield_plugin_instances{state=\"stopped\"} 1") !=
                std::string::npos);
    response = client.get("/ops/health");
    BOOST_REQUIRE(!response.empty());
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK_EQUAL(resp["data"]["checks"]["plugins"]["started"], 0u);

    // ...while the empty-sample path needs an explicit empty re-plan: it
    // resets the shared instance table, and the grouped emitter then skips
    // the whole family instead of emitting headers with no samples.
    shield::plugin::PluginConfig empty_pc;
    empty_pc.directory = "test_plugins";
    BOOST_REQUIRE_MESSAGE(host.plan_and_resolve(empty_pc, err), err);
    response = client.get("/ops/metrics");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK(RawHttpClient::body(response).find("shield_plugin_instances") ==
                std::string::npos);
}

// Branch coverage: a 'code' field that is present but not a string trips
// the second half of the body-validation disjunction ({"code": 123}),
// complementing the missing-field and valid-string shapes above.
BOOST_AUTO_TEST_CASE(EvalRejectsNonStringCode) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response =
        client.post_auth("/ops/eval", R"({"code": 123})", "cov-token");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 400);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("missing 'code'") !=
                std::string::npos);
}

// Slow-call metering (Phase B): the ring arms inside register_routes from
// http.slow_call_threshold_ms (non-numeric config logs and stays off, a
// real value arms the sticky process-wide gate), and a real cross-service
// call whose callee sleeps past the threshold must surface through the
// profile status slow_calls section. Registered last: the sticky gate is
// on from here on, but later cases don't make slow calls.
BOOST_AUTO_TEST_CASE(SlowCallGateArmsViaEndpointAndRecordsSlowSpan) {
    auto& cfg = shield::config::global_config();

    // Throwaway servers so the fixture's own registration stays untouched.
    cfg.set("http.slow_call_threshold_ms", std::string("bogus"));
    shield::net::HttpServerConfig scfg;
    scfg.host = "127.0.0.1";
    {
        scfg.port = free_port();
        shield::net::HttpServer srv(scfg);
        shield::console::OpsHttpHandler arm(*manager, *runtime);
        arm.register_routes(srv);  // stoull throws: logged, gate untouched
        srv.start();
        srv.stop();
    }
    cfg.set("http.slow_call_threshold_ms", std::string("20"));
    {
        scfg.port = free_port();
        shield::net::HttpServer srv(scfg);
        shield::console::OpsHttpHandler arm(*manager, *runtime);
        arm.register_routes(srv);  // arms the sticky gate at 20ms
        srv.start();
        srv.stop();
    }

    // callee sleeps 120ms per request; caller shield.calls it.
    const fs::path dir = fs::temp_directory_path() / "scg";
    fs::create_directories(dir);
    std::ofstream(dir / "scg_callee.lua")
        << "local M = {}\n"
           "function M.req(ctx) shield.sleep(120) return 'ok' end\n"
           "return M\n";
    std::ofstream(dir / "scg_caller.lua")
        << "local M = {}\n"
           "function M.kick(ctx, target)\n"
           "  local ok = shield.call_timeout(10000, target, 'req')\n"
           "  return ok\n"
           "end\n"
           "function M.kick_fast(ctx, target)\n"
           "  local ok = shield.call_timeout(50, target, 'req')\n"
           "  return ok\n"
           "end\n"
           "return M\n";
    auto callee =
        manager->spawn((dir / "scg_callee.lua").string(),
                       R"({"name":"scg_callee_svc","args":{},"config":{}})");
    BOOST_REQUIRE(callee.success);
    auto caller =
        manager->spawn((dir / "scg_caller.lua").string(),
                       R"({"name":"scg_caller_svc","args":{},"config":{}})");
    BOOST_REQUIRE(caller.success);

    std::string send_err;
    BOOST_CHECK(manager->send(caller.service_id, "kick",
                              nlohmann::json::array({callee.service_id}),
                              &send_err));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // A second callee slow enough to always blow the 50ms budget: the
    // call-timeout completion resumes the caller through the timeout
    // path, which must NOT land in the ring.
    std::ofstream(dir / "scg_timeout_callee.lua")
        << "local M = {}\n"
           "function M.req(ctx) shield.sleep(500) return 'late' end\n"
           "return M\n";
    auto slow_callee = manager->spawn(
        (dir / "scg_timeout_callee.lua").string(),
        R"({"name":"scg_timeout_callee","args":{},"config":{}})");
    BOOST_REQUIRE(slow_callee.success);
    BOOST_CHECK(manager->send(caller.service_id, "kick_fast",
                              nlohmann::json::array({slow_callee.service_id}),
                              &send_err));
    std::this_thread::sleep_for(std::chrono::milliseconds(900));

    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response = client.post_profile(R"({"action":"status"})");
    BOOST_REQUIRE_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    auto& sc = resp["data"]["slow_calls"];
    BOOST_CHECK(sc["recent"].is_array());
    BOOST_REQUIRE(!sc["recent"].empty());
    // Newest first: the span just recorded leads.
    BOOST_CHECK(sc["recent"][0]["caller"] == "scg_caller_svc");
    BOOST_CHECK(sc["recent"][0]["callee"] == "scg_callee_svc");
    BOOST_CHECK(sc["recent"][0]["ok"] == true);
    BOOST_CHECK(sc["recent"][0]["elapsed_ms"] >= 20);
    // The timed-out call is absent: timeout completions never record.
    for (const auto& rec : sc["recent"]) {
        BOOST_CHECK(rec["callee"] != "scg_timeout_callee");
    }

    manager->shutdown_all("done");
    cfg.set("http.slow_call_threshold_ms", std::string("0"));
}

BOOST_AUTO_TEST_SUITE_END()
