// [SHIELD_CONSOLE] Root command implementation
#include "shield/console/root_commands.hpp"

#include <future>
#include <nlohmann/json.hpp>

#include "cluster_status.hpp"
#include "global_status.hpp"
#include "server_status.hpp"
#include "shield/cluster/cluster_manager.hpp"
#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/plugin/plugin_host.hpp"
#ifdef SHIELD_ENABLE_SERVER
#include "shield/server/server_manager.hpp"
#endif

namespace shield::console {

RootCommands::RootCommands(shield::lua::LuaServiceManager& lua_mgr)
    : lua_mgr_(lua_mgr) {}

void RootCommands::register_all(CommandDispatcher& dispatcher) {
    dispatcher_ = &dispatcher;
    dispatcher.register_command("help", "List all available commands",
                                [this](auto& s, auto& a) { cmd_help(s, a); });
    dispatcher.register_command("root.status",
                                "Overview of services, plugins, cluster",
                                [this](auto& s, auto& a) { cmd_status(s, a); });
    dispatcher.register_command(
        "root.services", "List all Lua services",
        [this](auto& s, auto& a) { cmd_services(s, a); });
    dispatcher.register_command(
        "root.service", "Show details for a specific service",
        [this](auto& s, auto& a) { cmd_service(s, a); });
    dispatcher.register_command(
        "root.plugins", "List all plugin packages and instances",
        [this](auto& s, auto& a) { cmd_plugins(s, a); });
    dispatcher.register_command("root.plugin",
                                "Show details for a specific plugin",
                                [this](auto& s, auto& a) { cmd_plugin(s, a); });
    dispatcher.register_command("root.config",
                                "Show config (root.config [key])",
                                [this](auto& s, auto& a) { cmd_config(s, a); });
    dispatcher.register_command(
        "root.cluster", "Show cluster node status",
        [this](auto& s, auto& a) { cmd_cluster(s, a); });
    dispatcher.register_command("root.server",
                                "Show server state machine status",
                                [this](auto& s, auto& a) { cmd_server(s, a); });
    dispatcher.register_command("root.global",
                                "Show global capability store status",
                                [this](auto& s, auto& a) { cmd_global(s, a); });
    dispatcher.register_command(
        "root.log.level",
        "Get or set log level (root.log.level [debug|info|warn|error])",
        [this](auto& s, auto& a) { cmd_log_level(s, a); });
}

void RootCommands::cmd_help(shield::net::ConsoleSession& session,
                            const std::vector<std::string>& /*args*/) {
    nlohmann::json resp;
    resp["type"] = "result";
    resp["lines"] = nlohmann::json::array();
    if (dispatcher_) {  // GCOVR_EXCL_BR_LINE (defensive: cmd_help only runs
                        // through a registered dispatcher, dispatcher_ is never
                        // null)
        for (const auto& [name, help] : dispatcher_->list_commands()) {
            resp["lines"].push_back(name + "  - " + help);
        }
    }
    // Add Lua REPL commands
    resp["lines"].push_back(
        "attach <service>  - Enter interactive Lua REPL for a service");
    resp["lines"].push_back(
        "detach            - Exit Lua REPL back to command mode");
    resp["lines"].push_back(
        "eval <code>       - Execute Lua code in a sandbox");
    resp["lines"].push_back("exit / quit       - Disconnect");
    session.send_line(resp.dump());
}

void RootCommands::cmd_status(shield::net::ConsoleSession& session,
                              const std::vector<std::string>& /*args*/) {
    nlohmann::json data;

    // Services (via enqueue_forked_task - thread-safe)
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        auto future = promise->get_future();
        lua_mgr_.enqueue_forked_task("", [&mgr = lua_mgr_, promise]() {
            auto names = mgr.list_services();
            promise->set_value(nlohmann::json(names));
        });
        if (future.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready) {
            data["services"] = future.get();
        } else {
            data["services"] = "timeout";
        }
    }

    // Plugins (thread-safe)
    {
        auto& host = shield::plugin::global_host();
        auto instances = host.list_instances();
        nlohmann::json plugins = nlohmann::json::array();
        for (const auto& inst : instances) {
            plugins.push_back(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined nlohmann::json braced-init branches)
                {{"id",
                  inst.id},  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                             // nlohmann::json braced-init branches)
                 {"package", inst.package},
                 {"state", inst.state},
                 {"required", inst.required}});
        }
        data["plugins"] = plugins;
    }

    // Cluster (thread-safe, may be null)
#ifdef SHIELD_ENABLE_CLUSTER
    {
        auto* cm = shield::cluster::global_cluster_manager();
        if (cm) {
            data["cluster"] = build_cluster_status_json();
        }
    }
#endif

    // Server state machine (thread-safe, may be null)
#ifdef SHIELD_ENABLE_SERVER
    {
        auto* sm = shield::server::ServerManager::global();
        if (sm) {
            data["server"] = build_server_status_json();
        }
    }
#endif

    // Global capability store (thread-safe, may be null)
#ifdef SHIELD_ENABLE_GLOBAL
    {
        nlohmann::json global = build_global_status_json();
        if (!global.is_null()) {
            data["global"] = std::move(global);
        }
    }
#endif

    nlohmann::json resp = {
        {"type", "result"},
        {"data", data}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                          // nlohmann::json braced-init branches)
    session.send_line(resp.dump());
}

void RootCommands::cmd_services(shield::net::ConsoleSession& session,
                                const std::vector<std::string>& /*args*/) {
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();
    lua_mgr_.enqueue_forked_task("", [&mgr = lua_mgr_, promise]() {
        auto names = mgr.list_services();
        promise->set_value(nlohmann::json(names));
    });
    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        nlohmann::json resp = {
            {"type", "result"},
            {"data",
             future.get()}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                              // nlohmann::json braced-init branches)
        session.send_line(resp.dump());
    } else {
        nlohmann::json resp = {
            {"type", "error"},
            {"message",
             "timeout querying services"}};  // GCOVR_EXCL_BR_LINE (compiler
                                             // artifact: inlined nlohmann::json
                                             // braced-init branches)
        session.send_line(resp.dump());
    }
}

void RootCommands::cmd_service(shield::net::ConsoleSession& session,
                               const std::vector<std::string>& args) {
    if (args.empty()) {
        nlohmann::json resp = {
            {"type", "error"},
            {"message",
             "Usage: root.service <name>"}};  // GCOVR_EXCL_BR_LINE (compiler
                                              // artifact: inlined
                                              // nlohmann::json braced-init
                                              // branches)
        session.send_line(resp.dump());
        return;
    }
    const auto& name = args[0];
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();
    // GCOVR_EXCL_BR_START (compiler artifact: lambda-body STL branches
    // are attributed to the capture line)
    lua_mgr_.enqueue_forked_task(
        "", [&mgr = lua_mgr_,
             name,  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda-body STL
                    // branches attributed to this line)
             promise]() {  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda-body
                           // STL branches attributed to this line)
            nlohmann::json data;
            data["name"] = name;
            auto id = mgr.query_service(name);
            data["exists"] = !id.empty();
            if (!id.empty()) {
                data["id"] = id;
            }
            promise->set_value(data);
        });
    // GCOVR_EXCL_BR_STOP
    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        nlohmann::json resp = {
            {"type", "result"},
            {"data",
             future.get()}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                              // nlohmann::json braced-init branches)
        session.send_line(resp.dump());
    } else {
        nlohmann::json resp = {
            {"type", "error"},
            {"message",
             "timeout querying service"}};  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: inlined nlohmann::json
                                            // braced-init branches)
        session.send_line(resp.dump());
    }
}

void RootCommands::cmd_plugins(shield::net::ConsoleSession& session,
                               const std::vector<std::string>& /*args*/) {
    auto& host = shield::plugin::global_host();
    nlohmann::json data;

    auto packages = host.list_packages();
    data["packages"] = nlohmann::json::array();
    for (const auto& pkg : packages) {
        data["packages"]
            .push_back(  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                         // nlohmann::json braced-init branches)
                {{"id",
                  pkg.id},  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                            // nlohmann::json braced-init branches)
                 {"version", pkg.version},
                 {"kind", pkg.kind},
                 {"provides", pkg.provides}});
    }

    auto instances = host.list_instances();
    data["instances"] = nlohmann::json::array();
    for (const auto& inst : instances) {
        data["instances"]
            .push_back(  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                         // nlohmann::json braced-init branches)
                {{"id",
                  inst.id},  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                             // nlohmann::json braced-init branches)
                 {"package", inst.package},
                 {"state", inst.state},
                 {"required", inst.required}});
    }

    nlohmann::json resp = {
        {"type", "result"},
        {"data", data}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                          // nlohmann::json braced-init branches)
    session.send_line(resp.dump());
}

void RootCommands::cmd_plugin(shield::net::ConsoleSession& session,
                              const std::vector<std::string>& args) {
    if (args.empty()) {
        nlohmann::json resp = {
            {"type", "error"},
            {"message",
             "Usage: root.plugin <id>"}};  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: inlined nlohmann::json
                                           // braced-init branches)
        session.send_line(resp.dump());
        return;
    }
    auto& host = shield::plugin::global_host();
    auto* inst = host.find_instance(args[0]);
    if (!inst) {
        nlohmann::json resp = {
            {"type", "error"},
            {"message",
             "Plugin instance not found: " +
                 args[0]}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                             // nlohmann::json braced-init branches)
        session.send_line(resp.dump());
        return;
    }
    // Convert state enum to string
    std::string state_str;
    switch (inst->state) {  // GCOVR_EXCL_BR_LINE (defensive: exhaustive switch
                            // over all six State values, missed arm is the
                            // out-of-range guard)
        case shield::plugin::State::planned:
            state_str = "planned";
            break;
        case shield::plugin::State::loaded:
            state_str = "loaded";
            break;
        case shield::plugin::State::started:
            state_str = "started";
            break;
        case shield::plugin::State::unavailable:
            state_str = "unavailable";
            break;
        case shield::plugin::State::failed:
            state_str = "failed";
            break;
        case shield::plugin::State::stopped:
            state_str = "stopped";
            break;
    }
    nlohmann::json data = {
        {"id", inst->id},
        {"package", inst->package
                        ? inst->package->manifest.id
                        : ""},  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined nlohmann::json braced-init branches)
        {"state", state_str},
        {"last_error", inst->last_error},
        {"dependencies",
         inst->dep_ids}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                           // nlohmann::json braced-init branches)
    nlohmann::json resp = {
        {"type", "result"},
        {"data", data}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                          // nlohmann::json braced-init branches)
    session.send_line(resp.dump());
}

void RootCommands::cmd_config(shield::net::ConsoleSession& session,
                              const std::vector<std::string>& args) {
    auto& cfg = shield::config::global_config();
    if (args.empty()) {
        // Dump entire config as JSON
        nlohmann::json resp = {
            {"type", "result"},
            {"data", nlohmann::json::parse(
                         cfg.to_json())}};  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: inlined nlohmann::json
                                            // braced-init branches)
        session.send_line(resp.dump());
    } else {
        const auto& key = args[0];
        if (!cfg.has(key)) {
            nlohmann::json resp = {
                {"type", "error"},
                {"message",
                 "Config key not found: " +
                     key}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                             // nlohmann::json braced-init branches)
            session.send_line(resp.dump());
            return;
        }
        auto* val = cfg.get_value(key);
        if (!val) {  // GCOVR_EXCL_BR_LINE (defensive: ConfigValue has no empty
                     // alternative, has()==true implies a non-null get_value())
            nlohmann::json resp = {
                {"type", "error"},
                // GCOVR_EXCL_START (unreachable: ConfigValue has no empty
                // alternative, so a key found by has() always yields a
                // non-null get_value())
                {"message", "Config key has no value: " + key}};
            session.send_line(resp.dump());
            return;
        }
        // GCOVR_EXCL_STOP
        // ConfigValue is a variant; serialize based on type
        nlohmann::json data;
        if (auto* s = std::get_if<std::string>(val)) {
            data = *s;
        } else if (auto* i = std::get_if<int64_t>(val)) {
            data = *i;
        } else if (auto* d = std::get_if<double>(val)) {
            data = *d;
        } else if (auto* b = std::get_if<bool>(val)) {
            data = *b;
            // GCOVR_EXCL_BR_START (defensive: all ConfigValue alternatives
            // are matched by the preceding arms)
        } else if (auto* v = std::get_if<std::vector<
                       std::string>>(  // GCOVR_EXCL_BR_LINE (defensive: all
                                       // ConfigValue alternatives are matched
                                       // above)
                       val)) {  // GCOVR_EXCL_BR_LINE (defensive: all five
                                // ConfigValue alternatives are matched by the
                                // preceding arms)
            data = *v;
            // GCOVR_EXCL_BR_STOP
        }
        nlohmann::json resp = {
            {"type", "result"},
            {"data", data}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                              // nlohmann::json braced-init branches)
        session.send_line(resp.dump());
    }
}

void RootCommands::cmd_cluster(shield::net::ConsoleSession& session,
                               const std::vector<std::string>& /*args*/) {
#ifdef SHIELD_ENABLE_CLUSTER
    auto* cm = shield::cluster::global_cluster_manager();
    if (!cm) {
        nlohmann::json resp = {{"type", "error"},
                               {"message", "Cluster not enabled"}};
        session.send_line(resp.dump());
        return;
    }
    nlohmann::json data = build_cluster_status_json();
    nlohmann::json resp = {{"type", "result"}, {"data", data}};
    session.send_line(resp.dump());
#else
    nlohmann::json resp = {
        {"type", "error"},
        {"message",
         "Cluster not compiled"}};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // inlined nlohmann::json braced-init
                                    // branches)
    session.send_line(resp.dump());
#endif
}

void RootCommands::cmd_server(shield::net::ConsoleSession& session,
                              const std::vector<std::string>& /*args*/) {
#ifdef SHIELD_ENABLE_SERVER
    auto* sm = shield::server::ServerManager::global();
    if (!sm) {
        nlohmann::json resp = {{"type", "error"},
                               {"message", "Server not enabled"}};
        session.send_line(resp.dump());
        return;
    }
    nlohmann::json resp = {{"type", "result"},
                           {"data", build_server_status_json()}};
    session.send_line(resp.dump());
#else
    nlohmann::json resp = {
        {"type", "error"},
        {"message",
         "Server not compiled"}};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                   // inlined nlohmann::json braced-init
                                   // branches)
    session.send_line(resp.dump());
#endif
}

void RootCommands::cmd_global(shield::net::ConsoleSession& session,
                              const std::vector<std::string>& /*args*/) {
#ifdef SHIELD_ENABLE_GLOBAL
    nlohmann::json global = build_global_status_json();
    if (global.is_null()) {
        nlohmann::json resp = {{"type", "error"},
                               {"message", "Global not enabled"}};
        session.send_line(resp.dump());
        return;
    }
    nlohmann::json resp = {{"type", "result"}, {"data", global}};
    session.send_line(resp.dump());
#else
    nlohmann::json resp = {
        {"type", "error"},
        {"message",
         "Global not compiled"}};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                   // inlined nlohmann::json braced-init
                                   // branches)
    session.send_line(resp.dump());
#endif
}

void RootCommands::cmd_log_level(shield::net::ConsoleSession& session,
                                 const std::vector<std::string>& args) {
    if (args.empty()) {
        using shield::log::Level;
        std::string current;
        switch (shield::log::Logger::
                    get_global_level()) {  // GCOVR_EXCL_BR_LINE (defensive:
                                           // exhaustive switch over all five
                                           // Level values, missed arm is the
                                           // out-of-range guard)
            case Level::Debug:
                current = "debug";
                break;
            case Level::Info:
                current = "info";
                break;
            case Level::Warning:
                current = "warn";
                break;
            case Level::Error:
                current = "error";
                break;
            case Level::Fatal:
                current = "fatal";
                break;
        }
        nlohmann::json resp = {
            {"type", "result"},
            {"data", current}};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // inlined nlohmann::json braced-init branches)
        session.send_line(resp.dump());
        return;
    }
    const auto& level_str = args[0];
    shield::log::Level level;
    if (level_str == "debug") {
        level = shield::log::Level::Debug;
    } else if (level_str == "info") {
        level = shield::log::Level::Info;
    } else if (level_str == "warn" || level_str == "warning") {
        level = shield::log::Level::Warning;
    } else if (level_str == "error") {
        level = shield::log::Level::Error;
    } else {
        nlohmann::json resp = {{"type", "error"},
                               {"message",
                                "Invalid level. Use: debug, info, warn, "
                                "error"}};  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: inlined nlohmann::json
                                            // braced-init branches)
        session.send_line(resp.dump());
        return;
    }
    shield::log::Logger::set_global_level(level);
    nlohmann::json resp = {
        {"type", "result"},
        {"data",
         "Log level set to " +
             level_str}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                           // nlohmann::json braced-init branches)
    session.send_line(resp.dump());
}

}  // namespace shield::console
