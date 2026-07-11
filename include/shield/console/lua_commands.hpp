// [SHIELD_CONSOLE] Lua command handler for interactive REPL
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "shield/console/command_dispatcher.hpp"
#include "shield/lua/lua_service.hpp"

namespace shield::console {

/// @brief Handles Lua-related console commands: attach, detach, eval.
///
/// The attach command enters an interactive REPL mode where each line is
/// executed on the target service's Lua VM. The session transitions between
/// command mode and Lua REPL mode.
class LuaCommands {
public:
    LuaCommands(shield::lua::LuaServiceManager& lua_mgr,
                shield::lua::LuaRuntime& lua_rt);

    /// @brief Register attach/eval commands and the Lua line handler
    void register_all(CommandDispatcher& dispatcher);

private:
    void cmd_attach(shield::net::ConsoleSession& session,
                    const std::vector<std::string>& args);
    void cmd_eval(shield::net::ConsoleSession& session,
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
