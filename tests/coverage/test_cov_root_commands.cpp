#define BOOST_TEST_MODULE CovRootCommands
#include <algorithm>
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "shield/caf_initializer.hpp"
#include "shield/config/config.hpp"
#include "shield/console/command_dispatcher.hpp"
#include "shield/console/root_commands.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/console_session.hpp"
#include "shield/plugin/plugin_host.hpp"

namespace {

namespace local = boost::asio::local;
namespace fs = std::filesystem;

const char* kEchoScript = "local M = {}\nreturn M\n";

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

// Console session backed by a connected socket pair, so handlers can call
// send_line() and the test reads the JSON response from the peer socket.
class ConsoleHarness {
public:
    boost::asio::io_context io;
    local::stream_protocol::socket client{io};
    std::shared_ptr<shield::net::ConsoleSession> session;
    std::thread io_thread;

    ConsoleHarness() {
        local::stream_protocol::socket server_side(io);
        boost::asio::local::connect_pair(server_side, client);
        session = std::make_shared<shield::net::ConsoleSession>(
            1, std::move(server_side), shield::net::ConsoleSessionCallbacks{});
        session->start();
        io_thread = std::thread([this]() { io.run(); });
    }

    ~ConsoleHarness() {
        boost::system::error_code ec;
        client.close(ec);
        io.stop();
        if (io_thread.joinable()) io_thread.join();
    }

    std::string read_line(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        auto buf = std::make_shared<boost::asio::streambuf>();
        auto prom = std::make_shared<std::promise<std::string>>();
        auto fut = prom->get_future();
        boost::asio::post(io, [this, buf, prom]() {
            boost::asio::async_read_until(
                client, *buf, '\n',
                [buf, prom](boost::system::error_code ec, std::size_t) {
                    if (ec) {
                        prom->set_value(std::string{});
                        return;
                    }
                    std::istream is(buf.get());
                    std::string line;
                    std::getline(is, line);
                    prom->set_value(line);
                });
        });
        if (fut.wait_for(timeout) != std::future_status::ready) {
            boost::system::error_code ec;
            boost::asio::post(io, [this]() {
                boost::system::error_code cancel_ec;
                client.cancel(cancel_ec);
            });
            (void)ec;
            return {};
        }
        return fut.get();
    }
};

// Lua runtime + service manager + one spawned service named "svc".
struct LuaFixture {
    caf::actor_system_config caf_cfg;
    std::unique_ptr<caf::actor_system> system;
    std::unique_ptr<shield::lua::LuaRuntime> runtime;
    std::unique_ptr<shield::lua::LuaServiceManager> manager;
    fs::path script_path;

    LuaFixture() {
        script_path = fs::temp_directory_path() / "shield_cov_root_echo.lua";
        std::ofstream(script_path) << kEchoScript;

        system = std::make_unique<caf::actor_system>(caf_cfg);
        runtime = std::make_unique<shield::lua::LuaRuntime>();
        manager =
            std::make_unique<shield::lua::LuaServiceManager>(*runtime, *system);

        nlohmann::json opts = {{"name", "svc"},
                               {"args", nlohmann::json::object()},
                               {"config", nlohmann::json::object()}};
        auto result = manager->spawn(script_path.string(), opts.dump());
        BOOST_REQUIRE_MESSAGE(result.success, result.error_message);
    }
};

std::string write_file(const fs::path& path, const std::string& content) {
    std::ofstream(path) << content;
    return path.string();
}

}  // namespace

// The ConsoleHarness drives commands over a socketpair (boost.asio local
// stream protocol), which cannot operate on Windows: skip the suite there.
#ifndef _WIN32
BOOST_FIXTURE_TEST_SUITE(RootCommandsTests, LuaFixture)

BOOST_AUTO_TEST_CASE(HelpListsRegisteredCommands) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::RootCommands root(*manager);
    root.register_all(dispatcher);

    dispatcher.dispatch(harness.session, "help");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    std::string joined = resp["lines"].dump();
    BOOST_CHECK(joined.find("root.status") != std::string::npos);
    BOOST_CHECK(joined.find("attach") != std::string::npos);
    BOOST_CHECK(joined.find("detach") != std::string::npos);
    BOOST_CHECK(joined.find("eval") != std::string::npos);
    BOOST_CHECK(joined.find("exit / quit") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(StatusListsServices) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::RootCommands root(*manager);
    root.register_all(dispatcher);

    // The services block dispatches through a ""-id forked task, which
    // borrows any live service actor ("svc" here), so the list resolves.
    auto start = std::chrono::steady_clock::now();
    dispatcher.dispatch(harness.session, "root.status");
    std::string line = harness.read_line(std::chrono::milliseconds(8000));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    BOOST_REQUIRE(!line.empty());
    BOOST_CHECK(elapsed < std::chrono::seconds(2));
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["services"].is_array());
    BOOST_CHECK(resp["data"]["plugins"].is_array());
}

BOOST_AUTO_TEST_CASE(ServicesCommandListsNames) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::RootCommands root(*manager);
    root.register_all(dispatcher);

    dispatcher.dispatch(harness.session, "root.services");
    std::string line = harness.read_line(std::chrono::milliseconds(8000));
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"].is_array());
    bool found = false;
    for (const auto& name : resp["data"]) {
        if (name == "svc") {
            found = true;
        }
    }
    BOOST_CHECK(found);
}

BOOST_AUTO_TEST_CASE(ServiceCommandVariants) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::RootCommands root(*manager);
    root.register_all(dispatcher);

    // No argument -> usage error.
    dispatcher.dispatch(harness.session, "root.service");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("Usage") !=
                std::string::npos);

    // Existing service name resolves through the manager registry.
    dispatcher.dispatch(harness.session, "root.service svc");
    line = harness.read_line(std::chrono::milliseconds(8000));
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["name"] == "svc");
    BOOST_CHECK(resp["data"]["exists"] == true);

    // Unknown service -> result reporting exists == false.
    dispatcher.dispatch(harness.session, "root.service missing_svc");
    line = harness.read_line(std::chrono::milliseconds(8000));
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["exists"] == false);
}

BOOST_AUTO_TEST_CASE(PluginsAndPluginAcrossLifecycleStates) {
    fs::path root_dir = fs::temp_directory_path() / "shield_cov_root_plugins";
    fs::remove_all(root_dir);
    fs::create_directories(root_dir / "good.pkg" / "bin");
    fs::create_directories(root_dir / "broken.pkg" / "bin");

    const char* good_manifest =
        "schema_version: 1\n"
        "id: minimal.test\n"
        "name: Good\n"
        "version: 1.0.0\n"
        "kind: test\n"
        "entry: shield_plugin_get_v1\n"
        "library:\n"
        "  linux: bin/libshield_minimal_test_plugin.so\n"
        "  macos: bin/libshield_minimal_test_plugin.dylib\n"
        "  windows: bin/libshield_minimal_test_plugin.dll\n"
        "provides:\n"
        "  - interface: minimal.test.iface\n"
        "requires: []\n"
        "config_schema:\n"
        "  type: object\n";
    write_file(root_dir / "good.pkg" / "manifest.yaml", good_manifest);
    fs::copy_file(
#if defined(_WIN32)
        "test_plugins/minimal.test/bin/"
        "libshield_minimal_test_plugin.dll",
#elif defined(__APPLE__)
        "test_plugins/minimal.test/bin/"
        "libshield_minimal_test_plugin.dylib",
#else
        "test_plugins/minimal.test/bin/"
        "libshield_minimal_test_plugin.so",
#endif
        root_dir / "good.pkg" / "bin" /
#if defined(_WIN32)
            "libshield_minimal_test_plugin.dll",
#elif defined(__APPLE__)
            "libshield_minimal_test_plugin.dylib",
#else
            "libshield_minimal_test_plugin.so",
#endif
        fs::copy_options::overwrite_existing);

    const char* broken_manifest =
        "schema_version: 1\n"
        "id: broken.pkg\n"
        "name: Broken\n"
        "version: 1.0.0\n"
        "kind: test\n"
        "entry: shield_plugin_get_v1\n"
        "library:\n"
        "  linux: bin/missing_library.so\n"
        "  macos: bin/missing_library.dylib\n"
        "  windows: bin/missing_library.dll\n"
        "provides:\n"
        "  - interface: broken.iface\n"
        "requires: []\n"
        "config_schema:\n"
        "  type: object\n";
    write_file(root_dir / "broken.pkg" / "manifest.yaml", broken_manifest);

    auto& host = shield::plugin::global_host();
    std::string err;
    host.scan(root_dir.string());
    BOOST_REQUIRE_MESSAGE(host.catalog(err), err);

    shield::plugin::PluginConfig pc;
    pc.directory = root_dir.string();
    shield::plugin::InstanceDecl good;
    good.id = "inst_good";
    good.package = "minimal.test";
    good.required = true;
    shield::plugin::InstanceDecl opt;
    opt.id = "inst_opt";
    opt.package = "absent.pkg";
    opt.required = false;
    shield::plugin::InstanceDecl broken;
    broken.id = "inst_broken";
    broken.package = "broken.pkg";
    broken.required = true;
    pc.instances.push_back(good);
    pc.instances.push_back(opt);
    pc.instances.push_back(broken);
    BOOST_REQUIRE_MESSAGE(host.plan_and_resolve(pc, err), err);

    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::RootCommands root(*manager);
    root.register_all(dispatcher);

    auto query_state = [&](const std::string& cmd) -> std::string {
        dispatcher.dispatch(harness.session, cmd);
        std::string line = harness.read_line();
        BOOST_REQUIRE(!line.empty());
        return nlohmann::json::parse(line)["data"]["state"].get<std::string>();
    };

    // Usage error and unknown instance error.
    dispatcher.dispatch(harness.session, "root.plugin");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("Usage") !=
                std::string::npos);

    dispatcher.dispatch(harness.session, "root.plugin no_such_instance");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("not found") !=
                std::string::npos);

    // planned state.
    BOOST_CHECK_EQUAL(query_state("root.plugin inst_good"), "planned");

    // load: good succeeds, broken (required, missing library) fails.
    BOOST_CHECK(!host.load_all(err));
    {
        const auto* good_inst = host.find_instance("inst_good");
        const auto* broken_inst = host.find_instance("inst_broken");
        std::ostringstream diag;
        diag << "load_all err=" << err;
        if (good_inst)
            diag << " inst_good state=" << static_cast<int>(good_inst->state)
                 << " last_error=" << good_inst->last_error;
        if (broken_inst)
            diag << " inst_broken state="
                 << static_cast<int>(broken_inst->state);
        BOOST_CHECK_MESSAGE(query_state("root.plugin inst_good") == "loaded",
                            diag.str());
        BOOST_CHECK_EQUAL(query_state("root.plugin inst_broken"), "failed");
    }
    // unavailable instance has no package pointer.
    dispatcher.dispatch(harness.session, "root.plugin inst_opt");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["data"]["state"] == "unavailable");
    BOOST_CHECK(resp["data"]["package"] == "");

    // started state.
    BOOST_REQUIRE_MESSAGE(host.create_all(err), err);
    BOOST_REQUIRE_MESSAGE(host.start_all(err), err);
    BOOST_CHECK_EQUAL(query_state("root.plugin inst_good"), "started");

    // root.plugins lists packages and instances.
    dispatcher.dispatch(harness.session, "root.plugins");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["packages"].size() == 2);
    BOOST_CHECK(resp["data"]["instances"].size() == 3);

    // With instances present, root.status serializes each plugin instance.
    dispatcher.dispatch(harness.session, "root.status");
    line = harness.read_line(std::chrono::milliseconds(8000));
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["plugins"].size() == 3);
    BOOST_CHECK(resp["data"]["plugins"][0]["id"].is_string());

    // stopped state after shutdown.
    host.shutdown();
    BOOST_CHECK_EQUAL(query_state("root.plugin inst_good"), "stopped");

    fs::remove_all(root_dir);
}

BOOST_AUTO_TEST_CASE(ConfigCommandVariants) {
    auto& cfg = shield::config::global_config();
    cfg.set("cov.str", std::string("hello"));
    cfg.set("cov.int", static_cast<int64_t>(42));
    cfg.set("cov.dbl", 2.5);
    cfg.set("cov.flag", true);
    cfg.set("cov.arr", std::vector<std::string>{"a", "b"});

    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::RootCommands root(*manager);
    root.register_all(dispatcher);

    auto dispatch_json = [&](const std::string& cmd) -> nlohmann::json {
        dispatcher.dispatch(harness.session, cmd);
        std::string line = harness.read_line();
        BOOST_REQUIRE(!line.empty());
        return nlohmann::json::parse(line);
    };

    // Full dump.
    auto resp = dispatch_json("root.config");
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"].is_object());

    // Typed values.
    BOOST_CHECK(dispatch_json("root.config cov.str")["data"] == "hello");
    BOOST_CHECK(dispatch_json("root.config cov.int")["data"] == 42);
    BOOST_CHECK(dispatch_json("root.config cov.dbl")["data"] == 2.5);
    BOOST_CHECK(dispatch_json("root.config cov.flag")["data"] == true);
    BOOST_CHECK(dispatch_json("root.config cov.arr")["data"] ==
                nlohmann::json::array({"a", "b"}));

    // Missing key.
    resp = dispatch_json("root.config cov.missing");
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("not found") !=
                std::string::npos);
}

BOOST_AUTO_TEST_CASE(LogLevelCommandVariants) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::RootCommands root(*manager);
    root.register_all(dispatcher);

    auto dispatch_json = [&](const std::string& cmd) -> nlohmann::json {
        dispatcher.dispatch(harness.session, cmd);
        std::string line = harness.read_line();
        BOOST_REQUIRE(!line.empty());
        return nlohmann::json::parse(line);
    };

    // No argument returns the current level.
    auto resp = dispatch_json("root.log.level");
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"] == "info");

    // Valid levels.
    for (const char* level : {"debug", "info", "warn", "warning", "error"}) {
        resp = dispatch_json(std::string("root.log.level ") + level);
        BOOST_CHECK(resp["type"] == "result");
    }

    // The level actually changed and the getter reflects it.
    resp = dispatch_json("root.log.level");
    BOOST_CHECK(resp["data"] == "error");

    // Invalid level.
    resp = dispatch_json("root.log.level noisy");
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("Invalid level") !=
                std::string::npos);

    shield::log::Logger::set_global_level(shield::log::Level::Info);
}

BOOST_AUTO_TEST_CASE(ClusterCommandAvailability) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::RootCommands root(*manager);
    root.register_all(dispatcher);

    dispatcher.dispatch(harness.session, "root.cluster");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
#ifdef SHIELD_ENABLE_CLUSTER
    // Compiled but this fixture configures no cluster manager.
    BOOST_CHECK(resp["message"] == "Cluster not enabled");
#else
    BOOST_CHECK(resp["message"] == "Cluster not compiled");
#endif
}

// ---------------------------------------------------------------------------
// Branch-coverage additions (keep purely additive; no existing case above is
// modified).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(TimeoutPathsAfterServicesAreGone) {
    manager->shutdown_all("done");
    // The registry removal is asynchronous; wait until "svc" is gone.
    for (int i = 0; i < 400; ++i) {
        if (manager->query_service("svc").empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    BOOST_CHECK(manager->query_service("svc").empty());

    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::RootCommands root(*manager);
    root.register_all(dispatcher);

    // root.status: the ""-id task has no live actor to borrow, so the
    // services future times out ("timeout" marker) while plugins resolve.
    dispatcher.dispatch(harness.session, "root.status");
    std::string line = harness.read_line(std::chrono::milliseconds(8000));
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["services"] == "timeout");
    BOOST_CHECK(resp["data"]["plugins"].is_array());

    // root.services: same timeout, reported as an error.
    dispatcher.dispatch(harness.session, "root.services");
    line = harness.read_line(std::chrono::milliseconds(8000));
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("timeout") !=
                std::string::npos);

    // root.service with a dead name: enqueue is rejected (task id 0), the
    // future never fires and the command reports the timeout.
    dispatcher.dispatch(harness.session, "root.service svc");
    line = harness.read_line(std::chrono::milliseconds(8000));
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("timeout") !=
                std::string::npos);
}

BOOST_AUTO_TEST_CASE(LogLevelGetterAcrossAllEnumValues) {
    using shield::log::Level;
    using shield::log::Logger;

    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::RootCommands root(*manager);
    root.register_all(dispatcher);

    auto query_level = [&]() -> std::string {
        dispatcher.dispatch(harness.session, "root.log.level");
        std::string line = harness.read_line();
        BOOST_REQUIRE(!line.empty());
        auto resp = nlohmann::json::parse(line);
        BOOST_CHECK(resp["type"] == "result");
        return resp["data"].get<std::string>();
    };

    // Programmatic levels that the setter command cannot reach.
    Logger::set_global_level(Level::Fatal);
    BOOST_CHECK_EQUAL(query_level(), "fatal");
    Logger::set_global_level(Level::Debug);
    BOOST_CHECK_EQUAL(query_level(), "debug");
    Logger::set_global_level(Level::Warning);
    BOOST_CHECK_EQUAL(query_level(), "warn");

    // Restore the shared default for the remaining cases.
    Logger::set_global_level(Level::Info);
}

BOOST_AUTO_TEST_CASE(ConfigCommandValueShapes) {
    auto& cfg = shield::config::global_config();
    cfg.set("cov2.esc",
            std::string("quote\" back\\slash new\nline tab\t uni\xc3\x97"
                        " ctrl\x01"));
    cfg.set("cov2.long", std::string(140, 'x'));
    cfg.set("cov2.negint", static_cast<int64_t>(-1234567890123LL));
    cfg.set("cov2.zero", static_cast<int64_t>(0));
    cfg.set("cov2.big", static_cast<int64_t>(9223372036854775807LL));
    cfg.set("cov2.dbl", -0.001953125);
    cfg.set("cov2.dblwhole", 3.0);
    cfg.set("cov2.t", true);
    cfg.set("cov2.f", false);
    cfg.set("cov2.emptystr", std::string(""));
    cfg.set("cov2.arr0", std::vector<std::string>{});
    cfg.set("cov2.arrN", std::vector<std::string>{"a\"b", std::string(90, 'y'),
                                                  "\xe2\x9c\x93"});

    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::RootCommands root(*manager);
    root.register_all(dispatcher);

    auto dispatch_data = [&](const std::string& key) -> nlohmann::json {
        dispatcher.dispatch(harness.session, "root.config " + key);
        std::string line = harness.read_line();
        BOOST_REQUIRE(!line.empty());
        auto resp = nlohmann::json::parse(line);
        BOOST_CHECK(resp["type"] == "result");
        return resp["data"];
    };

    BOOST_CHECK_EQUAL(dispatch_data("cov2.esc")
                          .get<std::string>()
                          .find("quote\" back\\slash"),
                      0u);
    BOOST_CHECK_EQUAL(dispatch_data("cov2.long").get<std::string>().size(),
                      140u);
    BOOST_CHECK_EQUAL(dispatch_data("cov2.negint"), -1234567890123LL);
    BOOST_CHECK_EQUAL(dispatch_data("cov2.zero"), 0);
    BOOST_CHECK_EQUAL(dispatch_data("cov2.big"), 9223372036854775807LL);
    BOOST_CHECK_EQUAL(dispatch_data("cov2.dbl"), -0.001953125);
    BOOST_CHECK_EQUAL(dispatch_data("cov2.dblwhole"), 3.0);
    BOOST_CHECK_EQUAL(dispatch_data("cov2.t"), true);
    BOOST_CHECK_EQUAL(dispatch_data("cov2.f"), false);
    BOOST_CHECK_EQUAL(dispatch_data("cov2.emptystr"), "");
    BOOST_CHECK(dispatch_data("cov2.arr0") == nlohmann::json::array());
    BOOST_CHECK(
        dispatch_data("cov2.arrN") ==
        nlohmann::json::array({"a\"b", std::string(90, 'y'), "\xe2\x9c\x93"}));

    // Full dump with all shapes mixed in still parses.
    dispatcher.dispatch(harness.session, "root.config");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"].is_object());
}

BOOST_AUTO_TEST_CASE(ServiceCommandWithFancyName) {
    // A (missing) service name containing JSON-escapable characters still
    // flows through the root.service response dump.
    const std::string fancy = "svc\"quo\\te_ok";
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::RootCommands root(*manager);
    root.register_all(dispatcher);

    dispatcher.dispatch(harness.session, "root.service " + fancy);
    std::string line = harness.read_line(std::chrono::milliseconds(8000));
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["name"] == fancy);
    BOOST_CHECK(resp["data"]["exists"] == false);

    // root.status with a live service still serializes the name list.
    dispatcher.dispatch(harness.session, "root.status");
    line = harness.read_line(std::chrono::milliseconds(8000));
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["services"].is_array());
}

BOOST_AUTO_TEST_CASE(DispatcherBranchEdges) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;

    // Empty and whitespace-only lines return without writing anything.
    dispatcher.dispatch(harness.session, "");
    dispatcher.dispatch(harness.session, "   \t\r   ");
    BOOST_CHECK_EQUAL(harness.read_line(std::chrono::milliseconds(200)), "");

    // Unknown command reports an error with the trimmed command name.
    dispatcher.dispatch(harness.session, "  padded\top   ");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("padded") !=
                std::string::npos);

    // Escapable characters flow through the error JSON dump.
    dispatcher.dispatch(harness.session, "fancy\"cmd\"\xe2\x9c\x93");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");

    // Attached session with no registered Lua line handler: the line is
    // swallowed (handler absent half) and must not crash.
    harness.session->set_attached_service("svc");
    dispatcher.dispatch(harness.session, "anything at all");
    BOOST_CHECK_EQUAL(harness.read_line(std::chrono::milliseconds(200)), "");

    // "exit" and "detach" both leave attached mode.
    dispatcher.dispatch(harness.session, "exit");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    BOOST_CHECK(nlohmann::json::parse(line)["type"] == "detached");
    BOOST_CHECK(!harness.session->is_attached());

    harness.session->set_attached_service("svc");
    dispatcher.dispatch(harness.session, "  detach  ");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    BOOST_CHECK(nlohmann::json::parse(line)["type"] == "detached");

    // Registered command receives trimmed argument tokens.
    std::vector<std::string> captured_args;
    dispatcher.register_command("multi", "arg test",
                                [&](shield::net::ConsoleSession& s,
                                    const std::vector<std::string>& args) {
                                    captured_args = args;
                                    nlohmann::json r = {{"type", "result"},
                                                        {"data", "ran"}};
                                    s.send_line(r.dump());
                                });
    dispatcher.dispatch(harness.session, "  multi   a1   a2  ");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    BOOST_CHECK(nlohmann::json::parse(line)["data"] == "ran");
    BOOST_REQUIRE_EQUAL(captured_args.size(), 2u);
    BOOST_CHECK_EQUAL(captured_args[0], "a1");
    BOOST_CHECK_EQUAL(captured_args[1], "a2");

    const auto listed = dispatcher.list_commands();
    bool has_multi = false;
    for (const auto& [name, help] : listed) {
        if (name == "multi") has_multi = true;
    }
    BOOST_CHECK(has_multi);
}

// Runs last among the new cases: re-plans the global plugin host with no
// instances so the instance loops in root.plugins / root.status observe an
// empty collection. (Scanned packages accumulate on the process-wide host
// and cannot be cleared, so only the instance lists are asserted.)
BOOST_AUTO_TEST_CASE(PluginCommandsWithEmptyHostLists) {
    fs::path empty_dir = fs::temp_directory_path() / "shield_cov_root_empty";
    fs::remove_all(empty_dir);
    fs::create_directories(empty_dir);

    auto& host = shield::plugin::global_host();
    std::string err;
    host.scan(empty_dir.string());
    BOOST_REQUIRE_MESSAGE(host.catalog(err), err);
    shield::plugin::PluginConfig pc;
    pc.directory = empty_dir.string();
    BOOST_REQUIRE_MESSAGE(host.plan_and_resolve(pc, err), err);

    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::RootCommands root(*manager);
    root.register_all(dispatcher);

    dispatcher.dispatch(harness.session, "root.plugins");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["packages"].is_array());
    BOOST_CHECK(resp["data"]["instances"].is_array());
    BOOST_CHECK_EQUAL(resp["data"]["instances"].size(), 0u);

    dispatcher.dispatch(harness.session, "root.status");
    line = harness.read_line(std::chrono::milliseconds(8000));
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["plugins"].is_array());
    BOOST_CHECK_EQUAL(resp["data"]["plugins"].size(), 0u);

    fs::remove_all(empty_dir);
}

BOOST_AUTO_TEST_SUITE_END()
#endif  // !_WIN32
