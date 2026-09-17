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
    dispatcher.register_command("eval",
                                "Execute Lua code in a sandbox (eval <code>)",
                                [this](auto& s, auto& a) { cmd_eval(s, a); });
    // L2 restricted inspect: read-only registry projections, snapshot
    // capture and diff (see docs/ops-lua-console.md). All fields come from
    // registry-locked manager reads — no Lua state is touched.
    dispatcher.register_command(
        "lua.inspect",
        "Read-only service diagnostics "
        "(lua.inspect <service> "
        "[summary|memory|coroutines|timers|pending_calls|refs [depth]])",
        [this](auto& s, auto& a) { cmd_inspect(s, a); });
    dispatcher.register_command(
        "lua.snapshot",
        "Freeze a service's current gauges (lua.snapshot <service> [name])",
        [this](auto& s, auto& a) { cmd_snapshot(s, a); });
    dispatcher.register_command(
        "lua.diff", "Compare two snapshots (lua.diff <service> <a> <b>)",
        [this](auto& s, auto& a) { cmd_diff(s, a); });

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

    // Verify the service exists. query_service only reads the registry under
    // a shared lock, so it is safe to call directly from the console thread.
    if (lua_mgr_.query_service(service_name).empty()) {
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

    // Code compiles - execute on the owning service actor by enqueueing the
    // task with the target service id (exec_lua must run in that actor's
    // dispatch context).
    auto promise =
        std::make_shared<std::promise<std::pair<bool, nlohmann::json>>>();
    auto future = promise->get_future();

    const uint64_t task_id = lua_mgr_.enqueue_forked_task(
        service, [&mgr = lua_mgr_, service, code, promise]() {
            nlohmann::json result;
            std::string error;
            bool ok = mgr.exec_lua(service, code, &result, &error);
            if (ok) {
                promise->set_value({true, result});
            } else {
                promise->set_value({false, nlohmann::json(error)});
            }
        });

    // Task rejected (service gone): report immediately instead of timing out.
    if (task_id == 0) {
        nlohmann::json resp = {{"type", "error"},
                               {"message", "service unavailable: " + service}};
        session->send_line(resp.dump());
        return true;
    }

    // Wait with timeout
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        nlohmann::json resp = {{"type", "error"},
                               {"message", "execution timeout (5s)"}};
        session->send_line(resp.dump());
        return true;
    }

    auto [ok, data] = future.get();
    if (ok) {
        // Send each return value as a result
        if (data.is_array() && data.empty()) {
            // No return values
            // Unreachable: exec_lua leaves *result null when the chunk
            // yields no values, so data is never an empty array here.
            // Unreachable: exec_lua leaves *result null when the chunk
            // yields no values, so data is never an empty array here.
            // GCOVR_EXCL_START
            nlohmann::json resp = {{"type", "result"}, {"data", nullptr}};
            // GCOVR_EXCL_STOP
            session->send_line(resp.dump());  // GCOVR_EXCL_LINE
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
        nlohmann::json resp = {{"type", "error"},
                               {"message", data.get<std::string>()}};
        session->send_line(resp.dump());
    }
    return true;
}

void LuaCommands::cmd_inspect(shield::net::ConsoleSession& session,
                              const std::vector<std::string>& args) {
    const auto usage = [&session]() {
        nlohmann::json resp = {
            {"type", "error"},
            {"message",
             "Usage: lua.inspect <service> "
             "[summary|memory|coroutines|timers|pending_calls|refs [depth]]"}};
        session.send_line(resp.dump());
    };
    if (args.size() < 2) {
        usage();
        return;
    }
    // Resolve through the published-name registry (aliases included), then
    // read the detail snapshot under the same registry lock — a console
    // thread read, no Lua state, no actor round trip.
    const std::string service_id = lua_mgr_.query_service(args[0]);
    const std::optional<nlohmann::json> detail =
        service_id.empty() ? std::optional<nlohmann::json>{}
                           : lua_mgr_.service_detail(service_id);
    if (!detail.has_value()) {
        nlohmann::json resp = {{"type", "error"},
                               {"message", "Service not found: " + args[0]}};
        session.send_line(resp.dump());
        return;
    }
    const std::string& field = args[1];
    nlohmann::json data;
    if (field == "summary") {
        data = std::move(*detail);
    } else if (field == "memory") {
        data = nlohmann::json{{"name", args[0]},
                              {"memory_kb", (*detail)["memory_kb"]}};
    } else if (field == "coroutines") {
        data = nlohmann::json{{"name", args[0]},
                              {"coroutines", (*detail)["coroutines"]}};
    } else if (field == "timers") {
        data =
            nlohmann::json{{"name", args[0]}, {"timers", (*detail)["timers"]}};
    } else if (field == "pending_calls") {
        data = nlohmann::json{{"name", args[0]},
                              {"pending_calls", (*detail)["pending_calls"]}};
    } else if (field == "refs") {
        // The only subcommand that leaves the registry-read fast path: the
        // walk runs on the owning service actor thread (bounded fork task,
        // 2s dispatch wait). depth is optional and clamped to [1,8].
        int depth = 4;
        if (args.size() > 2) {
            depth = std::atoi(args[2].c_str());
            if (depth < 1 || depth > 8) {
                usage();
                return;
            }
        }
        std::string refs_error;
        const std::optional<nlohmann::json> refs =
            lua_mgr_.inspect_refs(service_id, depth, 20000, &refs_error);
        if (!refs.has_value()) {
            nlohmann::json resp = {{"type", "error"}, {"message", refs_error}};
            session.send_line(resp.dump());
            return;
        }
        data = std::move(*refs);
    } else {
        usage();
        return;
    }
    nlohmann::json resp = {{"type", "result"}, {"data", std::move(data)}};
    session.send_line(resp.dump());
}

void LuaCommands::cmd_snapshot(shield::net::ConsoleSession& session,
                               const std::vector<std::string>& args) {
    if (args.empty()) {
        nlohmann::json resp = {
            {"type", "error"},
            {"message", "Usage: lua.snapshot <service> [name]"}};
        session.send_line(resp.dump());
        return;
    }
    // An unresolved name and an incarnation that left the registry between
    // the two locked reads both surface as "not found".
    const std::string service_id = lua_mgr_.query_service(args[0]);
    std::string error;
    const std::optional<nlohmann::json> snap =
        service_id.empty()
            ? std::optional<nlohmann::json>{}
            : lua_mgr_.capture_inspect_snapshot(
                  service_id, args.size() > 1 ? args[1] : std::string(),
                  &error);
    if (!snap.has_value()) {
        nlohmann::json resp = {{"type", "error"},
                               {"message", "Service not found: " + args[0]}};
        session.send_line(resp.dump());
        return;
    }
    nlohmann::json resp = {{"type", "result"}, {"data", std::move(*snap)}};
    session.send_line(resp.dump());
}

void LuaCommands::cmd_diff(shield::net::ConsoleSession& session,
                           const std::vector<std::string>& args) {
    if (args.size() < 3) {
        nlohmann::json resp = {{"type", "error"},
                               {"message",
                                "Usage: lua.diff <service> <snapshot_a> "
                                "<snapshot_b>"}};
        session.send_line(resp.dump());
        return;
    }
    // Snapshot errors beyond name resolution (missing snapshots, unknown
    // snapshot names) carry their own message.
    const std::string service_id = lua_mgr_.query_service(args[0]);
    std::string error;
    const std::optional<nlohmann::json> diff =
        service_id.empty() ? std::optional<nlohmann::json>{}
                           : lua_mgr_.diff_inspect_snapshots(
                                 service_id, args[1], args[2], &error);
    if (!diff.has_value()) {
        nlohmann::json resp = {
            {"type", "error"},
            {"message", service_id.empty() ? "Service not found: " + args[0]
                                           : "diff failed: " + error}};
        session.send_line(resp.dump());
        return;
    }
    nlohmann::json resp = {{"type", "result"}, {"data", std::move(*diff)}};
    session.send_line(resp.dump());
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

    // Execute in a temporary sandbox VM (not attached to any service). The VM
    // is created, used and destroyed on this console thread, so no other
    // thread ever touches its lua_State; no actor handoff is needed.
    auto vm = lua_rt_.create_vm();
    lua_rt_.register_api(vm);
    nlohmann::json result;
    std::string error;
    const bool ok = lua_rt_.exec_lua(vm, code, &result, &error);
    nlohmann::json data;
    if (ok) {
        data = std::move(result);
    } else {
        data = nlohmann::json(error);
    }
    if (ok) {
        if (data.is_array() && data.empty()) {
            // Unreachable: same as above -- exec_lua produces null, never
            // an empty array, when the chunk returns nothing.
            // GCOVR_EXCL_START
            nlohmann::json resp = {{"type", "result"}, {"data", nullptr}};
            // GCOVR_EXCL_STOP
            session.send_line(resp.dump());  // GCOVR_EXCL_LINE
        } else if (data.is_array() && data.size() == 1) {
            nlohmann::json resp = {{"type", "result"}, {"data", data[0]}};
            session.send_line(resp.dump());
        } else {
            nlohmann::json resp = {{"type", "result"}, {"data", data}};
            session.send_line(resp.dump());
        }
    } else {
        nlohmann::json resp = {{"type", "error"},
                               {"message", data.get<std::string>()}};
        session.send_line(resp.dump());
    }
}

}  // namespace shield::console
