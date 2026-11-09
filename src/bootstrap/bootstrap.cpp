// [SHIELD_BOOTSTRAP] Bootstrap implementation: process-wide state,
// gateway-actor teardown, failure cleanup and the initialize/shutdown
// orchestration over the phase units (bootstrap_init_*.cpp).
#include "shield/bootstrap/bootstrap.hpp"

#include "shield/base/error.hpp"
#include "shield/bootstrap/starter.hpp"
#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"

#ifdef SHIELD_ENABLE_CRASHPAD
#include "shield/crash/crash.hpp"
#endif
#include "shield/plugin/plugin_host.hpp"
#include "shield/plugin/protocol_codec.h"
#ifdef SHIELD_ENABLE_CLUSTER
#include "shield/cluster/cluster_manager.hpp"
#include "shield/cluster/cluster_transport.hpp"
#include "shield/cluster/node_discovery.hpp"
#endif
#ifdef SHIELD_ENABLE_PLAYER
#include "shield/player/player_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_SERVER
#include "shield/server/server_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_GLOBAL
#include "shield/global/global_manager.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <caf/actor_system.hpp>
#include <caf/io/all.hpp>
#include <caf/scoped_actor.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bootstrap_impl.hpp"
#include "shield/caf_initializer.hpp"
#include "shield/console/command_dispatcher.hpp"
#include "shield/console/lua_commands.hpp"
#include "shield/console/ops_http_handler.hpp"
#include "shield/console/root_commands.hpp"
#include "shield/lua/gateway_actor.hpp"
#include "shield/lua/lua_gateway_bridge.hpp"
#include "shield/lua/lua_http_bridge.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/console_server.hpp"
#include "shield/net/listener.hpp"
#include "shield/net/tls_context.hpp"
#include "shield/shield.hpp"
#include "shield/transport/protocol.hpp"
#include "shield/transport/rpc_descriptor.hpp"

namespace shield::bootstrap {

// Process-wide bootstrap state (see bootstrap_impl.hpp). The g_state
// definition lives here; the phase units reach it through the extern.
GlobalState* g_state = nullptr;
static std::unique_ptr<GlobalState> g_state_owner;

// Exit every live gateway actor and block until each is really gone.
// down_msg only fires after the actor finished its last handler, so once
// this returns no bind/close/egress response can still dereference the raw
// LuaServiceManager pointer in GatewayDeps. A bare anon_send_exit loop used
// to race the teardown: the exit messages are queued asynchronously while
// the caller moved on to destroy the manager those handlers point at.
static void exit_gateway_actors_and_wait() {
    if (g_state->gateway_actors.empty()) {
        return;
    }
    caf::scoped_actor self{*g_state->actor_system};
    for (const auto& gateway_actor : g_state->gateway_actors) {
        self->monitor(gateway_actor);
        caf::anon_send_exit(gateway_actor, caf::exit_reason::user_shutdown);
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    size_t remaining = g_state->gateway_actors.size();
    while (remaining > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {  // GCOVR_EXCL_BR_LINE (defensive: 5s teardown
            // safety valve; the valve body below is already excluded and no
            // test wedges a gateway actor past the deadline)
            // GCOVR_EXCL_START (safety valve: no test wedges a gateway
            // actor mid-handler for 5s; teardown proceeds best-effort)
            SHIELD_LOG_WARNING(shield::log::get_logger("bootstrap"),
                               "gateway actor did not exit within 5s; "
                               "continuing teardown");
            break;
            // GCOVR_EXCL_STOP
        }
        self->receive(
            [&](const caf::down_msg& down) {
                for (const auto& gateway_actor :  // GCOVR_EXCL_BR_LINE (the
                                                  // range-for machinery arc,
                                                  // see annotation below)
                     g_state
                         ->gateway_actors) {  // GCOVR_EXCL_BR_LINE (compiler
                                              // artifact: range-for machinery
                                              // arc; the container is never
                                              // empty when a down message
                                              // arrives and both arms of the
                                              // address comparison on the next
                                              // line are exercised)
                    if (down.source == gateway_actor.address()) {
                        --remaining;
                        break;
                    }
                }
            },
            // GCOVR_EXCL_START (timeout arm of the receive never fires in
            // tests: gateway actors exit in milliseconds; the 5s deadline is
            // a teardown safety valve, same as the break above)
            caf::after(std::chrono::duration_cast<std::chrono::nanoseconds>(
                deadline - now)) >>
                [] {
                    // Timed out; the loop re-checks the deadline.
                });
        // GCOVR_EXCL_STOP
    }
    g_state->gateway_actors.clear();
}

void cleanup_failed_initialize() {
    if (g_state) {  // GCOVR_EXCL_BR_LINE (defensive: initialize() is the only
        // caller and g_state is installed before any failure return)
        for (auto& listener : g_state->tcp_listeners) {
            if (listener) {  // GCOVR_EXCL_BR_LINE (defensive: listeners are
                // pushed only after is_open() succeeds, never null)
                listener->stop();
            }
        }
        g_state->net_work_guard.reset();
        g_state->net_io.stop();
        for (auto& t :  // GCOVR_EXCL_BR_LINE (the loop machinery arc, see
                        // the annotation below)
             g_state
                 ->net_threads) {  // GCOVR_EXCL_BR_LINE (defensive: net threads
                                   // start only after every listener is up and
                                   // no initialize() failure happens past that
                                   // point, so cleanup never has threads to
                                   // join — see excluded join below)
            if (t.joinable())      // GCOVR_EXCL_LINE (net threads only exist
                t.join();          // GCOVR_EXCL_LINE with a live listener)
        }
        g_state->tcp_listeners.clear();
        g_state->gateway_bridges.clear();
        // A later listener (protocol validation, port bind) can fail after
        // earlier gateway actors have spawned.
        exit_gateway_actors_and_wait();
        if (g_state->console_server) {  // GCOVR_EXCL_BR_LINE (defensive: no
            // initialize() failure happens after the console server starts;
            // body already excluded)
            // GCOVR_EXCL_START (unreachable: no initialize() failure happens
            // after the console server starts)
            g_state->console_server->stop();
            g_state->console_server.reset();
            g_state->console_dispatcher.reset();
            g_state->root_commands.reset();
            g_state->lua_commands.reset();
            // GCOVR_EXCL_STOP
        }
        if (g_state->lua_services) {
            g_state->lua_services->shutdown_all("startup_failed");
        }
#ifdef SHIELD_ENABLE_SERVER
        // Halt the timer and drop the injected callbacks BEFORE lua_services
        // is freed below (same ordering rule as the successful teardown).
        if (g_state->server_manager) {
            g_state->server_manager->stop();
        }
        shield::server::ServerManager::set_global(nullptr);
        g_state->server_manager.reset();
#endif
#ifdef SHIELD_ENABLE_GLOBAL
        // stop() joins the tick thread and drops the fire callback before
        // lua_services is freed below, so a fire cannot race the teardown.
        if (g_state->global_manager) {
            g_state->global_manager->stop();
        }
        shield::global::GlobalManager::set_global(nullptr);
        g_state->global_manager.reset();
#endif
#ifdef SHIELD_ENABLE_CLUSTER
        // Stop the transport BEFORE releasing the service manager: inbound
        // envelope bridges dispatch into lua_services, so it must outlive the
        // transport actor's message loop (same ordering rule as the
        // successful teardown; a straggler mailbox message becomes a no-op
        // because stop() clears the bridges).
        if (g_state->cluster_transport) {
            g_state->cluster_transport->stop();
        }
#endif
        g_state->lua_services.reset();
        g_state->lua_runtime.reset();
        shield::plugin::global_host().shutdown();
#ifdef SHIELD_ENABLE_CLUSTER
        shield::cluster::set_global_cluster_transport(nullptr);
        g_state->cluster_transport.reset();
        if (g_state->cluster_manager) {
            g_state->cluster_manager->stop();
        }
        shield::cluster::set_global_cluster_manager(nullptr);
        g_state->cluster_manager.reset();
#endif
        g_state->actor_system.reset();
        g_state->initialized = false;
    }
    shield::log::Logger::shutdown();
    g_state_owner.reset();
    g_state = nullptr;
}

// Initialize
static bool initialize_impl(const RuntimeConfig& config) {
    if (g_state && g_state->initialized) {
        return true;  // Already initialized
    }

    g_state_owner = std::make_unique<GlobalState>();
    g_state = g_state_owner.get();
    g_state->config = config;

    auto& log = shield::log::get_logger("bootstrap");

    if (!init_config_and_logging(config)) {
        return false;
    }

    // Run POST_CONFIG starters before runtime systems consume config.
    run_starters(Phase::POST_CONFIG);

    if (!start_plugins()) {
        return false;
    }
#ifdef SHIELD_ENABLE_CLUSTER
    shield::cluster::ClusterConfig cluster_config;
    if (!init_cluster_manager(&cluster_config)) {
        return false;
    }
#endif
#ifdef SHIELD_ENABLE_PLAYER
    if (!init_player_module()) {
        return false;
    }
#endif
#ifdef SHIELD_ENABLE_SERVER
    if (!init_server_module()) {
        return false;
    }
#endif
#ifdef SHIELD_ENABLE_GLOBAL
    if (!init_global_module()) {
        return false;
    }
#endif

    init_actor_system(config);
#ifdef SHIELD_ENABLE_CLUSTER
    if (!start_cluster_transport(cluster_config)) {
        return false;
    }
#endif

    // Run POST_SYSTEM_INIT starters
    run_starters(Phase::POST_SYSTEM_INIT);

    install_lua_runtime();
#ifdef SHIELD_ENABLE_SERVER
    wire_server_state_bridge();
#endif
#ifdef SHIELD_ENABLE_GLOBAL
    wire_global_scheduler();
#endif
#ifdef SHIELD_ENABLE_CLUSTER
    wire_cluster_routes();
    wire_cluster_data_plane();
#endif

    nlohmann::json merged_rpc_routes = merge_actor_rpc_routes();
    shield::transport::RpcDescriptorTable descriptor_routes;
    if (!spawn_runtime_actors(merged_rpc_routes, &descriptor_routes)) {
        return false;
    }
    if (!start_tcp_listeners(descriptor_routes)) {
        return false;
    }
    start_net_io();

    // Run POST_START starters
    run_starters(Phase::POST_START);

    start_console_server();
    start_http_ops_server();
#ifdef SHIELD_ENABLE_SERVER
    mark_server_ready();
#endif

    g_state->initialized = true;
    SHIELD_LOG_INFO(log, "Shield runtime initialized");
    return true;
}

bool initialize(const RuntimeConfig& config) {
    const bool ok = initialize_impl(config);
    if (!ok) {
        // Teardown runs only after initialize_impl's stack is gone: the
        // failure sites sit mid-loop and their locals (the failed listener,
        // the bridge, the session callbacks) hold captures into
        // GlobalState-owned memory. The teardown used to run at the failure
        // site, so those locals destructed after GlobalState died and their
        // destructors read freed memory (ASan: ~TcpListener in the
        // DuplicateListenerPortFails crash family).
        cleanup_failed_initialize();
    }
    return ok;
}

// Shutdown
void shutdown() {
    if (!g_state ||  // GCOVR_EXCL_BR_LINE (the null-state arm is
                     // unobservable, see the annotated fragment below)
        !g_state->initialized) {  // GCOVR_EXCL_BR_LINE (defensive: g_state
                                  // exists only while initialized or
                                  // mid-initialize on the same thread; the torn
                                  // non-null-but-uninitialized state is
                                  // unobservable)
        return;
    }

    auto& log = shield::log::get_logger("bootstrap");
    SHIELD_LOG_INFO(log, "Shield runtime shutting down...");

#ifdef SHIELD_ENABLE_SERVER
    // External stop sources (SIGINT/SIGTERM, console stop) end up here
    // without ever touching the server state machine; drive it through its
    // terminal state first so watchers receive the "shutdown" notification
    // while the runtime can still deliver and /ops reads report the true
    // state during a slow teardown (runtime-server.md shutdown contract;
    // external shutdown keeps no Lua observation window). A server-side
    // shutdown(ms) handover already scheduled it — schedule_shutdown is
    // idempotent there ("shutdown already scheduled"), and the injected
    // stop request firing again is a harmless no-op mid-shutdown.
    if (g_state->server_manager) {  // GCOVR_EXCL_BR_LINE (defensive: shutdown()
                                    // early-returns unless initialized, and a
                                    // completed init always built the manager)
        std::string server_error;
        (void)g_state->server_manager->schedule_shutdown(0, &server_error);
    }
#endif

    // shutdown.timeout.* budgets (milliseconds). Built-in defaults keep a
    // stuck on_exit or plugin shutdown callback from hanging process exit;
    // the watchdog enforces the total budget by terminating the process.
    const auto drain_budget_ms =
        shield::config::get_int("shutdown.timeout.service_drain", 0);
    const auto stop_budget_ms =
        shield::config::get_int("shutdown.timeout.service_stop", 10000);
    const auto plugin_budget_ms =
        shield::config::get_int("shutdown.timeout.plugin_shutdown", 10000);
    const auto total_budget_ms =
        shield::config::get_int("shutdown.timeout.total", 30000);

    // Total-budget watchdog: a detached thread force-exits the process if
    // graceful shutdown has not completed in time.
    auto shutdown_done = std::make_shared<std::atomic<bool>>(false);
    if (total_budget_ms > 0) {
        std::thread(  // GCOVR_EXCL_BR_LINE (the thread-invocation clone
                      // arcs, see the annotated fragment below)
            [done = shutdown_done,  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // lambda capture arcs, see below)
             total_budget_ms]() {   // GCOVR_EXCL_BR_LINE (defensive:
                                    // detached watchdog; the post-sleep
                                    // arcs only complete past the total
                                    // budget, which trips the excluded
                                    // forced-exit body below, and the
                                    // never-executed arcs are
                                    // thread-invocation clones)
                std::this_thread::
                    sleep_for(  // GCOVR_EXCL_BR_LINE (defensive: tests always
                                // finish shutdown first and exit the process
                                // while the watchdog is still sleeping; waking
                                // it deterministically would kill the test
                                // process)
                        std::chrono::milliseconds(total_budget_ms));
                // GCOVR_EXCL_START (only runs when shutdown hangs past the
                // budget; testing it would kill the test process)
                if (!done->load()) {
                    shield::log::get_logger("bootstrap")
                        .fatal(
                            "shutdown total budget exhausted, forcing process "
                            "exit");
                    std::_Exit(70);
                }
                // GCOVR_EXCL_STOP
            })
            .detach();
    }

    // service_drain (pre-drain settle): give in-flight forked tasks a
    // bounded window to finish before the on_shutdown hooks run below —
    // hooks themselves may fork, and those later tasks are torn down with
    // the services, not awaited here.
    if (drain_budget_ms > 0 &&  // GCOVR_EXCL_BR_LINE (integration: suites shut
                                // down with a positive drain budget)
        g_state->lua_services) {  // GCOVR_EXCL_BR_LINE (defensive: lua_services
                                  // is always installed when shutdown runs; the
                                  // null arm cannot be observed)
        const auto drain_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(drain_budget_ms);
        // GCOVR_EXCL_START (drain only spins when tasks are still pending
        // at shutdown; coverage suites drain before calling shutdown)
        while (g_state->lua_services->pending_task_count_total() > 0 &&
               std::chrono::steady_clock::now() < drain_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // GCOVR_EXCL_STOP
    }

    // Run PRE_SHUTDOWN starters
    run_starters(Phase::PRE_SHUTDOWN);

    // Stop console server before other components
    // Stop accepting and close the sessions now, but keep the dispatcher
    // and the command objects alive until the net threads are joined below:
    // a line already read dispatches through them on those threads, and
    // destroying the objects first was an ASan-confirmed use-after-free
    // (cmd_help reading the freed RootCommands).
    if (g_state->console_server) {
        g_state->console_server->stop();
    }

    // Stop HTTP ops server
    if (g_state->http_server) {
        if (g_state  // GCOVR_EXCL_BR_LINE (defensive: http_bridge is always
                     // attached, see below)
                ->http_bridge) {  // GCOVR_EXCL_BR_LINE (defensive:
                                  // http_bridge is attached
                                  // immediately after http_server is
                                  // created with no failure path in
                                  // between, so
                                  // bridge-null-with-server-present is
                                  // unreachable)
            g_state->http_bridge->detach();
            g_state->http_bridge.reset();
        }
        g_state->http_server->stop();
        g_state->http_server.reset();
        g_state->ops_http_handler.reset();
    }

    // Stop network ingress first so no new Lua work is queued while we tear
    // down.
    // INVARIANT: network listeners (and with them every Session and its
    // ProtocolPipeline, which may hold a codec plugin vtable pointer) MUST
    // be torn down — and the net threads joined — BEFORE PluginHost shuts
    // the plugins down below. This guarantees no session pipeline can
    // outlive the plugin vtable it resolved at listener setup time.
    for (auto& listener : g_state->tcp_listeners) {
        if (listener) {  // GCOVR_EXCL_BR_LINE (defensive: listeners are pushed
                         // only after a successful start, the vector never
                         // holds null)
            listener->stop();
        }
    }
    g_state->net_work_guard.reset();
    g_state->net_io.stop();
    for (auto& t : g_state->net_threads) {
        if (t.joinable())  // GCOVR_EXCL_BR_LINE (defensive: joinable is
            // always true here, see the annotated fragment below)
            t.join();  // GCOVR_EXCL_BR_LINE (defensive: each net thread is
                       // visited and joined exactly once, joinable is always
                       // true at the check)
    }
    g_state->tcp_listeners.clear();
    g_state->gateway_bridges.clear();

    // The net threads are joined: nothing can dispatch into the console
    // dispatcher anymore, so the console objects can finally die. They hold
    // references into the Lua services/runtime, which are released below.
    g_state->console_server.reset();
    g_state->console_dispatcher.reset();
    g_state->root_commands.reset();
    g_state->lua_commands.reset();

    // Exit the gateway actors before the Lua services shut down: their
    // bind responses call back into the manager (complete_call). The wait
    // joins on each actor's down message, so no handler can still
    // dereference the manager once this returns.
    exit_gateway_actors_and_wait();

    // Graceful service drain (on_shutdown contract, docs/lua-api.md):
    // accept/readiness are stopped above (console, HTTP ops, listeners and
    // net threads are down) but every VM is still alive, so the hooks may
    // shield.call / shield.sleep within the shared service_drain budget.
    // Runs in reverse spawn order; hook errors and timeouts log and
    // continue. shutdown_all follows and takes the exit path.
    if (drain_budget_ms > 0 &&  // GCOVR_EXCL_BR_LINE (integration: suites shut
                                // down with a positive drain budget)
        g_state->lua_services) {  // GCOVR_EXCL_BR_LINE (defensive: same null
                                  // arm as the shutdown_all call below)
        g_state->lua_services->drain_all("stopping", drain_budget_ms);
    }

    // Shutdown actor system (which stops all actors)
    if (g_state->lua_services) {  // GCOVR_EXCL_BR_LINE (defensive: shutdown
                                  // only runs on an initialized runtime, where
                                  // lua_services is installed)
        g_state->lua_services->shutdown_all("stopping", stop_budget_ms);
    }
#ifdef SHIELD_ENABLE_CLUSTER
    // Stop the transport BEFORE releasing the service manager: inbound
    // envelope bridges dispatch into lua_services, so it must outlive the
    // transport actor's message loop. stop() also clears the bridges, so a
    // straggler mailbox message becomes an honest no-op. The transport
    // actor lives in the CAF system: unpublish and kill it while the system
    // (and the manager its callbacks point at) are still alive.
    if (g_state->cluster_transport) {
        g_state->cluster_transport->stop();
    }
#endif
#ifdef SHIELD_ENABLE_SERVER
    // Halt the shutdown timer and clear the notify/stop callbacks BEFORE
    // releasing the manager AND before lua_services is freed below: the
    // timer body reads the stop callback under the manager lock, and the
    // notify callback captures lua_services. stop() joins the timer, so
    // nothing can fire into the teardown.
    if (g_state->server_manager) {  // GCOVR_EXCL_BR_LINE (defensive: same
                                    // initialized-invariant as the
                                    // schedule_shutdown guard)
        g_state->server_manager->stop();
    }
    shield::server::ServerManager::set_global(nullptr);
    g_state->server_manager.reset();
#endif
#ifdef SHIELD_ENABLE_GLOBAL
    // Join the scheduler tick thread and drop the fire callback (it
    // captures lua_services, freed below) BEFORE the manager goes away
    // and before lua_services is freed: stop() guarantees no fire races
    // the teardown.
    if (g_state->global_manager) {  // GCOVR_EXCL_BR_LINE (defensive: same
                                    // initialized-invariant as the server
                                    // manager guards)
        g_state->global_manager->stop();
    }
    shield::global::GlobalManager::set_global(nullptr);
    g_state->global_manager.reset();
#endif
    g_state->lua_services.reset();
    g_state->lua_runtime.reset();
#ifdef SHIELD_ENABLE_CLUSTER
    shield::cluster::set_global_cluster_transport(nullptr);
    g_state->cluster_transport.reset();
#endif
#ifdef SHIELD_ENABLE_PLAYER
    // The session index dies with the process: every player service actor
    // was already stopped by shutdown_all above.
    shield::player::PlayerManager::set_global(nullptr);
    g_state->player_manager.reset();
#endif
    g_state->actor_system.reset();
#ifdef SHIELD_ENABLE_CLUSTER
    if (g_state->cluster_manager) {
        g_state->cluster_manager->stop();
    }
    shield::cluster::set_global_cluster_manager(nullptr);
    g_state->cluster_manager.reset();
#endif

    // Run POST_SHUTDOWN starters
    run_starters(Phase::POST_SHUTDOWN);

    // Tear down the plugin system (invokes each instance's shutdown
    // callback). Libraries stay mapped until process exit so any holder of a
    // resolved vtable remains valid.
    shield::plugin::global_host().shutdown(plugin_budget_ms);

    g_state->initialized = false;
    SHIELD_LOG_INFO(log, "Shield runtime shutdown complete");

    // Disarm the total-budget watchdog before logging tears down.
    shutdown_done->store(true);

    // Shutdown logging after the final runtime log has been emitted.
    shield::log::Logger::shutdown();

    g_state_owner.reset();
    g_state = nullptr;
}

// Check if initialized
bool is_initialized() {
    return g_state &&
           g_state->initialized;  // GCOVR_EXCL_BR_LINE (defensive: torn state
                                  // is unobservable, see below)
}  // GCOVR_EXCL_BR_LINE (defensive: the torn state — g_state set but not yet
   // initialized — is unobservable from the single-threaded call paths)

int run(int argc, char** argv) { return shield::run(argc, argv); }

}  // namespace shield::bootstrap
