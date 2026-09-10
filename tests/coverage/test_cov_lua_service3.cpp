// Coverage tests for the LuaServiceManager proxied-call primitives (the
// callee side of an M4 remote call) and call_with_session (the synchronous
// caller-side primitive). These seams are unconditional parts of the lua
// module — no cluster libraries — so this suite is registered in every build
// shape; the network-level envelope cases live in test_cov_cluster.cpp
// (cluster builds only).
#define BOOST_TEST_MODULE CovLuaService3
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_cov_lua_service3";

std::string write_script(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(kTmpDir);
    const std::string path = kTmpDir + "/" + name;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

std::string opts_for(const std::string& name) {
    nlohmann::json opts = {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
    return opts.dump();
}

// Echo callee for dispatch / proxied scenarios.
const char* kCalleeScript = R"lua(
local M = {}
function M.echo(ctx, v) return v end
return M
)lua";

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

}  // namespace

// ---------------------------------------------------------------------------
// Proxied-call primitives: begin/finish/abandon, the completion hook, and
// the self-contained expiry driver.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ProxiedCallPrimitives) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    std::atomic<bool> hook_fired{false};
    std::atomic<uint64_t> hook_session{0};
    std::atomic<bool> hook_ok{false};
    std::string hook_code;
    manager.set_proxied_call_hook(
        [&](uint64_t session, bool ok, const nlohmann::json& values) {
            hook_fired = true;
            hook_session = session;
            hook_ok = ok;
            if (values.is_array() && !values.empty() && values[0].is_object() &&
                values[0].contains("code")) {
                hook_code = values[0]["code"].get<std::string>();
            }
        });

    // Unknown sessions: finishing and abandoning are honest no-ops.
    BOOST_CHECK(!manager.finish_proxied_call(4242, true, {}));
    manager.abandon_proxied_call(4242);
    // The timeout driver ignores non-positive timeouts.
    manager.schedule_proxied_call_timeout(4242, 0);

    // A live proxied session completes through the hook.
    const uint64_t session = manager.begin_proxied_call(1000);
    BOOST_CHECK_NE(session, 0u);
    BOOST_CHECK(manager.finish_proxied_call(session, true,
                                            nlohmann::json::array({"pong"})));
    BOOST_CHECK(wait_until([&] { return hook_fired.load(); },
                           std::chrono::milliseconds(1000)));
    BOOST_CHECK_EQUAL(hook_session.load(), session);
    BOOST_CHECK(hook_ok.load());
    // The entry is gone: a second finish reports the unknown session.
    BOOST_CHECK(!manager.finish_proxied_call(session, true, {}));

    // Abandon removes the entry: a later finish finds nothing.
    const uint64_t abandoned = manager.begin_proxied_call(1000);
    manager.abandon_proxied_call(abandoned);
    BOOST_CHECK(!manager.finish_proxied_call(abandoned, false, {}));

    // The self-contained expiry driver finishes the session with the
    // timeout error after its (here: short) delay.
    const uint64_t expiring = manager.begin_proxied_call(1000);
    manager.schedule_proxied_call_timeout(expiring, 80);
    BOOST_CHECK(wait_until([&] { return hook_fired.load() && !hook_ok.load(); },
                           std::chrono::milliseconds(3000)));
    BOOST_CHECK_EQUAL(hook_session.load(), expiring);
    BOOST_CHECK_EQUAL(hook_code, "timeout");
}

// ---------------------------------------------------------------------------
// dispatch_remote_call: the transport-side entry point for inbound call
// envelopes (fail-fast, success completion, and dispatch failure).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(DispatchRemoteCallPaths) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto callee =
        manager.spawn(write_script("cov_lsvc3_echo.lua", kCalleeScript),
                      opts_for("cov_lsvc3_echo_impl"));
    BOOST_REQUIRE(callee.success);

    std::atomic<bool> hook_fired{false};
    std::atomic<bool> hook_ok{false};
    manager.set_proxied_call_hook(
        [&](uint64_t, bool ok, const nlohmann::json&) {
            hook_fired = true;
            hook_ok = ok;
        });

    // Unknown local service: the dispatch fails synchronously, the session
    // is abandoned, and the caller of the API gets 0.
    std::string error;
    BOOST_CHECK_EQUAL(
        manager.dispatch_remote_call("ghost_svc", "echo",
                                     nlohmann::json::array({"x"}), 500, &error),
        0u);
    BOOST_CHECK(!error.empty());

    // Live service: the proxied session is dispatched and completed through
    // the hook once the callee handler returns.
    const uint64_t session = manager.dispatch_remote_call(
        callee.service_id, "echo", nlohmann::json::array({"direct"}), 2000,
        nullptr);
    BOOST_CHECK_NE(session, 0u);
    BOOST_CHECK(wait_until([&] { return hook_fired.load(); },
                           std::chrono::milliseconds(3000)));
    BOOST_CHECK(hook_ok.load());
}

// ---------------------------------------------------------------------------
// call_with_session: the synchronous cross-node primitive used by
// main-thread shield.call on remote targets.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CallWithSessionPaths) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    // No initiate at all.
    auto failed = manager.call_with_session({}, 100);
    BOOST_CHECK(!failed.success);
    BOOST_CHECK_EQUAL(failed.error_message, "call dispatch failed");

    // Synchronous initiate failure surfaces the transport's error string.
    failed = manager.call_with_session(
        [](uint64_t, std::string& err) {
            err = "node_offline";
            return false;
        },
        100);
    BOOST_CHECK(!failed.success);
    BOOST_CHECK_EQUAL(failed.error_message, "node_offline");

    // Completion from "the transport": values come back as success.
    auto done = manager.call_with_session(
        [&](uint64_t session, std::string&) {
            std::thread([&, session]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                manager.complete_call(session, true,
                                      nlohmann::json::array({"pong"}));
            }).detach();
            return true;
        },
        3000);
    BOOST_CHECK(done.success);
    BOOST_REQUIRE_EQUAL(done.values.size(), 1u);
    BOOST_CHECK_EQUAL(done.values[0].get<std::string>(), "pong");

    // Failure completion: the error object passes through.
    auto failed_call = manager.call_with_session(
        [&](uint64_t session, std::string&) {
            std::thread([&, session]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                manager.complete_call(
                    session, false,
                    nlohmann::json::array({nlohmann::json::object(
                        {{"code", "handler_error"}, {"message", "boom"}})}));
            }).detach();
            return true;
        },
        3000);
    BOOST_CHECK(!failed_call.success);
    BOOST_CHECK_EQUAL(failed_call.error_message, "boom");

    // Nobody completes: the bounded wait returns the timeout error.
    auto timeout = manager.call_with_session(
        [](uint64_t, std::string&) { return true; }, 80);
    BOOST_CHECK(!timeout.success);
    BOOST_CHECK(timeout.error_message.find("call timeout") !=
                std::string::npos);
}

// ---------------------------------------------------------------------------
// Runtime stopping: pending initiates are rejected during shutdown.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CallWithSessionRejectsWhenStopping) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    manager.shutdown_all("cov_done");
    auto result = manager.call_with_session(
        [](uint64_t, std::string&) { return true; }, 100);
    BOOST_CHECK(!result.success);
    BOOST_CHECK_EQUAL(result.error_message, "runtime is stopping");
}

// ---------------------------------------------------------------------------
// Shutdown also wakes calls that are already blocked on the completion CV:
// they fail with "runtime is stopping" instead of riding out their timeout.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ShutdownWakesPendingSyncCalls) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    std::optional<CallResult> result;
    std::atomic<bool> initiated{false};
    std::thread blocked([&] {
        result = manager.call_with_session(
            [&](uint64_t, std::string&) {
                initiated = true;
                return true;
            },
            30000);
    });
    // Wait until the initiate ran (the caller is about to block on the CV),
    // then tear the runtime down under it.
    BOOST_CHECK(wait_until([&] { return initiated.load(); },
                           std::chrono::milliseconds(2000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    manager.shutdown_all("cov_wake");
    blocked.join();
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK(!result->success);
    BOOST_CHECK_EQUAL(result->error_message, "runtime is stopping");
}
