// ServerManager unit tests (shield_server P0).
// Constructed directly from ServerConfig: no global config, no Lua, no
// bootstrap. Config parsing tests opt in via global_config().set().
#define BOOST_TEST_MODULE ServerManagerTests
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/server/server_manager.hpp"

using shield::server::Delivery;
using shield::server::parse_server_state;
using shield::server::server_state_name;
using shield::server::ServerConfig;
using shield::server::ServerManager;
using shield::server::ServerState;
using shield::server::validate_server_config;

namespace {

ServerConfig default_config() {
    ServerConfig config;
    config.info_version = "1.0.0-test";
    return config;
}

/// Manager + notify recorder: every delivery is appended to `calls`
/// as "service_id:state_name", with a scriptable Delivery result.
struct RecordedManager {
    ServerManager mgr;
    std::vector<std::string> calls;
    Delivery next_result = Delivery::kOk;

    explicit RecordedManager(ServerConfig config = default_config())
        : mgr(std::move(config)) {
        mgr.set_notify_fn([this](const std::string& service_id,
                                 const std::string& state_name) {
            calls.push_back(service_id + ":" + state_name);
            return next_result;
        });
    }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(ServerStateNames)

BOOST_AUTO_TEST_CASE(NamesCoverAllStates) {
    BOOST_CHECK_EQUAL(server_state_name(ServerState::kStarting), "starting");
    BOOST_CHECK_EQUAL(server_state_name(ServerState::kRunning), "running");
    BOOST_CHECK_EQUAL(server_state_name(ServerState::kMaintenance),
                      "maintenance");
    BOOST_CHECK_EQUAL(server_state_name(ServerState::kShutdown), "shutdown");
}

BOOST_AUTO_TEST_CASE(ParseRoundTripsEveryName) {
    for (auto state : {ServerState::kStarting, ServerState::kRunning,
                       ServerState::kMaintenance, ServerState::kShutdown}) {
        ServerState parsed = ServerState::kStarting;
        BOOST_REQUIRE(parse_server_state(server_state_name(state), &parsed));
        BOOST_CHECK(parsed == state);
    }
}

BOOST_AUTO_TEST_CASE(ParseRejectsUnknownNames) {
    ServerState parsed = ServerState::kRunning;
    BOOST_CHECK(!parse_server_state("Starting", &parsed));
    BOOST_CHECK(!parse_server_state("", &parsed));
    BOOST_CHECK(!parse_server_state("stopped", &parsed));
    // Unknown names leave the out param untouched.
    BOOST_CHECK(parsed == ServerState::kRunning);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ServerTransitions)

BOOST_AUTO_TEST_CASE(FullMatrixTableDriven) {
    const auto states = {ServerState::kStarting, ServerState::kRunning,
                         ServerState::kMaintenance, ServerState::kShutdown};
    for (auto from : states) {
        for (auto to : states) {
            // Same-value sets are always idempotent successes.
            const bool expected =
                from == to || (from != ServerState::kShutdown &&
                               to != ServerState::kStarting &&
                               !(from == ServerState::kStarting &&
                                 to == ServerState::kMaintenance));
            BOOST_CHECK_MESSAGE(
                ServerManager::transition_allowed(from, to) == expected,
                std::string(server_state_name(from)) + " -> " +
                    server_state_name(to));
        }
    }
}

BOOST_AUTO_TEST_CASE(IllegalStartingEdgeReturnsErrorText) {
    ServerManager mgr(default_config());
    std::string error;
    // starting -> maintenance is the one edge the initial state rejects.
    BOOST_CHECK(!mgr.set_state(ServerState::kMaintenance, &error));
    BOOST_CHECK_EQUAL(error,
                      "invalid state transition: starting -> maintenance");
    BOOST_CHECK(mgr.state() == ServerState::kStarting);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ServerLifecycle)

BOOST_AUTO_TEST_CASE(InitialAccessorsBeforeReady) {
    ServerManager mgr(default_config());
    BOOST_CHECK(mgr.state() == ServerState::kStarting);
    BOOST_CHECK_EQUAL(mgr.uptime_seconds(), 0.0);
    BOOST_CHECK_EQUAL(mgr.started_at_ms(), 0u);
    BOOST_CHECK_EQUAL(mgr.node_id(), "");
    BOOST_CHECK(!mgr.shutdown_scheduled());
    BOOST_CHECK_EQUAL(mgr.watcher_count(), 0u);
}

BOOST_AUTO_TEST_CASE(MarkReadyStampsOriginOnce) {
    ServerManager mgr(default_config());
    mgr.mark_ready();
    BOOST_CHECK(mgr.state() == ServerState::kRunning);

    const auto started = mgr.started_at_ms();
    BOOST_CHECK_NE(started, 0u);
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    // The stamp is a wall-clock sample from inside mark_ready().
    BOOST_CHECK_LE(started, static_cast<std::uint64_t>(now_ms));

    const double uptime = mgr.uptime_seconds();
    BOOST_CHECK_GE(uptime, 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    BOOST_CHECK_GE(mgr.uptime_seconds(), uptime);

    // Idempotent: the second ready keeps the first origin stamp.
    mgr.mark_ready();
    BOOST_CHECK_EQUAL(mgr.started_at_ms(), started);
}

BOOST_AUTO_TEST_CASE(VersionThreeSources) {
    ServerConfig with_info;
    with_info.info_version = "9.9.9";
    ServerManager mgr_info(with_info);
    mgr_info.set_version_fallback("0.1.0-fallback");
    BOOST_CHECK_EQUAL(mgr_info.version(), "9.9.9");

    ServerManager mgr_fallback(ServerConfig{});
    mgr_fallback.set_version_fallback("0.1.0-fallback");
    BOOST_CHECK_EQUAL(mgr_fallback.version(), "0.1.0-fallback");

    ServerManager mgr_none(ServerConfig{});
    BOOST_CHECK_EQUAL(mgr_none.version(), "");
}

BOOST_AUTO_TEST_CASE(LocalityRoundTrip) {
    ServerManager mgr(default_config());
    mgr.set_locality("node-a");
    BOOST_CHECK_EQUAL(mgr.node_id(), "node-a");
    mgr.set_locality("node-b");
    BOOST_CHECK_EQUAL(mgr.node_id(), "node-b");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ServerSetState)

BOOST_AUTO_TEST_CASE(LegalMigrationsNotifyWatchers) {
    RecordedManager rec;
    rec.mgr.set_locality("");
    const auto id = rec.mgr.watch("svc_a");
    BOOST_CHECK_EQUAL(id, 1u);

    BOOST_CHECK(rec.mgr.set_state(ServerState::kRunning, nullptr));
    BOOST_REQUIRE_EQUAL(rec.calls.size(), 1u);
    BOOST_CHECK_EQUAL(rec.calls[0], "svc_a:running");

    BOOST_CHECK(rec.mgr.set_state(ServerState::kMaintenance, nullptr));
    BOOST_REQUIRE_EQUAL(rec.calls.size(), 2u);
    BOOST_CHECK_EQUAL(rec.calls[1], "svc_a:maintenance");

    BOOST_CHECK(rec.mgr.set_state(ServerState::kRunning, nullptr));
    BOOST_REQUIRE_EQUAL(rec.calls.size(), 3u);
    BOOST_CHECK_EQUAL(rec.calls[2], "svc_a:running");
}

BOOST_AUTO_TEST_CASE(SameValueSetIsIdempotentAndSilent) {
    RecordedManager rec;
    rec.mgr.watch("svc_a");
    rec.mgr.watch("svc_b");

    BOOST_CHECK(rec.mgr.set_state(ServerState::kStarting, nullptr));
    BOOST_CHECK(rec.calls.empty());

    // Two watchers -> one real transition delivers twice; the immediate
    // same-value repeat delivers nothing more.
    BOOST_CHECK(rec.mgr.set_state(ServerState::kRunning, nullptr));
    BOOST_CHECK(rec.mgr.set_state(ServerState::kRunning, nullptr));
    BOOST_CHECK_EQUAL(rec.calls.size(), 2u);

    // Terminal same-value is also an idempotent success.
    BOOST_CHECK(rec.mgr.set_state(ServerState::kShutdown, nullptr));
    BOOST_CHECK(rec.mgr.set_state(ServerState::kShutdown, nullptr));
    BOOST_CHECK_EQUAL(rec.calls.size(), 4u);
}

BOOST_AUTO_TEST_CASE(IllegalMigrationsRejectedWithErrors) {
    ServerManager mgr(default_config());
    std::string error;

    // starting -> maintenance is the rejected bootstrap edge.
    BOOST_CHECK(!mgr.set_state(ServerState::kMaintenance, &error));
    BOOST_CHECK_EQUAL(error,
                      "invalid state transition: starting -> maintenance");
    BOOST_CHECK(mgr.state() == ServerState::kStarting);

    // Nothing may migrate back into the initial state.
    mgr.set_state(ServerState::kRunning, nullptr);
    BOOST_CHECK(!mgr.set_state(ServerState::kStarting, &error));
    BOOST_CHECK_EQUAL(error, "invalid state transition: running -> starting");

    // shutdown is terminal, from every other state.
    mgr.set_state(ServerState::kMaintenance, nullptr);
    mgr.set_state(ServerState::kShutdown, nullptr);
    for (auto to : {ServerState::kStarting, ServerState::kRunning,
                    ServerState::kMaintenance}) {
        BOOST_CHECK(!mgr.set_state(to, &error));
        BOOST_CHECK_EQUAL(
            error, std::string("invalid state transition: shutdown -> ") +
                       server_state_name(to));
    }
    BOOST_CHECK(mgr.state() == ServerState::kShutdown);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ServerWatch)

BOOST_AUTO_TEST_CASE(WatchIdsAreSequentialAndServiceIdempotent) {
    ServerManager mgr(default_config());
    const auto first = mgr.watch("svc_a");
    const auto second = mgr.watch("svc_b");
    BOOST_CHECK_EQUAL(first, 1u);
    BOOST_CHECK_EQUAL(second, 2u);
    BOOST_CHECK_EQUAL(mgr.watcher_count(), 2u);

    // Re-watching the same service returns the existing id and does not
    // duplicate the registration.
    BOOST_CHECK_EQUAL(mgr.watch("svc_a"), first);
    BOOST_CHECK_EQUAL(mgr.watcher_count(), 2u);
}

BOOST_AUTO_TEST_CASE(UnwatchIsIdempotentAndStopsDelivery) {
    RecordedManager rec;
    const auto id = rec.mgr.watch("svc_a");
    rec.mgr.watch("svc_b");

    rec.mgr.unwatch(id);
    BOOST_CHECK_EQUAL(rec.mgr.watcher_count(), 1u);
    // Unknown/removed ids are a silent success.
    rec.mgr.unwatch(id);
    rec.mgr.unwatch(9999);
    BOOST_CHECK_EQUAL(rec.mgr.watcher_count(), 1u);

    rec.mgr.set_state(ServerState::kRunning, nullptr);
    BOOST_REQUIRE_EQUAL(rec.calls.size(), 1u);
    BOOST_CHECK_EQUAL(rec.calls[0], "svc_b:running");
}

BOOST_AUTO_TEST_CASE(NoTransitionMeansNoNotification) {
    RecordedManager rec;
    rec.mgr.watch("svc_a");
    // Same-value sets notify nobody even with watchers present.
    BOOST_CHECK(rec.mgr.set_state(ServerState::kStarting, nullptr));
    // Failed transitions notify nobody either.
    std::string error;
    BOOST_CHECK(!rec.mgr.set_state(ServerState::kMaintenance, &error));
    BOOST_CHECK(rec.calls.empty());
}

BOOST_AUTO_TEST_CASE(GoneWatchersAreUnregistered) {
    RecordedManager rec;
    rec.mgr.watch("svc_a");
    rec.mgr.watch("svc_b");
    rec.next_result = Delivery::kGone;

    rec.mgr.set_state(ServerState::kRunning, nullptr);
    // Both deliveries happened, then both registrations were dropped.
    BOOST_CHECK_EQUAL(rec.calls.size(), 2u);
    BOOST_CHECK_EQUAL(rec.mgr.watcher_count(), 0u);

    // No further delivery after the automatic deregistration.
    rec.mgr.set_state(ServerState::kMaintenance, nullptr);
    BOOST_CHECK_EQUAL(rec.calls.size(), 2u);
}

BOOST_AUTO_TEST_CASE(RetryableWatchersAreKept) {
    RecordedManager rec;
    rec.mgr.watch("svc_a");
    rec.next_result = Delivery::kRetryable;

    rec.mgr.set_state(ServerState::kRunning, nullptr);
    BOOST_CHECK_EQUAL(rec.calls.size(), 1u);
    BOOST_CHECK_EQUAL(rec.mgr.watcher_count(), 1u);

    // A kept watcher still receives the next transition.
    rec.next_result = Delivery::kOk;
    rec.mgr.set_state(ServerState::kMaintenance, nullptr);
    BOOST_REQUIRE_EQUAL(rec.calls.size(), 2u);
    BOOST_CHECK_EQUAL(rec.calls[1], "svc_a:maintenance");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ServerShutdownScheduling)

BOOST_AUTO_TEST_CASE(ImmediateShutdownMigratesNotifiesAndStopsSync) {
    RecordedManager rec;
    int stop_requests = 0;
    rec.mgr.set_stop_request_fn([&stop_requests] { ++stop_requests; });
    rec.mgr.watch("svc_a");

    BOOST_CHECK(rec.mgr.schedule_shutdown(0, nullptr));
    BOOST_CHECK(rec.mgr.state() == ServerState::kShutdown);
    BOOST_CHECK(rec.mgr.shutdown_scheduled());
    BOOST_CHECK_EQUAL(stop_requests, 1);
    BOOST_REQUIRE_EQUAL(rec.calls.size(), 1u);
    BOOST_CHECK_EQUAL(rec.calls[0], "svc_a:shutdown");

    // A second schedule is refused even though the state is terminal.
    std::string error;
    BOOST_CHECK(!rec.mgr.schedule_shutdown(0, &error));
    BOOST_CHECK_EQUAL(error, "shutdown already scheduled");
    BOOST_CHECK_EQUAL(stop_requests, 1);
}

BOOST_AUTO_TEST_CASE(ScheduleRefusedAfterExternalShutdownState) {
    RecordedManager rec;
    int stop_requests = 0;
    rec.mgr.set_stop_request_fn([&stop_requests] { ++stop_requests; });
    rec.mgr.watch("svc_a");

    // Reaching `shutdown` through set_state also locks out the handover,
    // but schedules nothing and runs no timer.
    BOOST_CHECK(rec.mgr.set_state(ServerState::kShutdown, nullptr));
    BOOST_CHECK(!rec.mgr.shutdown_scheduled());

    std::string error;
    BOOST_CHECK(!rec.mgr.schedule_shutdown(0, &error));
    BOOST_CHECK_EQUAL(error, "shutdown already scheduled");
    BOOST_CHECK_EQUAL(stop_requests, 0);
    BOOST_CHECK(!rec.mgr.shutdown_scheduled());
}

BOOST_AUTO_TEST_CASE(DelayedShutdownFiresStopRequest) {
    RecordedManager rec;
    int stop_requests = 0;
    rec.mgr.set_stop_request_fn([&stop_requests] { ++stop_requests; });

    BOOST_CHECK(rec.mgr.schedule_shutdown(60, nullptr));
    BOOST_CHECK(rec.mgr.state() == ServerState::kShutdown);
    BOOST_CHECK_EQUAL(stop_requests, 0);

    // The timer thread fires after the delay.
    for (int i = 0; i < 100 && stop_requests == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_CHECK_EQUAL(stop_requests, 1);
}

BOOST_AUTO_TEST_CASE(StopAbortsPendingTimerWithoutCallback) {
    RecordedManager rec;
    int stop_requests = 0;
    rec.mgr.set_stop_request_fn([&stop_requests] { ++stop_requests; });

    BOOST_CHECK(rec.mgr.schedule_shutdown(10000, nullptr));
    rec.mgr.stop();

    // The aborted timer must not fire the stop request.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    BOOST_CHECK_EQUAL(stop_requests, 0);

    // stop() dropped the callbacks but the handover stays consumed: a
    // second schedule is still refused and fires nothing.
    std::string error;
    BOOST_CHECK(!rec.mgr.schedule_shutdown(0, &error));
    BOOST_CHECK_EQUAL(error, "shutdown already scheduled");
    BOOST_CHECK_EQUAL(stop_requests, 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ServerConfigParsing)

BOOST_AUTO_TEST_CASE(DefaultsAreOptional) {
    ServerConfig config;
    std::string error;
    BOOST_CHECK(ServerConfig::from_global_config(&config, &error));
    BOOST_CHECK_EQUAL(config.name, "server_manager");
    BOOST_CHECK_EQUAL(config.info_name, "");
    BOOST_CHECK_EQUAL(config.info_version, "");
    BOOST_CHECK_EQUAL(config.info_region, "");
    BOOST_CHECK(validate_server_config(config, &error));
}

BOOST_AUTO_TEST_CASE(FromGlobalConfigReadsFlatKeys) {
    auto& cfg = shield::config::global_config();
    cfg.set("server_manager.name", std::string("gateway-one"));
    cfg.set("server_manager.info.name", std::string("My Game"));
    cfg.set("server_manager.info.version", std::string("2.3.4"));
    cfg.set("server_manager.info.region", std::string("cn-east"));

    ServerConfig config;
    std::string error;
    BOOST_CHECK(ServerConfig::from_global_config(&config, &error));
    BOOST_CHECK_EQUAL(config.name, "gateway-one");
    BOOST_CHECK_EQUAL(config.info_name, "My Game");
    BOOST_CHECK_EQUAL(config.info_version, "2.3.4");
    BOOST_CHECK_EQUAL(config.info_region, "cn-east");
    BOOST_CHECK(validate_server_config(config, &error));
}

BOOST_AUTO_TEST_CASE(ValidateRejectsEmptyName) {
    ServerConfig config;
    config.name = "";
    std::string error;
    BOOST_CHECK(!validate_server_config(config, &error));
    BOOST_CHECK(error.find("server_manager.name") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ValidateRejectsOverlongFields) {
    const std::string long_value(65, 'x');
    const std::string ok_value(64, 'x');

    ServerConfig config;
    config.name = long_value;
    std::string error;
    BOOST_CHECK(!validate_server_config(config, &error));
    BOOST_CHECK(error.find("server_manager.name") != std::string::npos);

    config.name = ok_value;
    config.info_name = long_value;
    BOOST_CHECK(!validate_server_config(config, &error));
    BOOST_CHECK(error.find("server_manager.info.name") != std::string::npos);

    config.info_name = ok_value;
    config.info_version = long_value;
    BOOST_CHECK(!validate_server_config(config, &error));
    BOOST_CHECK(error.find("server_manager.info.version") != std::string::npos);

    config.info_version = ok_value;
    config.info_region = long_value;
    BOOST_CHECK(!validate_server_config(config, &error));
    BOOST_CHECK(error.find("server_manager.info.region") != std::string::npos);

    // All at the 64-char boundary: valid.
    config.info_region = ok_value;
    BOOST_CHECK(validate_server_config(config, &error));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ServerGlobalAccessors)

BOOST_AUTO_TEST_CASE(GlobalRoundTrip) {
    BOOST_CHECK(ServerManager::global() == nullptr);
    ServerManager mgr(default_config());
    ServerManager::set_global(&mgr);
    BOOST_CHECK(ServerManager::global() == &mgr);
    ServerManager::set_global(nullptr);
    BOOST_CHECK(ServerManager::global() == nullptr);
}

BOOST_AUTO_TEST_CASE(ConfigAccessorReturnsOwnConfig) {
    ServerConfig config;
    config.name = "custom-name";
    config.info_region = "ap-south";
    ServerManager mgr(config);
    BOOST_CHECK_EQUAL(mgr.config().name, "custom-name");
    BOOST_CHECK_EQUAL(mgr.config().info_region, "ap-south");
}

BOOST_AUTO_TEST_SUITE_END()
