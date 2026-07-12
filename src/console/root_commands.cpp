// [SHIELD_CONSOLE] Root command implementation
#include "shield/console/root_commands.hpp"

#include <future>
#include <nlohmann/json.hpp>

#include "shield/cluster/cluster_manager.hpp"
#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/plugin/plugin_host.hpp"

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
    dispatcher.register_command("root.services", "List all Lua services",
                                [this](auto& s, auto& a) { cmd_services(s, a); });
    dispatcher.register_command("root.service",
                                "Show details for a specific service",
                                [this](auto& s, auto& a) { cmd_service(s, a); });
    dispatcher.register_command("root.plugins",
                                "List all plugin packages and instances",
                                [this](auto& s, auto& a) { cmd_plugins(s, a); });
    dispatcher.register_command("root.plugin",
                                "Show details for a specific plugin",
                                [this](auto& s, auto& a) { cmd_plugin(s, a); });
    dispatcher.register_command("root.config",
                                "Show config (root.config [key])",
                                [this](auto& s, auto& a) { cmd_config(s, a); });
    dispatcher.register_command("root.cluster",
                                "Show cluster node status",
                                [this](auto& s, auto& a) { cmd_cluster(s, a); });
    dispatcher.register_command("root.log.level",
                                "Get or set log level (root.log.level [debug|info|warn|error])",
                                [this](auto& s, auto& a) { cmd_log_level(s, a); });
}

void RootCommands::cmd_help(shield::net::ConsoleSession& session,
                            const std::vector<std::string>& /*args*/) {
    nlohmann::json resp;
    resp["type"] = "result";
    resp["lines"] = nlohmann::json::array();
    if (dispatcher_) {
        for (const auto& [name, help] : dispatcher_->list_commands()) {
            resp["lines"].push_back(name + "  - " + help);
        }
    }
    // Add Lua REPL commands
    resp["lines"].push_back("attach <service>  - Enter interactive Lua REPL for a service");
    resp["lines"].push_back("detach            - Exit Lua REPL back to command mode");
    resp["lines"].push_back("eval <code>       - Execute Lua code in a sandbox");
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
            plugins.push_back({{"id", inst.id},
                               {"package", inst.package},
                               {"state", inst.state},
                               {"required", inst.required}});
        }
        data["plugins"] = plugins;
    }

    // Cluster (thread-safe, may be null)
    {
        auto* cm = shield::cluster::global_cluster_manager();
        if (cm) {
            auto nodes = cm->nodes();
            nlohmann::json cluster;
            cluster["node_id"] = cm->node_id();
            cluster["node_epoch"] = cm->node_epoch();
            cluster["nodes"] = nlohmann::json::array();
            for (const auto& n : nodes) {
                cluster["nodes"].push_back(
                    {{"node_id", n.node_id},
                     {"address", n.address},
                     {"state", shield::cluster::node_state_name(n.state)},
                     {"epoch", n.epoch}});
            }
            data["cluster"] = cluster;
        }
    }

    nlohmann::json resp = {{"type", "result"}, {"data", data}};
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
    if (future.wait_for(std::chrono::seconds(2)) ==
        std::future_status::ready) {
        nlohmann::json resp = {{"type", "result"}, {"data", future.get()}};
        session.send_line(resp.dump());
    } else {
        nlohmann::json resp = {
            {"type", "error"}, {"message", "timeout querying services"}};
        session.send_line(resp.dump());
    }
}

void RootCommands::cmd_service(shield::net::ConsoleSession& session,
                               const std::vector<std::string>& args) {
    if (args.empty()) {
        nlohmann::json resp = {{"type", "error"},
                               {"message", "Usage: root.service <name>"}};
        session.send_line(resp.dump());
        return;
    }
    const auto& name = args[0];
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();
    lua_mgr_.enqueue_forked_task(
        "", [&mgr = lua_mgr_, name, promise]() {
            nlohmann::json data;
            data["name"] = name;
            auto id = mgr.query_service(name);
            data["exists"] = !id.empty();
            if (!id.empty()) {
                data["id"] = id;
            }
            promise->set_value(data);
        });
    if (future.wait_for(std::chrono::seconds(2)) ==
        std::future_status::ready) {
        nlohmann::json resp = {{"type", "result"}, {"data", future.get()}};
        session.send_line(resp.dump());
    } else {
        nlohmann::json resp = {
            {"type", "error"}, {"message", "timeout querying service"}};
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
        data["packages"].push_back({{"id", pkg.id},
                                    {"version", pkg.version},
                                    {"kind", pkg.kind},
                                    {"provides", pkg.provides}});
    }

    auto instances = host.list_instances();
    data["instances"] = nlohmann::json::array();
    for (const auto& inst : instances) {
        data["instances"].push_back({{"id", inst.id},
                                     {"package", inst.package},
                                     {"state", inst.state},
                                     {"required", inst.required}});
    }

    nlohmann::json resp = {{"type", "result"}, {"data", data}};
    session.send_line(resp.dump());
}

void RootCommands::cmd_plugin(shield::net::ConsoleSession& session,
                              const std::vector<std::string>& args) {
    if (args.empty()) {
        nlohmann::json resp = {{"type", "error"},
                               {"message", "Usage: root.plugin <id>"}};
        session.send_line(resp.dump());
        return;
    }
    auto& host = shield::plugin::global_host();
    auto* inst = host.find_instance(args[0]);
    if (!inst) {
        nlohmann::json resp = {
            {"type", "error"},
            {"message", "Plugin instance not found: " + args[0]}};
        session.send_line(resp.dump());
        return;
    }
    // Convert state enum to string
    std::string state_str;
    switch (inst->state) {
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
    nlohmann::json data = {{"id", inst->id},
                           {"package", inst->package ? inst->package->manifest.id : ""},
                           {"state", state_str},
                           {"last_error", inst->last_error},
                           {"dependencies", inst->dep_ids}};
    nlohmann::json resp = {{"type", "result"}, {"data", data}};
    session.send_line(resp.dump());
}

void RootCommands::cmd_config(shield::net::ConsoleSession& session,
                              const std::vector<std::string>& args) {
    auto& cfg = shield::config::global_config();
    if (args.empty()) {
        // Dump entire config as JSON
        nlohmann::json resp = {{"type", "result"},
                               {"data", nlohmann::json::parse(cfg.to_json())}};
        session.send_line(resp.dump());
    } else {
        const auto& key = args[0];
        if (!cfg.has(key)) {
            nlohmann::json resp = {
                {"type", "error"},
                {"message", "Config key not found: " + key}};
            session.send_line(resp.dump());
            return;
        }
        auto* val = cfg.get_value(key);
        if (!val) {
            nlohmann::json resp = {
                {"type", "error"},
                {"message", "Config key has no value: " + key}};
            session.send_line(resp.dump());
            return;
        }
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
        } else if (auto* v = std::get_if<std::vector<std::string>>(val)) {
            data = *v;
        }
        nlohmann::json resp = {{"type", "result"}, {"data", data}};
        session.send_line(resp.dump());
    }
}

void RootCommands::cmd_cluster(shield::net::ConsoleSession& session,
                               const std::vector<std::string>& /*args*/) {
    auto* cm = shield::cluster::global_cluster_manager();
    if (!cm) {
        nlohmann::json resp = {
            {"type", "error"}, {"message", "Cluster not enabled"}};
        session.send_line(resp.dump());
        return;
    }
    auto nodes = cm->nodes();
    nlohmann::json data;
    data["node_id"] = cm->node_id();
    data["node_epoch"] = cm->node_epoch();
    data["nodes"] = nlohmann::json::array();
    for (const auto& n : nodes) {
        data["nodes"].push_back(
            {{"node_id", n.node_id},
             {"address", n.address},
             {"state", shield::cluster::node_state_name(n.state)},
             {"epoch", n.epoch},
             {"last_heartbeat_ms", n.last_heartbeat_ms}});
    }
    nlohmann::json resp = {{"type", "result"}, {"data", data}};
    session.send_line(resp.dump());
}

void RootCommands::cmd_log_level(shield::net::ConsoleSession& session,
                                 const std::vector<std::string>& args) {
    if (args.empty()) {
        // TODO: add get_global_level() to Logger
        nlohmann::json resp = {
            {"type", "result"},
            {"data", "Use: root.log.level [debug|info|warn|error]"}};
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
        nlohmann::json resp = {
            {"type", "error"},
            {"message", "Invalid level. Use: debug, info, warn, error"}};
        session.send_line(resp.dump());
        return;
    }
    shield::log::Logger::set_global_level(level);
    nlohmann::json resp = {
        {"type", "result"},
        {"data", "Log level set to " + level_str}};
    session.send_line(resp.dump());
}

}  // namespace shield::console
