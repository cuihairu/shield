// [SHIELD_CONSOLE] Command dispatcher implementation
#include "shield/console/command_dispatcher.hpp"

#include <nlohmann/json.hpp>
#include <sstream>

namespace shield::console {

CommandDispatcher::CommandDispatcher() = default;

void CommandDispatcher::register_command(const std::string& name,
                                         const std::string& help,
                                         CommandHandler handler) {
    commands_.push_back({name, help, std::move(handler)});
}

void CommandDispatcher::set_lua_line_handler(
    std::function<void(std::shared_ptr<shield::net::ConsoleSession>,
                       const std::string&)>
        handler) {
    lua_line_handler_ = std::move(handler);
}

void CommandDispatcher::dispatch(std::shared_ptr<shield::net::ConsoleSession> session,
                                  const std::string& line) {
    // Strip leading/trailing whitespace
    std::string trimmed = line;
    auto start = trimmed.find_first_not_of(" \t\r");
    if (start == std::string::npos) return;  // empty line
    trimmed = trimmed.substr(start);
    auto end = trimmed.find_last_not_of(" \t\r");
    if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);

    if (trimmed.empty()) return;

    // If session is attached to a Lua service, route to Lua handler
    if (session->is_attached()) {
        // Special commands that exit Lua mode
        if (trimmed == "detach" || trimmed == "exit") {
            session->set_attached_service("");
            session->clear_multiline();
            nlohmann::json resp = {{"type", "detached"}};
            session->send_line(resp.dump());
            return;
        }
        if (lua_line_handler_) {
            lua_line_handler_(session, trimmed);
        }
        return;
    }

    // Command mode: parse "command arg1 arg2 ..."
    std::istringstream iss(trimmed);
    std::string cmd;
    iss >> cmd;

    std::vector<std::string> args;
    std::string arg;
    while (iss >> arg) {
        args.push_back(std::move(arg));
    }

    // Find and execute handler
    for (const auto& c : commands_) {
        if (c.name == cmd) {
            c.handler(*session, args);
            return;
        }
    }

    // Unknown command
    nlohmann::json resp = {
        {"type", "error"},
        {"message", "Unknown command: " + cmd + ". Type 'help' for available commands."}};
    session->send_line(resp.dump());
}

std::vector<std::pair<std::string, std::string>>
CommandDispatcher::list_commands() const {
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(commands_.size());
    for (const auto& c : commands_) {
        result.emplace_back(c.name, c.help);
    }
    return result;
}

}  // namespace shield::console
