// [SHIELD_CONSOLE] Lua command implementation
#include "shield/console/lua_commands.hpp"

#include <future>
#include <nlohmann/json.hpp>

#include "shield/log/logger.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace shield::console {

LuaCommands::LuaCommands(shield::lua::LuaServiceManager& lua_mgr,
                         shield::lua::LuaRuntime& lua_rt)
    : lua_mgr_(lua_mgr), lua_rt_(lua_rt) {}

void LuaCommands::register_all(CommandDispatcher& dispatcher) {
    dispatcher.register_command(
        "attach", "Enter interactive Lua REPL for a service (attach <service>)",
        [this](auto& s, auto& a) { cmd_attach(s, a); });
    dispatcher.register_command(
        "eval", "Execute Lua code in a sandbox (eval <code>)",
        [this](auto& s, auto& a) { cmd_eval(s, a); });

    // Set the Lua line handler for attached sessions
    dispatcher.set_lua_line_handler(
        [this](std::shared_ptr<shield::net::ConsoleSession> session,
               const std::string& line) { handle_lua_line(session, line); });
}

void LuaCommands::cmd_attach(shield::net::ConsoleSession& session,
                              const std::vector<std::string>& args) {
    if (args.empty()) {
        nlohmann::json resp = {{"type", "error"},
                               {"message", "Usage: attach <service>"}};
        session.send_line(resp.dump());
        return;
    }

    const auto& service_name = args[0];

    // Verify the service exists (via enqueue_forked_task for thread safety)
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    lua_mgr_.enqueue_forked_task(
        "", [&mgr = lua_mgr_, service_name, promise]() {
            auto id = mgr.query_service(service_name);
            promise->set_value(!id.empty());
        });

    if (future.wait_for(std::chrono::seconds(2)) !=
        std::future_status::ready) {
        nlohmann::json resp = {
            {"type", "error"}, {"message", "timeout verifying service"}};
        session.send_line(resp.dump());
        return;
    }

    if (!future.get()) {
        nlohmann::json resp = {
            {"type", "error"},
            {"message", "Service not found: " + service_name}};
        session.send_line(resp.dump());
        return;
    }

    // Enter REPL mode
    session.set_attached_service(service_name);
    session.clear_multiline();
    nlohmann::json resp = {{"type", "attached"}, {"service", service_name}};
    session.send_line(resp.dump());
}

void LuaCommands::handle_lua_line(
    std::shared_ptr<shield::net::ConsoleSession> session,
    const std::string& line) {
    const auto& service = session->attached_service();

    // If we're accumulating multiline input
    if (!session->multiline_buffer().empty()) {
        session->append_multiline("\n" + line);
        if (try_execute(session, service, session->multiline_buffer())) {
            session->clear_multiline();
        } else {
            // Need more input
            nlohmann::json resp = {{"type", "continue"}};
            session->send_line(resp.dump());
        }
        return;
    }

    // Single line attempt
    if (try_execute(session, service, line)) {
        // Executed successfully
    } else {
        // Incomplete statement, start multiline
        session->append_multiline(line);
        nlohmann::json resp = {{"type", "continue"}};
        session->send_line(resp.dump());
    }
}

bool LuaCommands::try_execute(
    std::shared_ptr<shield::net::ConsoleSession> session,
    const std::string& service, const std::string& code) {
    // First try to compile to check if the statement is complete
    {
        // Use a temporary Lua state just for compilation check
        lua_State* L = luaL_newstate();
        int status = luaL_loadbuffer(L, code.c_str(), code.size(), "=repl");
        if (status == LUA_ERRSYNTAX) {
            const char* msg = lua_tostring(L, -1);
            std::string err_msg = msg ? msg : "syntax error";
            // Check if it's an "eof" error (incomplete statement)
            if (err_msg.find("<eof>") != std::string::npos ||
                err_msg.find("eof") != std::string::npos) {
                lua_close(L);
                return false;  // Need more input
            }
            // Real syntax error
            lua_close(L);
            nlohmann::json resp = {{"type", "error"}, {"message", err_msg}};
            session->send_line(resp.dump());
            return true;  // Handled (error reported)
        }
        lua_close(L);
    }

    // Code compiles - execute on the worker thread via enqueue_forked_task
    auto promise =
        std::make_shared<std::promise<std::pair<bool, nlohmann::json>>>();
    auto future = promise->get_future();

    lua_mgr_.enqueue_forked_task(
        "", [&mgr = lua_mgr_, service, code, promise]() {
            nlohmann::json result;
            std::string error;
            bool ok = mgr.exec_lua(service, code, &result, &error);
            if (ok) {
                promise->set_value({true, result});
            } else {
                promise->set_value({false, nlohmann::json(error)});
            }
        });

    // Wait with timeout
    if (future.wait_for(std::chrono::seconds(5)) !=
        std::future_status::ready) {
        nlohmann::json resp = {
            {"type", "error"}, {"message", "execution timeout (5s)"}};
        session->send_line(resp.dump());
        return true;
    }

    auto [ok, data] = future.get();
    if (ok) {
        // Send each return value as a result
        if (data.is_array() && data.empty()) {
            // No return values
            nlohmann::json resp = {{"type", "result"}, {"data", nullptr}};
            session->send_line(resp.dump());
        } else if (data.is_array() && data.size() == 1) {
            // Single return value
            nlohmann::json resp = {{"type", "result"}, {"data", data[0]}};
            session->send_line(resp.dump());
        } else {
            // Multiple return values
            nlohmann::json resp = {{"type", "result"}, {"data", data}};
            session->send_line(resp.dump());
        }
    } else {
        nlohmann::json resp = {
            {"type", "error"}, {"message", data.get<std::string>()}};
        session->send_line(resp.dump());
    }
    return true;
}

void LuaCommands::cmd_eval(shield::net::ConsoleSession& session,
                            const std::vector<std::string>& args) {
    if (args.empty()) {
        nlohmann::json resp = {{"type", "error"},
                               {"message", "Usage: eval <lua code>"}};
        session.send_line(resp.dump());
        return;
    }

    // Join args into a single code string
    std::string code;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) code += " ";
        code += args[i];
    }

    // Execute in a temporary sandbox VM (not attached to any service)
    auto promise =
        std::make_shared<std::promise<std::pair<bool, nlohmann::json>>>();
    auto future = promise->get_future();

    lua_mgr_.enqueue_forked_task(
        "", [&rt = lua_rt_, code, promise]() {
            // Create a temporary VM for sandboxed eval
            auto vm = rt.create_vm();
            rt.register_api(vm);
            nlohmann::json result;
            std::string error;
            bool ok = rt.exec_lua(vm, code, &result, &error);
            if (ok) {
                promise->set_value({true, result});
            } else {
                promise->set_value({false, nlohmann::json(error)});
            }
        });

    if (future.wait_for(std::chrono::seconds(5)) !=
        std::future_status::ready) {
        nlohmann::json resp = {
            {"type", "error"}, {"message", "execution timeout (5s)"}};
        session.send_line(resp.dump());
        return;
    }

    auto [ok, data] = future.get();
    if (ok) {
        if (data.is_array() && data.empty()) {
            nlohmann::json resp = {{"type", "result"}, {"data", nullptr}};
            session.send_line(resp.dump());
        } else if (data.is_array() && data.size() == 1) {
            nlohmann::json resp = {{"type", "result"}, {"data", data[0]}};
            session.send_line(resp.dump());
        } else {
            nlohmann::json resp = {{"type", "result"}, {"data", data}};
            session.send_line(resp.dump());
        }
    } else {
        nlohmann::json resp = {
            {"type", "error"}, {"message", data.get<std::string>()}};
        session.send_line(resp.dump());
    }
}

}  // namespace shield::console
