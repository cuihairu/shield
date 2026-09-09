#define BOOST_TEST_MODULE CovLuaCommands
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "shield/caf_initializer.hpp"
#include "shield/console/command_dispatcher.hpp"
#include "shield/console/lua_commands.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/console_session.hpp"

namespace {

namespace local = boost::asio::local;
namespace fs = std::filesystem;

const char* kEchoScript = "local M = {}\nreturn M\n";

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

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
            7, std::move(server_side), shield::net::ConsoleSessionCallbacks{});
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
        std::chrono::milliseconds timeout = std::chrono::milliseconds(6000)) {
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
            boost::asio::post(io, [this]() {
                boost::system::error_code cancel_ec;
                client.cancel(cancel_ec);
            });
            return {};
        }
        return fut.get();
    }
};

struct LuaFixture {
    caf::actor_system_config caf_cfg;
    std::unique_ptr<caf::actor_system> system;
    std::unique_ptr<shield::lua::LuaRuntime> runtime;
    std::unique_ptr<shield::lua::LuaServiceManager> manager;
    fs::path script_path;

    LuaFixture() {
        script_path = fs::temp_directory_path() / "shield_cov_lua_echo.lua";
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

}  // namespace

BOOST_FIXTURE_TEST_SUITE(LuaCommandsTests, LuaFixture)

BOOST_AUTO_TEST_CASE(AttachUsageAndErrors) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    // No argument -> usage.
    dispatcher.dispatch(harness.session, "attach");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("Usage") !=
                std::string::npos);

    // Unknown service -> not found.
    dispatcher.dispatch(harness.session, "attach ghost_service");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("not found") !=
                std::string::npos);

    // Existing service -> attached.
    dispatcher.dispatch(harness.session, "attach svc");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "attached");
    BOOST_CHECK(resp["service"] == "svc");
    BOOST_CHECK(harness.session->is_attached());

    // Detach returns to command mode.
    dispatcher.dispatch(harness.session, "detach");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "detached");
    BOOST_CHECK(!harness.session->is_attached());
}

BOOST_AUTO_TEST_CASE(EvalCommandVariants) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    auto dispatch_json = [&](const std::string& cmd) -> nlohmann::json {
        dispatcher.dispatch(harness.session, cmd);
        std::string line = harness.read_line();
        BOOST_REQUIRE(!line.empty());
        return nlohmann::json::parse(line);
    };

    // No code -> usage error.
    auto resp = dispatch_json("eval");
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("Usage") !=
                std::string::npos);

    // No return values.
    resp = dispatch_json("eval local x = 1");
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"].is_null());

    // Single return value.
    resp = dispatch_json("eval return 42");
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"] == 42);

    // Multiple return values.
    resp = dispatch_json("eval return 1, 2, 3");
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"] == nlohmann::json::array({1, 2, 3}));

    // Runtime error.
    resp = dispatch_json("eval error('boom')");
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("boom") !=
                std::string::npos);

    // Syntax error.
    resp = dispatch_json("eval return )");
    BOOST_CHECK(resp["type"] == "error");
}

BOOST_AUTO_TEST_CASE(ReplSingleLineVariants) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    dispatcher.dispatch(harness.session, "attach svc");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());

    auto dispatch_json = [&](const std::string& cmd) -> nlohmann::json {
        dispatcher.dispatch(harness.session, cmd);
        std::string l = harness.read_line();
        BOOST_REQUIRE(!l.empty());
        return nlohmann::json::parse(l);
    };

    // Single return value.
    auto resp = dispatch_json("return 42");
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"] == 42);

    // Multiple return values.
    resp = dispatch_json("return 'a', 'b'");
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"] == nlohmann::json::array({"a", "b"}));

    // No return values.
    resp = dispatch_json("x = 1");
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"].is_null());

    // Runtime error is reported as an error response.
    resp = dispatch_json("error('repl failure')");
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("repl failure") !=
                std::string::npos);

    // Real syntax error (not an eof continuation).
    resp = dispatch_json("return )");
    BOOST_CHECK(resp["type"] == "error");
}

BOOST_AUTO_TEST_CASE(ReplMultilineAccumulation) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    dispatcher.dispatch(harness.session, "attach svc");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());

    auto dispatch_json = [&](const std::string& cmd) -> nlohmann::json {
        dispatcher.dispatch(harness.session, cmd);
        std::string l = harness.read_line();
        BOOST_REQUIRE(!l.empty());
        return nlohmann::json::parse(l);
    };

    // Incomplete single line starts multiline mode.
    auto resp = dispatch_json("if true then");
    BOOST_CHECK(resp["type"] == "continue");
    BOOST_CHECK(!harness.session->multiline_buffer().empty());

    // Still incomplete -> continue again.
    resp = dispatch_json("x = 1");
    BOOST_CHECK(resp["type"] == "continue");

    // Completing the statement executes it and clears the buffer.
    resp = dispatch_json("end");
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(harness.session->multiline_buffer().empty());

    // A multiline block that returns a value.
    resp = dispatch_json("for i = 1, 2 do");
    BOOST_CHECK(resp["type"] == "continue");
    resp = dispatch_json("end");
    BOOST_CHECK(resp["type"] == "result");
}

BOOST_AUTO_TEST_CASE(ReplOnVanishedService) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    // Simulate a session attached to a service whose actor is gone: the
    // forked exec task is rejected immediately.
    harness.session->set_attached_service("vanished_service");
    dispatcher.dispatch(harness.session, "return 1");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find(
                    "service unavailable: vanished_service") !=
                std::string::npos);
}

// ---------------------------------------------------------------------------
// Branch-coverage additions (purely additive).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ReplValueShapes) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    dispatcher.dispatch(harness.session, "attach svc");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());

    auto dispatch_data = [&](const std::string& code) -> nlohmann::json {
        dispatcher.dispatch(harness.session, code);
        std::string l = harness.read_line();
        BOOST_REQUIRE(!l.empty());
        auto r = nlohmann::json::parse(l);
        BOOST_CHECK(r["type"] == "result");
        return r["data"];
    };

    // Numeric shapes: fractional, negative, huge (exponent formatting).
    BOOST_CHECK_EQUAL(dispatch_data("return 3.14"), 3.14);
    BOOST_CHECK_EQUAL(dispatch_data("return -7"), -7);
    BOOST_CHECK(dispatch_data("return 1e300").get<double>() > 1e299);

    // Booleans and nil.
    BOOST_CHECK_EQUAL(dispatch_data("return true"), true);
    BOOST_CHECK_EQUAL(dispatch_data("return false"), false);
    BOOST_CHECK(dispatch_data("return nil").is_null());

    // Strings: escapable characters, unicode, and past the SSO boundary.
    // (Lua source needs a doubled backslash for one literal backslash.)
    BOOST_CHECK_EQUAL(dispatch_data("return 'quoted \"str\" back\\\\'"),
                      "quoted \"str\" back\\");
    BOOST_CHECK_EQUAL(dispatch_data("return 'tab\\there'"), "tab\there");
    BOOST_CHECK_EQUAL(dispatch_data("return '\xc3\xa9\xe2\x9c\x93'"),
                      "\xc3\xa9\xe2\x9c\x93");
    const std::string long_str =
        dispatch_data("return string.rep('z', 80)").get<std::string>();
    BOOST_CHECK_EQUAL(long_str.size(), 80u);

    // Nested table returns serialize to JSON (shape depends on the keys).
    auto table = dispatch_data("return {1, 'a', true}");
    BOOST_CHECK(table.is_string() || table.is_array() || table.is_object());
    auto named = dispatch_data("return {x = 1}");
    BOOST_CHECK(named.is_string() || named.is_object());
}

BOOST_AUTO_TEST_CASE(ReplMultilineSyntaxErrorClearsBuffer) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    dispatcher.dispatch(harness.session, "attach svc");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());

    // Start a multiline block...
    dispatcher.dispatch(harness.session, "if true then");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    BOOST_CHECK(nlohmann::json::parse(line)["type"] == "continue");
    BOOST_CHECK(!harness.session->multiline_buffer().empty());

    // ...then make the accumulated buffer a hard syntax error: reported and
    // the multiline state cleared.
    dispatcher.dispatch(harness.session, "))) garbage ((( ");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(harness.session->multiline_buffer().empty());

    // The session is usable again.
    dispatcher.dispatch(harness.session, "return 1");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    BOOST_CHECK(nlohmann::json::parse(line)["data"] == 1);
}

BOOST_AUTO_TEST_CASE(EvalValueShapes) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    auto dispatch_data = [&](const std::string& code) -> nlohmann::json {
        dispatcher.dispatch(harness.session, "eval " + code);
        std::string l = harness.read_line();
        BOOST_REQUIRE(!l.empty());
        auto r = nlohmann::json::parse(l);
        BOOST_CHECK(r["type"] == "result");
        return r["data"];
    };

    BOOST_CHECK_EQUAL(dispatch_data("return 2.5"), 2.5);
    BOOST_CHECK_EQUAL(dispatch_data("return -900"), -900);
    BOOST_CHECK_EQUAL(dispatch_data("return false"), false);
    BOOST_CHECK(dispatch_data("return nil").is_null());
    BOOST_CHECK_EQUAL(dispatch_data("return 'a\"b'"), "a\"b");
    BOOST_CHECK_EQUAL(dispatch_data("return 'nl\\nline'"), "nl\nline");
    BOOST_CHECK_EQUAL(
        dispatch_data("return '\xc3\xa9\xe2\x9c\x93'").get<std::string>(),
        "\xc3\xa9\xe2\x9c\x93");
    BOOST_CHECK_EQUAL(
        dispatch_data("return string.rep('q', 90)").get<std::string>().size(),
        90u);
    auto table = dispatch_data("return {x = 1}");
    BOOST_CHECK(table.is_string() || table.is_object());
}

// An explicit empty `return` yields an empty result array, which the eval
// and REPL handlers report as {"type":"result","data":null}.
BOOST_FIXTURE_TEST_CASE(EmptyReturnShapes, LuaFixture) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    // eval path: empty array -> data null.
    dispatcher.dispatch(harness.session, "eval return");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"].is_null());

    // REPL path (attach first): empty array -> data null.
    dispatcher.dispatch(harness.session, "attach svc");
    BOOST_REQUIRE(!harness.read_line().empty());
    dispatcher.dispatch(harness.session, "return");
    line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"].is_null());
}

// ---------------------------------------------------------------------------
// Round-3: attached REPL execution exceeding the 5s console deadline.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ReplExecutionTimeout) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    dispatcher.dispatch(harness.session, "attach svc");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());

    // Busy-loop for ~7s: the REPL's 5s wait_for lapses first and reports an
    // execution timeout to the console.
    dispatcher.dispatch(harness.session,
                        "local t = os.clock() while os.clock() - t < 7 do end");
    line = harness.read_line(std::chrono::milliseconds(15000));
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("execution timeout") !=
                std::string::npos);

    // Give the busy loop a moment to finish so teardown does not race the
    // still-running exec task on the service actor.
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
}

BOOST_AUTO_TEST_SUITE_END()
