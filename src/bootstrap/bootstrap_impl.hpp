// [SHIELD_BOOTSTRAP] Internal cross-TU declarations for the bootstrap
// split.
//
// Not a public contract: the public API stays in
// shield/bootstrap/bootstrap.hpp. GlobalState moves here verbatim so
// the bootstrap.cpp / bootstrap_init_*.cpp translation units share one
// definition; g_state is defined (external linkage) in bootstrap.cpp
// and the phase functions are defined flat, one declaration each,
// grouped by the unit that defines them.
#pragma once

#include "shield/base/error.hpp"
#include "shield/bootstrap/bootstrap.hpp"
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

// Global state
struct GlobalState {
    RuntimeConfig config;
    std::unique_ptr<caf::actor_system> actor_system;
    std::unique_ptr<shield::lua::LuaRuntime> lua_runtime;
    // shared_ptr (not unique_ptr): the M4 data-plane closures (envelope
    // bridges, remote-send, proxied-call hook) run on transport/service
    // actor threads and may still be in flight while shutdown rewrites the
    // other half of the pair. Shared ownership keeps every capture valid;
    // the objects themselves fail honestly after their stop() has run.
    std::shared_ptr<shield::lua::LuaServiceManager> lua_services;
    boost::asio::io_context net_io;
    // A work guard keeps net_io.run() alive even when no async operations
    // are pending, preventing the thread pool from spinning down prematurely.
    std::optional<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>
        net_work_guard;
    std::vector<std::thread> net_threads;
    std::vector<std::unique_ptr<shield::lua::LuaGatewayBridge>> gateway_bridges;
    // One gateway actor per listener; exited after the listeners stop and
    // before the Lua services shut down (their bind responses call back into
    // the manager).
    std::vector<caf::actor> gateway_actors;
    std::vector<std::unique_ptr<shield::net::TcpListener>> tcp_listeners;
    std::unique_ptr<shield::net::ConsoleServer> console_server;
    std::unique_ptr<shield::console::CommandDispatcher> console_dispatcher;
    // The dispatcher stores the command handlers as lambdas capturing `this`,
    // so the command objects must outlive initialize()'s scope. They used to
    // be locals: freed while console sessions still dispatched into them
    // (Linux only survived because the allocator handed the freed chunk to
    // the next command object with an identical field layout).
    std::shared_ptr<shield::console::RootCommands> root_commands;
    std::shared_ptr<shield::console::LuaCommands> lua_commands;
    std::unique_ptr<shield::net::HttpServer> http_server;
    std::unique_ptr<shield::console::OpsHttpHandler> ops_http_handler;
    std::unique_ptr<shield::lua::LuaHttpBridge> http_bridge;
#ifdef SHIELD_ENABLE_CLUSTER
    std::unique_ptr<shield::cluster::ClusterManager> cluster_manager;
    std::shared_ptr<shield::cluster::ClusterTransport> cluster_transport;
#endif
#ifdef SHIELD_ENABLE_PLAYER
    std::unique_ptr<shield::player::PlayerManager> player_manager;
#endif
#ifdef SHIELD_ENABLE_SERVER
    std::unique_ptr<shield::server::ServerManager> server_manager;
#endif
#ifdef SHIELD_ENABLE_GLOBAL
    std::unique_ptr<shield::global::GlobalManager> global_manager;
#endif
    bool initialized = false;
};

extern GlobalState* g_state;

// bootstrap_init_runtime.cpp
bool init_config_and_logging(const RuntimeConfig& config);
bool start_plugins();
#ifdef SHIELD_ENABLE_CLUSTER
bool init_cluster_manager(shield::cluster::ClusterConfig* out);
#endif
#ifdef SHIELD_ENABLE_PLAYER
bool init_player_module();
#endif
#ifdef SHIELD_ENABLE_SERVER
bool init_server_module();
#endif
#ifdef SHIELD_ENABLE_GLOBAL
bool init_global_module();
#endif

// bootstrap_init_system.cpp
void init_actor_system(const RuntimeConfig& config);
#ifdef SHIELD_ENABLE_CLUSTER
bool start_cluster_transport(
    const shield::cluster::ClusterConfig& cluster_config);
#endif
void install_lua_runtime();
#ifdef SHIELD_ENABLE_SERVER
void wire_server_state_bridge();
#endif
#ifdef SHIELD_ENABLE_GLOBAL
void wire_global_scheduler();
#endif
#ifdef SHIELD_ENABLE_CLUSTER
void wire_cluster_routes();
void wire_cluster_data_plane();
#endif

// bootstrap_init_actors.cpp
nlohmann::json merge_actor_rpc_routes();
bool spawn_runtime_actors(const nlohmann::json& merged_rpc_routes,
                          shield::transport::RpcDescriptorTable* out);
bool start_tcp_listeners(
    shield::transport::RpcDescriptorTable& descriptor_routes);
void start_net_io();

// bootstrap_init_servers.cpp
void start_console_server();
void start_http_ops_server();
#ifdef SHIELD_ENABLE_SERVER
void mark_server_ready();
#endif

}  // namespace shield::bootstrap
