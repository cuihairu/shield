// [SHIELD_CONSOLE] Lua command handler for interactive REPL
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "shield/console/command_dispatcher.hpp"
#include "shield/lua/lua_service.hpp"

namespace shield::console {

/// @brief Handles Lua-related console commands: attach, detach, eval, and
/// the L2 restricted-inspect family (lua.inspect / lua.snapshot /
/// lua.diff).
///
/// The attach command enters an interactive REPL mode where each line is
/// executed on the target service's Lua VM. The session transitions between
/// command mode and Lua REPL mode.
class LuaCommands {
public:
    LuaCommands(shield::lua::LuaServiceManager& lua_mgr,
                shield::lua::LuaRuntime& lua_rt);

    /// @brief Register attach/eval and L2 inspect commands plus the Lua
    /// line handler
    void register_all(CommandDispatcher& dispatcher);

private:
    void cmd_attach(shield::net::ConsoleSession& session,
                    const std::vector<std::string>& args);
    void cmd_eval(shield::net::ConsoleSession& session,
                  const std::vector<std::string>& args);

    /// @brief lua.inspect <service> <field>: registry-locked read-only
    /// projection of one service's L1 gauges. Fields: summary (everything),
    /// memory, coroutines, timers, pending_calls. No Lua state is touched
    /// and no actor round trip is made.
    void cmd_inspect(shield::net::ConsoleSession& session,
                     const std::vector<std::string>& args);

    /// @brief lua.snapshot <service> [name]: freeze the current gauges
    /// under a name (auto-named snap-N when omitted; same name replaces).
    void cmd_snapshot(shield::net::ConsoleSession& session,
                      const std::vector<std::string>& args);

    /// @brief lua.diff <service> <a> <b>: per-field delta between two
    /// stored snapshots.
    void cmd_diff(shield::net::ConsoleSession& session,
                  const std::vector<std::string>& args);

    /// @brief Handle a line in Lua REPL mode (called by dispatcher)
    void handle_lua_line(std::shared_ptr<shield::net::ConsoleSession> session,
                         const std::string& line);

    /// @brief Try to compile and execute code. Returns true if executed
    /// (complete statement), false if more input is needed.
    bool try_execute(std::shared_ptr<shield::net::ConsoleSession> session,
                     const std::string& service, const std::string& code);

    shield::lua::LuaServiceManager& lua_mgr_;
    shield::lua::LuaRuntime& lua_rt_;
};

}  // namespace shield::console
