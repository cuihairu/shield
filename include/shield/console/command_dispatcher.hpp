// [SHIELD_CONSOLE] Command dispatcher for console session
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "shield/net/console_session.hpp"

namespace shield::console {

/// @brief Handler function signature: receives the session and parsed arguments
using CommandHandler = std::function<void(
    shield::net::ConsoleSession& session, const std::vector<std::string>& args)>;

/// @brief Dispatches text commands from console sessions to registered handlers.
///
/// Supports two modes:
/// - Command mode: lines are parsed as "command arg1 arg2 ..." and dispatched
/// - Lua REPL mode (when session.is_attached()): lines are routed to the
///   registered Lua line handler
class CommandDispatcher {
public:
    CommandDispatcher();

    /// @brief Register a command handler
    /// @param name Command name (e.g. "root.status", "attach", "help")
    /// @param help One-line help text
    /// @param handler The handler function
    void register_command(const std::string& name, const std::string& help,
                          CommandHandler handler);

    /// @brief Set the handler for Lua REPL lines (when session is attached)
    /// @param handler Called with (session, raw_line) for each line in Lua mode
    void set_lua_line_handler(
        std::function<void(std::shared_ptr<shield::net::ConsoleSession>,
                           const std::string&)>
            handler);

    /// @brief Dispatch a line from a console session
    ///
    /// If the session is attached to a Lua service, routes to the Lua line
    /// handler (unless the line is "detach" or "exit"). Otherwise, parses
    /// the line as a command and dispatches to the matching handler.
    void dispatch(std::shared_ptr<shield::net::ConsoleSession> session,
                  const std::string& line);

    /// @brief Get all registered commands with their help text
    std::vector<std::pair<std::string, std::string>> list_commands() const;

private:
    struct Command {
        std::string name;
        std::string help;
        CommandHandler handler;
    };

    std::vector<Command> commands_;
    std::function<void(std::shared_ptr<shield::net::ConsoleSession>,
                       const std::string&)>
        lua_line_handler_;
};

}  // namespace shield::console
