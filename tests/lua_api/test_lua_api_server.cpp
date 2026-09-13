// LAPI-SV: shield_server P0 — the shield.server facade against a real
// dispatch context: the read-only info surface, the set_state validation
// matrix, watcher registration with asynchronous delivery through the
// system-message channel (including idempotent re-watch and unwatch), and
// the shutdown handover argument/duplicate matrix.
//
// When SHIELD_ENABLE_SERVER is off, only the stub case compiles: every
// shield.server.* entry reports module_unavailable instead of being nil.
#define BOOST_TEST_MODULE LuaApiServerTests
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {

const std::string SERVER_SCRIPT = "../tests/lua_api/scripts/server_service.lua";

nlohmann::json service_opts(const std::string& name) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
}

bool wait_until(std::function<bool()> predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

CallResult call(LuaServiceManager& manager, const std::string& service,
                const std::string& method, nlohmann::json args) {
    return manager.call(service, method, std::move(args), 5000);
}

// The watch callback log table serialized by the service's calls_json.
std::vector<nlohmann::json> calls(LuaServiceManager& manager,
                                  const std::string& service) {
    CallResult cr =
        call(manager, service, "calls_json", nlohmann::json::array());
    std::vector<nlohmann::json> out;
    if (cr.success && !cr.values.empty() && cr.values[0].is_array()) {
        for (const auto& entry : cr.values[0]) {
            out.push_back(entry);
        }
    }
    return out;
}

}  // namespace

#ifdef SHIELD_ENABLE_SERVER

#include "shield/server/server_manager.hpp"

namespace {

// Per-case ServerManager singleton: each case owns a fresh state machine so
// terminal states never leak between tests. The notify wiring mirrors
// bootstrap's (service_vm liveness + send_system over the system channel).
struct ServerWorld {
    caf::actor_system_config caf_cfg;
    caf::actor_system system;
    LuaRuntime runtime;
    LuaServiceManager manager;
    shield::server::ServerConfig config;
    shield::server::ServerManager sm;
    int stop_requests = 0;

    explicit ServerWorld(
        shield::server::ServerConfig cfg = shield::server::ServerConfig{})
        : system(caf_cfg),
          manager(runtime, system),
          config(std::move(cfg)),
          sm(config) {
        sm.set_notify_fn(
            [this](const std::string& service_id,
                   const std::string& state_name) -> shield::server::Delivery {
                if (!manager.service_vm(service_id)) {
                    return shield::server::Delivery::kGone;
                }
                std::string error;
                if (!manager.send_system(service_id, "on_server_state_change",
                                         nlohmann::json::array({state_name}),
                                         &error)) {
                    return error == "runtime is stopping"
                               ? shield::server::Delivery::kRetryable
                               : shield::server::Delivery::kGone;
                }
                return shield::server::Delivery::kOk;
            });
        sm.set_stop_request_fn([this] { ++stop_requests; });
        shield::server::ServerManager::set_global(&sm);
    }
    ~ServerWorld() { shield::server::ServerManager::set_global(nullptr); }

    SpawnResult spawn(const std::string& name) {
        return manager.spawn(SERVER_SCRIPT, service_opts(name).dump());
    }
};

}  // namespace

// The CAF global meta objects must exist before the first actor_system (the
// ServerWorld cases) is constructed.
struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

BOOST_AUTO_TEST_SUITE(LapiServerFacade)

BOOST_AUTO_TEST_CASE(LAPI_SV_01_InfoReadsGlobalManager) {
    shield::server::ServerConfig cfg;
    cfg.name = "facade-server";
    cfg.info_name = "Facade";
    cfg.info_version = "9.9.9-lua";
    cfg.info_region = "test-1";
    ServerWorld world(cfg);

    auto svc = world.spawn("sv_info");
    if (!svc.success) BOOST_TEST_MESSAGE("spawn error: " << svc.error_message);
    BOOST_REQUIRE(svc.success);

    CallResult info =
        call(world.manager, svc.service_id, "info", nlohmann::json::array());
    BOOST_REQUIRE(info.success);
    const nlohmann::json& v = info.values[0];
    BOOST_CHECK_EQUAL(v["state"].get<std::string>(), "starting");
    BOOST_CHECK_EQUAL(v["uptime"].get<double>(), 0.0);
    BOOST_CHECK_EQUAL(v["version"].get<std::string>(), "9.9.9-lua");
    BOOST_CHECK_EQUAL(v["node_id"].get<std::string>(), "");
    BOOST_CHECK_EQUAL(v["started_at"].get<uint64_t>(), 0u);

    CallResult cfg_result =
        call(world.manager, svc.service_id, "config", nlohmann::json::array());
    BOOST_REQUIRE(cfg_result.success);
    BOOST_CHECK_EQUAL(cfg_result.values[0]["name"].get<std::string>(),
                      "facade-server");
    BOOST_CHECK_EQUAL(cfg_result.values[0]["info"]["region"].get<std::string>(),
                      "test-1");

    // After mark_ready the same accessors report the ready origin.
    world.sm.mark_ready();
    BOOST_CHECK(wait_until(
        [&] {
            CallResult again = call(world.manager, svc.service_id, "info",
                                    nlohmann::json::array());
            return again.success &&
                   again.values[0]["state"].get<std::string>() == "running" &&
                   again.values[0]["started_at"].get<uint64_t>() != 0u;
        },
        std::chrono::seconds(5)));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(LapiServerSetState)

BOOST_AUTO_TEST_CASE(LAPI_SV_02_SetStateValidationMatrix) {
    ServerWorld world;
    auto svc = world.spawn("sv_state");
    if (!svc.success) BOOST_TEST_MESSAGE("spawn error: " << svc.error_message);
    BOOST_REQUIRE(svc.success);

    // Unknown names are invalid_state.
    CallResult bogus = call(world.manager, svc.service_id, "do_set_state",
                            nlohmann::json::array({"bogus"}));
    BOOST_REQUIRE(bogus.success);
    BOOST_CHECK_EQUAL(bogus.values[0]["ok"].get<bool>(), false);
    BOOST_CHECK_EQUAL(bogus.values[0]["code"].get<std::string>(),
                      "invalid_state");

    // starting -> maintenance is the rejected bootstrap edge.
    CallResult early = call(world.manager, svc.service_id, "do_set_state",
                            nlohmann::json::array({"maintenance"}));
    BOOST_REQUIRE(early.success);
    BOOST_CHECK_EQUAL(early.values[0]["code"].get<std::string>(),
                      "invalid_state_transition");
    BOOST_CHECK(world.sm.state() == shield::server::ServerState::kStarting);

    // The legal walk from Lua: running -> maintenance (idempotent repeat).
    world.sm.mark_ready();
    CallResult run = call(world.manager, svc.service_id, "do_set_state",
                          nlohmann::json::array({"running"}));
    BOOST_REQUIRE(run.success);
    BOOST_CHECK_EQUAL(run.values[0]["ok"].get<bool>(), true);

    CallResult maint = call(world.manager, svc.service_id, "do_set_state",
                            nlohmann::json::array({"maintenance"}));
    BOOST_REQUIRE(maint.success);
    BOOST_CHECK(world.sm.state() == shield::server::ServerState::kMaintenance);

    CallResult again = call(world.manager, svc.service_id, "do_set_state",
                            nlohmann::json::array({"maintenance"}));
    BOOST_REQUIRE(again.success);
    BOOST_CHECK_EQUAL(again.values[0]["ok"].get<bool>(), true);

    // Nothing migrates back into the initial state.
    CallResult back = call(world.manager, svc.service_id, "do_set_state",
                           nlohmann::json::array({"starting"}));
    BOOST_REQUIRE(back.success);
    BOOST_CHECK_EQUAL(back.values[0]["code"].get<std::string>(),
                      "invalid_state_transition");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(LapiServerWatch)

BOOST_AUTO_TEST_CASE(LAPI_SV_03_WatchDeliversTransitionsAsync) {
    ServerWorld world;
    auto svc = world.spawn("sv_watch");
    if (!svc.success) BOOST_TEST_MESSAGE("spawn error: " << svc.error_message);
    BOOST_REQUIRE(svc.success);
    world.sm.mark_ready();

    CallResult w = call(world.manager, svc.service_id, "do_watch",
                        nlohmann::json::array());
    BOOST_REQUIRE(w.success);
    BOOST_CHECK_EQUAL(w.values[0]["ok"].get<bool>(), true);
    BOOST_CHECK_EQUAL(w.values[0]["id"].get<uint64_t>(), 1u);
    BOOST_CHECK_EQUAL(world.sm.watcher_count(), 1u);

    // The transition notifies on the caller's thread; the delivery lands on
    // the service actor, so poll for it.
    BOOST_CHECK(
        world.sm.set_state(shield::server::ServerState::kMaintenance, nullptr));
    BOOST_CHECK(wait_until(
        [&] {
            for (const auto& entry : calls(world.manager, svc.service_id)) {
                if (entry["state"] == "maintenance") return true;
            }
            return false;
        },
        std::chrono::seconds(5)));

    // Re-watching returns the same id and keeps the original callback:
    // exactly one new entry arrives for the next transition.
    CallResult w2 = call(world.manager, svc.service_id, "do_watch",
                         nlohmann::json::array());
    BOOST_REQUIRE(w2.success);
    BOOST_CHECK_EQUAL(w2.values[0]["id"].get<uint64_t>(), 1u);

    BOOST_CHECK(
        world.sm.set_state(shield::server::ServerState::kRunning, nullptr));
    BOOST_CHECK(wait_until(
        [&] {
            auto log = calls(world.manager, svc.service_id);
            return log.size() == 2u && log[1]["state"] == "running";
        },
        std::chrono::seconds(5)));

    // unwatch stops the delivery.
    CallResult u = call(world.manager, svc.service_id, "do_unwatch",
                        nlohmann::json::array());
    BOOST_REQUIRE(u.success);
    BOOST_CHECK_EQUAL(world.sm.watcher_count(), 0u);

    BOOST_CHECK(
        world.sm.set_state(shield::server::ServerState::kMaintenance, nullptr));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    BOOST_CHECK_EQUAL(calls(world.manager, svc.service_id).size(), 2u);

    world.manager.exit(svc.service_id);
    BOOST_CHECK(!world.manager.service_vm(svc.service_id));
}

BOOST_AUTO_TEST_CASE(LAPI_SV_04_ExitedWatcherAutoDeregisters) {
    ServerWorld world;
    auto svc = world.spawn("sv_watch_gone");
    if (!svc.success) BOOST_TEST_MESSAGE("spawn error: " << svc.error_message);
    BOOST_REQUIRE(svc.success);

    CallResult w = call(world.manager, svc.service_id, "do_watch",
                        nlohmann::json::array());
    BOOST_REQUIRE(w.success);
    BOOST_CHECK_EQUAL(world.sm.watcher_count(), 1u);

    // Once the observing service is gone the next transition reports kGone
    // and the registration is dropped instead of leaking.
    world.manager.exit(svc.service_id);
    BOOST_CHECK(!world.manager.service_vm(svc.service_id));

    world.sm.set_state(shield::server::ServerState::kRunning, nullptr);
    BOOST_CHECK_EQUAL(world.sm.watcher_count(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(LapiServerWatchEdges)

// A float-typed watch id takes the facade's double unwrapping branch; the
// contract keeps unknown ids silently successful.
BOOST_AUTO_TEST_CASE(LAPI_SV_08_UnwatchAcceptsFloatId) {
    ServerWorld world;
    auto svc = world.spawn("sv_unwatch_float");
    if (!svc.success) BOOST_TEST_MESSAGE("spawn error: " << svc.error_message);
    BOOST_REQUIRE(svc.success);

    CallResult u = call(world.manager, svc.service_id, "do_unwatch_float",
                        nlohmann::json::array({7.0}));
    BOOST_REQUIRE(u.success);
    BOOST_CHECK(!u.values.empty());
    BOOST_CHECK(u.values[0] == true);
}

// Watch called at module top level: no dispatch frame and no registry entry
// yet, so the facade answers with invalid_argument instead of registering.
// The script asserts the error itself; a failed assert fails the spawn.
BOOST_AUTO_TEST_CASE(LAPI_SV_09_TopLevelWatchIsRejected) {
    ServerWorld world;
    SpawnResult svc = world.manager.spawn(
        "../tests/lua_api/scripts/server_service_topwatch.lua",
        service_opts("sv_top_watch").dump());
    if (!svc.success) BOOST_TEST_MESSAGE("spawn error: " << svc.error_message);
    BOOST_REQUIRE(svc.success);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(LapiServerShutdown)

BOOST_AUTO_TEST_CASE(LAPI_SV_05_ImmediateShutdownClosedLoop) {
    ServerWorld world;
    auto svc = world.spawn("sv_shutdown");
    if (!svc.success) BOOST_TEST_MESSAGE("spawn error: " << svc.error_message);
    BOOST_REQUIRE(svc.success);

    CallResult s = call(world.manager, svc.service_id, "do_shutdown",
                        nlohmann::json::array({0}));
    BOOST_REQUIRE(s.success);
    BOOST_CHECK_EQUAL(s.values[0]["ok"].get<bool>(), true);
    BOOST_CHECK(world.sm.state() == shield::server::ServerState::kShutdown);
    BOOST_CHECK(world.sm.shutdown_scheduled());
    BOOST_CHECK_EQUAL(world.stop_requests, 1);

    // A second handover is refused with the stable code.
    CallResult dup = call(world.manager, svc.service_id, "do_shutdown",
                          nlohmann::json::array({0}));
    BOOST_REQUIRE(dup.success);
    BOOST_CHECK_EQUAL(dup.values[0]["ok"].get<bool>(), false);
    BOOST_CHECK_EQUAL(dup.values[0]["code"].get<std::string>(),
                      "shutdown_already_scheduled");
    BOOST_CHECK_EQUAL(world.stop_requests, 1);
}

BOOST_AUTO_TEST_CASE(LAPI_SV_06_ShutdownArgumentMatrix) {
    ServerWorld world;
    auto svc = world.spawn("sv_shutdown_arg");
    if (!svc.success) BOOST_TEST_MESSAGE("spawn error: " << svc.error_message);
    BOOST_REQUIRE(svc.success);

    for (const auto& bad : nlohmann::json::array({-1, 1.5, "x"})) {
        CallResult r = call(world.manager, svc.service_id, "do_shutdown",
                            nlohmann::json::array({bad}));
        BOOST_REQUIRE(r.success);
        BOOST_CHECK_EQUAL(r.values[0]["ok"].get<bool>(), false);
        BOOST_CHECK_EQUAL(r.values[0]["code"].get<std::string>(),
                          "invalid_argument");
    }
    // Nothing was scheduled and no stop was requested.
    BOOST_CHECK(!world.sm.shutdown_scheduled());
    BOOST_CHECK(world.sm.state() == shield::server::ServerState::kStarting);
    BOOST_CHECK_EQUAL(world.stop_requests, 0);

    // A fractional-integral number parses as ms and fires the injected
    // stop request once the delay elapses.
    CallResult delayed = call(world.manager, svc.service_id, "do_shutdown",
                              nlohmann::json::array({80.0}));
    BOOST_REQUIRE(delayed.success);
    BOOST_CHECK_EQUAL(delayed.values[0]["ok"].get<bool>(), true);
    BOOST_CHECK_EQUAL(world.stop_requests, 0);
    BOOST_CHECK(wait_until([&] { return world.stop_requests == 1; },
                           std::chrono::seconds(5)));
}

BOOST_AUTO_TEST_SUITE_END()

#else  // SHIELD_ENABLE_SERVER

// The server-enabled build has no global fixture of its own; the stub build
// needs the CAF global meta objects before the actor_system below can exist.
struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

BOOST_AUTO_TEST_SUITE(LapiServerStub)

BOOST_AUTO_TEST_CASE(LAPI_SV_07_StubReportsModuleUnavailable) {
    caf::actor_system_config caf_cfg;
    caf::actor_system system(caf_cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto svc = manager.spawn(SERVER_SCRIPT, service_opts("sv_stub").dump());
    if (!svc.success) BOOST_TEST_MESSAGE("spawn error: " << svc.error_message);
    BOOST_REQUIRE(svc.success);

    // Every facade entry degrades to the stable module_unavailable code.
    CallResult probe =
        call(manager, svc.service_id, "stub_probe", nlohmann::json::array());
    BOOST_REQUIRE(probe.success);
    const nlohmann::json& codes = probe.values[0];
    for (const std::string& name :
         {"state", "uptime", "version", "node_id", "started_at", "config",
          "set_state", "shutdown", "watch", "unwatch"}) {
        BOOST_CHECK_EQUAL(codes[name].get<std::string>(), "module_unavailable");
    }
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // SHIELD_ENABLE_SERVER
