// [SHIELD_CONSOLE] Root command handler for C++ subsystem queries
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "shield/console/command_dispatcher.hpp"
#include "shield/lua/lua_service.hpp"

namespace shield::console {

/// @brief Handles all "root.*" diagnostic commands that query C++ subsystems.
///
/// Thread-safe subsystems (PluginHost, Config, ClusterManager, Logger) are
/// queried directly. Lua service queries are dispatched through
/// enqueue_forked_task to run on the Lua worker thread.
class RootCommands {
public:
    RootCommands(shield::lua::LuaServiceManager& lua_mgr);

    /// @brief Register all root.* commands with the dispatcher
    void register_all(CommandDispatcher& dispatcher);

private:
    void cmd_help(shield::net::ConsoleSession& session,
                  const std::vector<std::string>& args);
    void cmd_status(shield::net::ConsoleSession& session,
                    const std::vector<std::string>& args);
    void cmd_services(shield::net::ConsoleSession& session,
                      const std::vector<std::string>& args);
    void cmd_service(shield::net::ConsoleSession& session,
                     const std::vector<std::string>& args);
    void cmd_plugins(shield::net::ConsoleSession& session,
                     const std::vector<std::string>& args);
    void cmd_plugin(shield::net::ConsoleSession& session,
                    const std::vector<std::string>& args);
    void cmd_config(shield::net::ConsoleSession& session,
                    const std::vector<std::string>& args);
    void cmd_cluster(shield::net::ConsoleSession& session,
                     const std::vector<std::string>& args);
    void cmd_log_level(shield::net::ConsoleSession& session,
                       const std::vector<std::string>& args);

    shield::lua::LuaServiceManager& lua_mgr_;

    // Back-pointer to dispatcher for help generation
    CommandDispatcher* dispatcher_{nullptr};
};

}  // namespace shield::console
