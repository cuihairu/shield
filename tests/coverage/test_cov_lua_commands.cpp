#define BOOST_TEST_MODULE CovLuaCommands
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdint>
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

// ---------------------------------------------------------------------------
// L2 restricted inspect: lua.inspect / lua.snapshot / lua.diff.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LuaInspectCommandVariants) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    // Missing service / field and unknown field all surface the usage.
    dispatcher.dispatch(harness.session, "lua.inspect");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("Usage") !=
                std::string::npos);
    dispatcher.dispatch(harness.session, "lua.inspect svc");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    dispatcher.dispatch(harness.session, "lua.inspect svc bogus_field");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("Usage") !=
                std::string::npos);

    // Unknown service.
    dispatcher.dispatch(harness.session, "lua.inspect ghost summary");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("not found") !=
                std::string::npos);

    // summary carries the full detail snapshot (same fields as
    // /ops/services/:name).
    dispatcher.dispatch(harness.session, "lua.inspect svc summary");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["name"], "svc");
    BOOST_CHECK(resp["data"]["requests"].is_number_unsigned());
    BOOST_CHECK(resp["data"]["memory_kb"].is_number_unsigned());
    BOOST_CHECK(resp["data"]["coroutines"].is_number_unsigned());

    // memory is dispatch-shaped now (owner-thread GC sample + retainers
    // head, see InspectMemoryGcAndRetainers); the focused projection keeps
    // only its headline fields.
    dispatcher.dispatch(harness.session, "lua.inspect svc memory");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["name"], "svc");
    BOOST_CHECK(resp["data"]["gc"]["memory_kb"].is_number_unsigned());
    BOOST_CHECK(resp["data"]["retainers"].is_object());
    // coroutines is dispatch-shaped now (owner-thread enumeration, see
    // InspectCoroutinesDetail); the idle echo service reports none.
    dispatcher.dispatch(harness.session, "lua.inspect svc coroutines");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["total"], 0u);
    dispatcher.dispatch(harness.session, "lua.inspect svc timers");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["timers"], 0u);
    dispatcher.dispatch(harness.session, "lua.inspect svc pending_calls");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["pending_calls"], 0u);
}

BOOST_AUTO_TEST_CASE(LuaSnapshotAndDiffCommands) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    // Usage shapes.
    dispatcher.dispatch(harness.session, "lua.snapshot");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("Usage") !=
                std::string::npos);
    dispatcher.dispatch(harness.session, "lua.diff svc only_one");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("Usage") !=
                std::string::npos);

    // Unknown service for both.
    dispatcher.dispatch(harness.session, "lua.snapshot ghost");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("not found") !=
                std::string::npos);
    dispatcher.dispatch(harness.session, "lua.diff ghost a b");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("not found") !=
                std::string::npos);

    // Auto-named capture.
    dispatcher.dispatch(harness.session, "lua.snapshot svc");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["name"].get<std::string>().find("snap-") == 0u);
    BOOST_CHECK(resp["data"]["wall_ms"].is_number());
    // Named capture.
    dispatcher.dispatch(harness.session, "lua.snapshot svc base");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["name"], "base");

    // Unknown snapshot names error with the missing name.
    dispatcher.dispatch(harness.session, "lua.diff svc base missing");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("missing") !=
                std::string::npos);

    // Same-name diff: the ring holds one entry, so a == b and every delta
    // is zero (legitimate, not an error).
    dispatcher.dispatch(harness.session, "lua.snapshot svc base");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    dispatcher.dispatch(harness.session, "lua.diff svc base base");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["a"]["name"], "base");
    BOOST_CHECK_EQUAL(resp["data"]["delta"]["requests"], 0);
    BOOST_CHECK_EQUAL(resp["data"]["delta"]["memory_kb"], 0);
    BOOST_CHECK(resp["data"]["delta"]["uptime_seconds"].get<double>() >= 0.0);

    // The third argument opts the capture into the owner-thread refs walk.
    // Same-name refresh replaces the stored entry, so the plain capture
    // above is overwritten by a refs-sampled one.
    dispatcher.dispatch(harness.session, "lua.snapshot svc base refs");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["refs"].is_object());
    BOOST_CHECK(resp["data"]["refs"]["counts"]["tables"] >= 1);
    // A fourth argument (anything but exactly "refs") surfaces usage.
    dispatcher.dispatch(harness.session, "lua.snapshot svc x extra");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("Usage") !=
                std::string::npos);
    // Diff between refs-sampled ends carries the refs delta object.
    dispatcher.dispatch(harness.session, "lua.snapshot svc base2 refs");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    dispatcher.dispatch(harness.session, "lua.diff svc base base2");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK(resp["data"]["delta"]["refs"].is_object());
    BOOST_CHECK_EQUAL(resp["data"]["delta"]["refs"]["counts"]["tables"], 0);
}

BOOST_AUTO_TEST_CASE(InspectSnapshotRingAndTeardown) {
    std::string err;
    // Manager-level guard: an unpublished name refuses to capture.
    auto bad =
        manager->capture_inspect_snapshot("ghost_svc", "g1", false, &err);
    BOOST_CHECK(!bad.has_value());
    BOOST_CHECK(err.find("not published") != std::string::npos);

    // Ring bound: 9 auto captures keep only the last 8, so the oldest
    // (snap-A below) is gone by the end.
    const std::string first =
        (*manager->capture_inspect_snapshot("svc", "", false, &err))["name"];
    for (int i = 0; i < 8; ++i) {
        BOOST_REQUIRE(manager->capture_inspect_snapshot("svc", "", false, &err)
                          .has_value());
    }
    auto gone = manager->diff_inspect_snapshots("svc", first, first, &err);
    BOOST_CHECK(!gone.has_value());
    BOOST_CHECK(err.find("unknown snapshot") != std::string::npos);

    // Teardown drops the service's snapshots: spawn a second incarnation,
    // capture, exit, and observe the "no snapshots" verdict by direct call
    // (the published name no longer resolves through query_service).
    const fs::path extra = fs::temp_directory_path() / "shield_cov_lua_l2.lua";
    std::ofstream(extra) << "local M = {}\nreturn M\n";
    auto spawned = manager->spawn(extra.string(),
                                  R"({"name":"svc_l2","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    bool published = false;
    for (int i = 0; i < 200 && !published; ++i) {
        published = !manager->query_service("svc_l2").empty();
        if (!published) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(published);
    BOOST_REQUIRE(manager->capture_inspect_snapshot("svc_l2", "s1", false, &err)
                      .has_value());
    manager->exit("svc_l2");
    bool exited = false;
    for (int i = 0; i < 200 && !exited; ++i) {
        exited = manager->query_service("svc_l2").empty();
        if (!exited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(exited);
    auto dropped = manager->diff_inspect_snapshots("svc_l2", "s1", "s1", &err);
    BOOST_CHECK(!dropped.has_value());
    BOOST_CHECK(err.find("no snapshots") != std::string::npos);
}

// A capture taken while the service has live state: an armed timer, a
// handler coroutine suspended in a cross-service call, and the matching
// pending-call entry. This is the L2 snapshot's raison d'être — freezing
// gauges mid-flight, not only idle zeros.
BOOST_AUTO_TEST_CASE(InspectSnapshotCapturesLiveState) {
    const fs::path slow = fs::temp_directory_path() / "shield_cov_l2_slow.lua";
    std::ofstream(slow) << "local M = {}\n"
                           "function M.slow(ctx) shield.sleep(600) return 'ok' "
                           "end\n"
                           "return M\n";
    const fs::path busy = fs::temp_directory_path() / "shield_cov_l2_busy.lua";
    std::ofstream(busy) << "local M = {}\n"
                           "function M.on_init(args)\n"
                           "  shield.timer_once(700, function() end)\n"
                           "end\n"
                           "function M.nap(ctx)\n"
                           "  return shield.call('cov_l2_slow', 'slow')\n"
                           "end\n"
                           "return M\n";

    auto slow_svc = manager->spawn(
        slow.string(), R"({"name":"cov_l2_slow","args":{},"config":{}})");
    BOOST_REQUIRE(slow_svc.success);
    auto busy_svc = manager->spawn(
        busy.string(), R"({"name":"cov_l2_busy","args":{},"config":{}})");
    BOOST_REQUIRE(busy_svc.success);

    // Fire-and-forget: the nap handler suspends inside the cross-service
    // call, keeping its coroutine, the pending-call entry and the init-time
    // timer alive simultaneously.
    BOOST_REQUIRE(
        manager->send(busy_svc.service_id, "nap", nlohmann::json::array()));

    // Poll the registry with direct C++ reads (an HTTP poll loop here would
    // starve the fork lane that drives the call continuation).
    bool live = false;
    for (int i = 0; i < 400 && !live; ++i) {
        auto d = manager->service_detail(busy_svc.service_id);
        live = d.has_value() && (*d)["coroutines"].get<std::uint64_t>() >= 1 &&
               (*d)["pending_calls"].get<std::uint64_t>() >= 1 &&
               (*d)["timers"].get<std::uint64_t>() >= 1;
        if (!live) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    BOOST_REQUIRE(live);

    std::string err;
    const auto snap = manager->capture_inspect_snapshot(busy_svc.service_id,
                                                        "live", false, &err);
    BOOST_REQUIRE(snap.has_value());
    BOOST_CHECK_GE((*snap)["timers"].get<std::uint64_t>(), 1u);
    BOOST_CHECK_GE((*snap)["coroutines"].get<std::uint64_t>(), 1u);
    BOOST_CHECK_GE((*snap)["pending_calls"].get<std::uint64_t>(), 1u);

    // Let the nap continuation and the 700ms timer finish so teardown does
    // not race them.
    for (int i = 0; i < 900; ++i) {
        auto d = manager->service_detail(busy_svc.service_id);
        if (d.has_value() && (*d)["coroutines"].get<std::uint64_t>() == 0 &&
            (*d)["timers"].get<std::uint64_t>() == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// ---------------------------------------------------------------------------
// L2 refs walker: lua.inspect <svc> refs / LuaServiceManager::inspect_refs.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(InspectRefsWalkerSummary) {
    // The module packs one value of every walker category: nested tables,
    // a function, a coroutine (LUA_TTHREAD), a ServiceHandle (LUA_TUSERDATA,
    // shield.self), strings, a non-string table key ("[boolean]" segment),
    // over-long keys (the 48-char segment cap and the 96-char path cut)
    // and a self-reference cycle. Twenty leaf tables push the table count
    // past the 16-entry report limit.
    const fs::path rich = fs::temp_directory_path() / "shield_cov_l2_refs.lua";
    std::ofstream(rich) << "local M = {}\n"
                           "M.label = \"widget\"\n"
                           "M[42] = { x = 1, y = 2 }\n"
                           "M[true] = {}\n"
                           "M.fn = function() end\n"
                           "M.co = coroutine.create(function() end)\n"
                           "M[string.rep('a', 60)] = "
                           "{ [string.rep('b', 60)] = {} }\n"
                           "M.nested = { deep = { deeper = {} } }\n"
                           "M.self = M\n"
                           "for i = 1, 20 do M['leaf' .. i] = {} end\n"
                           "function M.on_init()\n"
                           "  M.handle = shield.self()\n"
                           "end\n"
                           "return M\n";
    auto spawned = manager->spawn(
        rich.string(), R"({"name":"svc_refs","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    bool published = false;
    for (int i = 0; i < 200 && !published; ++i) {
        published = !manager->query_service("svc_refs").empty();
        if (!published) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(published);

    std::string err;
    // Unpublished names refuse before any fork dispatch.
    auto ghost = manager->inspect_refs("ghost_refs", 4, 1000, &err);
    BOOST_CHECK(!ghost.has_value());
    BOOST_CHECK(err.find("not published") != std::string::npos);

    // Bounded walk: counts, top tables, cycle handling (M.self points back
    // at the already-visited module table) and the depth cut at max_depth 2
    // (M.nested.deep.deeper stays unvisited, without marking truncated).
    auto refs = manager->inspect_refs("svc_refs", 2, 20000, &err);
    BOOST_REQUIRE_MESSAGE(refs.has_value(), err);
    BOOST_CHECK_EQUAL((*refs)["name"], "svc_refs");
    BOOST_CHECK_EQUAL((*refs)["depth_limit"], 2);
    BOOST_CHECK((*refs)["truncated"] == false);
    // M + [42] + [true]'s value + the two long-key tables + nested + deep
    // + 20 leaves (deeper stays below the depth cut).
    BOOST_CHECK_EQUAL((*refs)["counts"]["tables"], 27u);
    // M.fn plus the on_init hook.
    BOOST_CHECK_EQUAL((*refs)["counts"]["functions"], 2u);
    BOOST_CHECK_EQUAL((*refs)["counts"]["coroutines"], 1u);
    BOOST_CHECK_EQUAL((*refs)["counts"]["userdata"], 1u);
    BOOST_CHECK_EQUAL((*refs)["counts"]["strings"], 1u);
    BOOST_CHECK_EQUAL((*refs)["counts"]["string_bytes"], 6u);
    const auto& top = (*refs)["top_tables"];
    BOOST_REQUIRE(top.is_array());
    // 27 visited tables, but the report keeps at most 16.
    BOOST_REQUIRE_EQUAL(top.size(), 16u);
    BOOST_CHECK_EQUAL(top[0]["path"], "M");
    BOOST_CHECK_EQUAL(top[0]["entries"], 30u);
    // The 60+60-char key chain exceeds the 96-char path budget exactly at
    // the innermost table: its reported path is cut with the "~" tail.
    bool path_cut_seen = false;
    for (const auto& t : top) {
        const std::string p = t["path"].get<std::string>();
        if (!p.empty() && p.back() == '~') {
            BOOST_CHECK_EQUAL(p.size(), 97u);
            path_cut_seen = true;
        }
    }
    BOOST_CHECK(path_cut_seen);
    // nodes_visited: 30 root + 2 in M.[42] + 1 in the long-key outer
    // table + 1 M.nested + 1 M.nested.deep.
    BOOST_CHECK_EQUAL((*refs)["nodes_visited"], 35u);

    // Node budget exhaustion marks the walk truncated.
    auto tight = manager->inspect_refs("svc_refs", 4, 3, &err);
    BOOST_REQUIRE(tight.has_value());
    BOOST_CHECK((*tight)["truncated"] == true);
    BOOST_CHECK_EQUAL((*tight)["nodes_visited"], 3u);

    // Out-of-range arguments land on the documented bounds.
    auto clamped = manager->inspect_refs("svc_refs", 100, 100000, &err);
    BOOST_REQUIRE(clamped.has_value());
    BOOST_CHECK_EQUAL((*clamped)["depth_limit"], 8);
    BOOST_CHECK_EQUAL((*clamped)["node_budget"], 50000u);
}

BOOST_AUTO_TEST_CASE(InspectRefsConsoleCommand) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    // Depth outside [1,8] surfaces the usage line, not a walker error.
    for (const char* bad : {"lua.inspect svc refs 0", "lua.inspect svc refs 9",
                            "lua.inspect svc refs NaN"}) {
        dispatcher.dispatch(harness.session, bad);
        std::string line = harness.read_line();
        auto resp = nlohmann::json::parse(line);
        BOOST_CHECK(resp["type"] == "error");
        BOOST_CHECK(resp["message"].get<std::string>().find("Usage") !=
                    std::string::npos);
    }

    // Unknown service is rejected by the console fast path (registry read),
    // same as every other inspect subcommand.
    dispatcher.dispatch(harness.session, "lua.inspect ghost refs");
    std::string line = harness.read_line();
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("not found") !=
                std::string::npos);

    // Happy path, default depth.
    dispatcher.dispatch(harness.session, "lua.inspect svc refs");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["name"], "svc");
    BOOST_CHECK_GE(resp["data"]["counts"]["tables"].get<std::uint64_t>(), 1u);
    BOOST_CHECK(resp["data"]["top_tables"].is_array());

    // Explicit depth is echoed through depth_limit.
    dispatcher.dispatch(harness.session, "lua.inspect svc refs 3");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["depth_limit"], 3);
}

// L2 memory: lua.inspect <svc> memory / LuaServiceManager::inspect_memory —
// one owner-thread fork task carrying the GC sample and the retainers head.
BOOST_AUTO_TEST_CASE(InspectMemoryGcAndRetainers) {
    const fs::path mem = fs::temp_directory_path() / "shield_cov_l2_mem.lua";
    std::ofstream(mem) << "local M = {}\n"
                          "M.blob = {}\n"
                          "for i = 1, 50 do M['k' .. i] = { i } end\n"
                          "return M\n";
    auto spawned = manager->spawn(
        mem.string(), R"({"name":"svc_mem","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    bool published = false;
    for (int i = 0; i < 200 && !published; ++i) {
        published = !manager->query_service("svc_mem").empty();
        if (!published) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(published);

    std::string err;
    // Unpublished names refuse before any fork dispatch.
    auto ghost = manager->inspect_memory("ghost_mem", &err);
    BOOST_CHECK(!ghost.has_value());
    BOOST_CHECK(err.find("not published") != std::string::npos);

    auto report = manager->inspect_memory("svc_mem", &err);
    BOOST_REQUIRE_MESSAGE(report.has_value(), err);
    BOOST_CHECK_EQUAL((*report)["name"], "svc_mem");
    const std::int64_t kb = (*report)["gc"]["memory_kb"].get<std::int64_t>();
    BOOST_CHECK_GT(kb, 0);
    // total_bytes is the same sample refined with GCCOUNTB.
    BOOST_CHECK_EQUAL((*report)["gc"]["total_bytes"].get<std::int64_t>() / 1024,
                      kb);
    // The default collector runs incremental; every parameter knob is
    // exposed with a non-negative value.
    BOOST_CHECK_EQUAL((*report)["gc"]["mode"], "incremental");
    BOOST_CHECK((*report)["gc"]["running"].is_boolean());
    for (const char* p : {"minormul", "majorminor", "minormajor", "pause",
                          "stepmul", "stepsize"}) {
        BOOST_CHECK_MESSAGE((*report)["gc"]["params"].contains(p),
                            "missing gc param: " << p);
        BOOST_CHECK_GE((*report)["gc"]["params"][p].get<int>(), 0);
    }
    // Retainers head: the walk starts at the module table, which holds
    // blob plus k1..k50.
    BOOST_CHECK((*report)["retainers"]["truncated"] == false);
    BOOST_CHECK_GT((*report)["retainers"]["nodes_visited"].get<std::uint64_t>(),
                   0u);
    const auto& top = (*report)["retainers"]["top_tables"];
    BOOST_REQUIRE(top.is_array());
    BOOST_REQUIRE_GE(top.size(), 1u);
    BOOST_CHECK_EQUAL(top[0]["path"], "M");
    BOOST_CHECK_GE(top[0]["entries"].get<std::uint64_t>(), 51u);

    // Console frontend: the ghost fast path is unchanged (registry read),
    // the happy path now carries the dispatch-shaped report.
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    dispatcher.dispatch(harness.session, "lua.inspect ghost memory");
    std::string line = harness.read_line();
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("not found") !=
                std::string::npos);

    dispatcher.dispatch(harness.session, "lua.inspect svc_mem memory");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["name"], "svc_mem");
    BOOST_CHECK_GT(resp["data"]["gc"]["memory_kb"].get<std::int64_t>(), 0);
    BOOST_CHECK(resp["data"]["gc"]["params"].is_object());
    BOOST_CHECK(resp["data"]["retainers"]["top_tables"].is_array());
}

// The memory sample is a fork task on the owning service actor, same as
// the refs walk: a service wedged in a synchronous handler cannot pick it
// up, and the 2s bounded wait lapses instead of hanging the console.
BOOST_AUTO_TEST_CASE(InspectMemoryDispatchTimeout) {
    const fs::path churn =
        fs::temp_directory_path() / "shield_cov_l2_churn_mem.lua";
    std::ofstream(churn) << "local M = {}\n"
                            "function M.churn(ctx)\n"
                            "  local t0 = os.clock()\n"
                            "  while os.clock() - t0 < 7 do end\n"
                            "  return 'done'\n"
                            "end\n"
                            "return M\n";
    auto spawned = manager->spawn(
        churn.string(), R"({"name":"svc_churn_mem","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    bool published = false;
    for (int i = 0; i < 200 && !published; ++i) {
        published = !manager->query_service("svc_churn_mem").empty();
        if (!published) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(published);

    // One busy run covers both frontends: the console command surfaces the
    // timeout as an error line, then the manager-level call asserts it.
    BOOST_REQUIRE(
        manager->send("svc_churn_mem", "churn", nlohmann::json::array()));

    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);
    dispatcher.dispatch(harness.session, "lua.inspect svc_churn_mem memory");
    std::string line = harness.read_line(std::chrono::milliseconds(15000));
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find(
                    "memory dispatch timeout") != std::string::npos);

    std::string err;
    const auto mem = manager->inspect_memory("svc_churn_mem", &err);
    BOOST_CHECK(!mem.has_value());
    BOOST_CHECK(err.find("memory dispatch timeout") != std::string::npos);

    // Let the busy handler finish so teardown does not race it.
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
}

// L2 coroutines: lua.inspect <svc> coroutines / inspect_coroutines — the
// per-coroutine lua_status read on the owner thread plus the resume
// bookkeeping (origin / last resume source / resume count).
BOOST_AUTO_TEST_CASE(InspectCoroutinesDetail) {
    // park: a bare coroutine.yield() suspends the handler coroutine and no
    // C++ resume source ever picks it up — exactly the stuck-coroutine
    // shape this subcommand exists to expose. slow_echo: a responder whose
    // 300ms sleep widens the window in which the two_calls caller sits on
    // its SECOND shield.call, i.e. after one call-response resume.
    const fs::path co_mod = fs::temp_directory_path() / "shield_cov_l2_co.lua";
    std::ofstream(co_mod) << "local M = {}\n"
                             "function M.park(ctx)\n"
                             "  coroutine.yield()\n"
                             "  return 'unreachable'\n"
                             "end\n"
                             "function M.two_calls(ctx)\n"
                             "  shield.call('co_rsp', 'slow_echo', {'a'})\n"
                             "  shield.call('co_rsp', 'slow_echo', {'b'})\n"
                             "  return 'done'\n"
                             "end\n"
                             "return M\n";
    const fs::path rsp_mod =
        fs::temp_directory_path() / "shield_cov_l2_co_rsp.lua";
    std::ofstream(rsp_mod) << "local M = {}\n"
                              "function M.slow_echo(ctx, v)\n"
                              "  shield.sleep(300)\n"
                              "  return v\n"
                              "end\n"
                              "return M\n";
    auto spawned = manager->spawn(co_mod.string(),
                                  R"({"name":"svc_co","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    auto spawned_rsp = manager->spawn(
        rsp_mod.string(), R"({"name":"co_rsp","args":{},"config":{}})");
    BOOST_REQUIRE(spawned_rsp.success);
    bool published = false;
    bool published_rsp = false;
    for (int i = 0; i < 200 && (!published || !published_rsp); ++i) {
        published = !manager->query_service("svc_co").empty();
        published_rsp = !manager->query_service("co_rsp").empty();
        if (!published || !published_rsp) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(published);
    BOOST_REQUIRE(published_rsp);

    std::string err;
    // Unpublished names refuse before any fork dispatch.
    auto ghost = manager->inspect_coroutines("ghost_co", &err);
    BOOST_CHECK(!ghost.has_value());
    BOOST_CHECK(err.find("not published") != std::string::npos);

    // Idle service: no live coroutines.
    auto idle = manager->inspect_coroutines("svc_co", &err);
    BOOST_REQUIRE_MESSAGE(idle.has_value(), err);
    BOOST_CHECK_EQUAL((*idle)["total"], 0u);
    BOOST_CHECK((*idle)["truncated"] == false);
    BOOST_CHECK((*idle)["entries"].is_array());

    // Park one handler coroutine (fire-and-forget; poll for the async
    // dispatch to land).
    BOOST_REQUIRE(manager->send("svc_co", "park", nlohmann::json::array()));
    std::optional<nlohmann::json> parked;
    for (int i = 0; i < 200; ++i) {
        auto snap = manager->inspect_coroutines("svc_co", &err);
        if (snap.has_value() && (*snap)["total"] >= 1u) {
            parked = std::move(snap);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_REQUIRE_MESSAGE(parked.has_value(), "parked coroutine never showed");
    BOOST_CHECK_EQUAL((*parked)["total"], 1u);
    BOOST_CHECK_EQUAL((*parked)["by_status"]["suspended"], 1u);
    const auto& e0 = (*parked)["entries"][0];
    BOOST_CHECK_EQUAL(e0["status"], "suspended");
    BOOST_CHECK_EQUAL(e0["origin"], "handler");
    BOOST_CHECK_EQUAL(e0["resumes"], 1u);
    BOOST_CHECK_EQUAL(e0["last_resume"], "dispatch");
    BOOST_CHECK(e0["driving"] == false);
    BOOST_CHECK(e0["waiting_call"].is_null());
    BOOST_CHECK_GE(e0["age_ms"].get<std::int64_t>(), 0);

    // Call-response resume bookkeeping: two_calls suspends twice. While it
    // sits on the second shield.call the responder's 300ms sleep holds the
    // window open — the caller's last resume source is "call-response",
    // the resume count ticks to 2, and waiting_call points at the
    // still-pending session. All three must hold on one entry: the poll
    // tolerates a snapshot that caught the caller right between its resume
    // and its next suspend bookkeeping.
    BOOST_REQUIRE(
        manager->send("svc_co", "two_calls", nlohmann::json::array()));
    nlohmann::json caller_entry;
    bool caller_seen = false;
    for (int i = 0; i < 300 && !caller_seen; ++i) {
        auto snap = manager->inspect_coroutines("svc_co", &err);
        if (snap.has_value()) {
            for (const auto& e : (*snap)["entries"]) {
                if (e["origin"] == "handler" &&
                    e["last_resume"] == "call-response" &&
                    e["status"] == "suspended" && e["resumes"] == 2u &&
                    !e["waiting_call"].is_null()) {
                    caller_entry = e;
                    caller_seen = true;
                    break;
                }
            }
        }
        if (!caller_seen) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_CHECK_MESSAGE(caller_seen, "call-response resume never observed");
    // The parked coroutine from the first segment is still there: first
    // drive only, bare yield, no call session.
    if (caller_seen) {
        auto snap = manager->inspect_coroutines("svc_co", &err);
        BOOST_REQUIRE(snap.has_value());
        bool parked_seen = false;
        for (const auto& e : (*snap)["entries"]) {
            if (e["last_resume"] == "dispatch" && e["resumes"] == 1u &&
                e["waiting_call"].is_null()) {
                parked_seen = true;
                BOOST_CHECK_EQUAL(e["status"], "suspended");
                BOOST_CHECK_EQUAL(e["origin"], "handler");
            }
        }
        BOOST_CHECK(parked_seen);
    }
    // The responder side: co_rsp enumerates its own slow_echo coroutine,
    // suspended inside shield.sleep while serving the caller's request —
    // first drive only, waiting_call holds the callee-side session.
    bool responder_seen = false;
    for (int i = 0; i < 300 && !responder_seen; ++i) {
        auto rsp_snap = manager->inspect_coroutines("co_rsp", &err);
        if (rsp_snap.has_value()) {
            for (const auto& e : (*rsp_snap)["entries"]) {
                if (e["last_resume"] == "dispatch" && e["resumes"] == 1u &&
                    !e["waiting_call"].is_null()) {
                    responder_seen = true;
                    BOOST_CHECK_EQUAL(e["status"], "suspended");
                    BOOST_CHECK_EQUAL(e["origin"], "handler");
                    BOOST_CHECK_EQUAL(e["last_resume"], "dispatch");
                }
            }
        }
        if (!responder_seen) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_CHECK(responder_seen);

    // two_calls and both responders complete and drain; the parked
    // coroutine from the first segment stays (no resume source exists for
    // a bare yield) — exactly one live coroutine remains.
    bool drained = false;
    for (int i = 0; i < 300 && !drained; ++i) {
        auto snap = manager->inspect_coroutines("svc_co", &err);
        drained = snap.has_value() && (*snap)["total"] == 1u &&
                  (*snap)["entries"][0]["resumes"] == 1u &&
                  (*snap)["entries"][0]["last_resume"] == "dispatch";
        if (!drained) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_CHECK_MESSAGE(drained, "two_calls never drained");

    // Console frontend: the ghost fast path is unchanged, the happy path
    // carries the dispatch-shaped detail.
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    dispatcher.dispatch(harness.session, "lua.inspect ghost coroutines");
    std::string line = harness.read_line();
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("not found") !=
                std::string::npos);

    dispatcher.dispatch(harness.session, "lua.inspect svc_co coroutines");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["name"], "svc_co");
    BOOST_CHECK(resp["data"]["by_status"].is_object());
    BOOST_CHECK(resp["data"]["entries"].is_array());
}

// The coroutine enumeration is a fork task on the owning service actor,
// same as memory/refs: a service wedged in a synchronous handler cannot
// pick it up, and the 2s bounded wait lapses with the coroutines-specific
// message. Self-contained spawn: this case must also run standalone.
BOOST_AUTO_TEST_CASE(InspectCoroutinesDispatchTimeout) {
    const fs::path churn =
        fs::temp_directory_path() / "shield_cov_l2_churn_co.lua";
    std::ofstream(churn) << "local M = {}\n"
                            "function M.churn(ctx)\n"
                            "  local t0 = os.clock()\n"
                            "  while os.clock() - t0 < 7 do end\n"
                            "  return 'done'\n"
                            "end\n"
                            "return M\n";
    auto spawned = manager->spawn(
        churn.string(), R"({"name":"svc_churn_co","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    bool published = false;
    for (int i = 0; i < 200 && !published; ++i) {
        published = !manager->query_service("svc_churn_co").empty();
        if (!published) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(published);

    // One busy run covers both frontends: console error line first, then
    // the manager-level call asserts the message.
    BOOST_REQUIRE(
        manager->send("svc_churn_co", "churn", nlohmann::json::array()));

    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);
    dispatcher.dispatch(harness.session, "lua.inspect svc_churn_co coroutines");
    std::string line = harness.read_line(std::chrono::milliseconds(15000));
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find(
                    "coroutines dispatch timeout") != std::string::npos);

    std::string err;
    const auto cos = manager->inspect_coroutines("svc_churn_co", &err);
    BOOST_CHECK(!cos.has_value());
    BOOST_CHECK(err.find("coroutines dispatch timeout") != std::string::npos);

    // Let the busy handler finish so teardown does not race it.
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
}

// 40 parked coroutines overflow the 32-entry report cap: total counts all
// of them, the report keeps the 32 most recently resumed entries and flags
// truncated.
BOOST_AUTO_TEST_CASE(InspectCoroutinesTruncation) {
    const fs::path co_mod =
        fs::temp_directory_path() / "shield_cov_l2_co_tr.lua";
    std::ofstream(co_mod) << "local M = {}\n"
                             "function M.park(ctx)\n"
                             "  coroutine.yield()\n"
                             "  return 'unreachable'\n"
                             "end\n"
                             "return M\n";
    auto spawned = manager->spawn(
        co_mod.string(), R"({"name":"svc_co_tr","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    bool published = false;
    for (int i = 0; i < 200 && !published; ++i) {
        published = !manager->query_service("svc_co_tr").empty();
        if (!published) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(published);

    for (int i = 0; i < 40; ++i) {
        BOOST_REQUIRE(
            manager->send("svc_co_tr", "park", nlohmann::json::array()));
    }
    std::string err;
    std::optional<nlohmann::json> snap;
    std::uint64_t last_total = 0;
    for (int i = 0; i < 300; ++i) {
        auto s = manager->inspect_coroutines("svc_co_tr", &err);
        if (s.has_value()) {
            last_total = (*s)["total"].get<std::uint64_t>();
            if (last_total >= 40u) {
                snap = std::move(s);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_TEST_MESSAGE("last observed total = " << last_total);
    BOOST_REQUIRE_MESSAGE(snap.has_value(),
                          "40 parked coroutines never accumulated");
    BOOST_CHECK_EQUAL((*snap)["total"], 40u);
    BOOST_CHECK((*snap)["truncated"] == true);
    BOOST_CHECK_EQUAL((*snap)["entries"].size(), 32u);
    // by_status is tallied before the cap, so it still counts all 40.
    BOOST_CHECK_EQUAL((*snap)["by_status"]["suspended"], 40u);
    // Kept entries are the most recently resumed ones: age_ms ascending.
    std::int64_t prev_age = -1;
    for (const auto& e : (*snap)["entries"]) {
        const std::int64_t age = e["age_ms"].get<std::int64_t>();
        BOOST_CHECK_GE(age, prev_age);
        prev_age = age;
    }
}

// The refs walk is a fork task on the owning service actor: a service
// wedged in a synchronous (non-yielding) handler cannot pick it up, and the
// console-side bounded wait lapses after 2s instead of hanging.
BOOST_AUTO_TEST_CASE(InspectRefsDispatchTimeout) {
    const fs::path churn =
        fs::temp_directory_path() / "shield_cov_l2_churn.lua";
    std::ofstream(churn) << "local M = {}\n"
                            "function M.churn(ctx)\n"
                            "  local t0 = os.clock()\n"
                            "  while os.clock() - t0 < 7 do end\n"
                            "  return 'done'\n"
                            "end\n"
                            "return M\n";
    auto spawned = manager->spawn(
        churn.string(), R"({"name":"svc_churn","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    bool published = false;
    for (int i = 0; i < 200 && !published; ++i) {
        published = !manager->query_service("svc_churn").empty();
        if (!published) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(published);

    // Occupy the service actor with a 7s synchronous handler; the 2s refs
    // dispatch wait must lapse long before the mailbox drains. One busy
    // run covers both frontends: the console command surfaces the timeout
    // as an error line, then the manager-level call asserts the message.
    BOOST_REQUIRE(manager->send("svc_churn", "churn", nlohmann::json::array()));

    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);
    dispatcher.dispatch(harness.session, "lua.inspect svc_churn refs");
    std::string line = harness.read_line(std::chrono::milliseconds(15000));
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find(
                    "refs dispatch timeout") != std::string::npos);

    std::string err;
    const auto refs = manager->inspect_refs("svc_churn", 4, 20000, &err);
    BOOST_CHECK(!refs.has_value());
    BOOST_CHECK(err.find("refs dispatch timeout") != std::string::npos);

    // Let the busy handler finish so teardown does not race it.
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
}

// ---------------------------------------------------------------------------
// Snapshot object-graph extension: with_refs captures, refs delta in diff,
// and the busy-owner refs_error record.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(InspectSnapshotWithRefsAndDiff) {
    // The module grows and shrinks a subtable between captures so the refs
    // delta has real movement: M.box gains three leaf tables, then goes
    // away (driven over fire-and-forget sends, polled through captures).
    const fs::path mut = fs::temp_directory_path() / "shield_cov_l2_mut.lua";
    std::ofstream(mut) << "local M = {}\n"
                          "function M.grow(ctx)\n"
                          "  M.box = {}\n"
                          "  for i = 1, 3 do M.box['leaf' .. i] = {} end\n"
                          "  return 'grown'\n"
                          "end\n"
                          "function M.shrink(ctx)\n"
                          "  M.box = nil\n"
                          "  return 'shrunk'\n"
                          "end\n"
                          "return M\n";
    auto spawned = manager->spawn(
        mut.string(), R"({"name":"svc_mut","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    bool published = false;
    for (int i = 0; i < 200 && !published; ++i) {
        published = !manager->query_service("svc_mut").empty();
        if (!published) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(published);

    std::string err;
    // A plain capture keeps refs null and records no error.
    const auto plain =
        manager->capture_inspect_snapshot("svc_mut", "p0", false, &err);
    BOOST_REQUIRE(plain.has_value());
    BOOST_CHECK((*plain)["refs"].is_null());
    BOOST_CHECK(!plain->contains("refs_error"));

    // Base capture with refs: M alone (the module has no on_init).
    const auto base =
        manager->capture_inspect_snapshot("svc_mut", "rb", true, &err);
    BOOST_REQUIRE_MESSAGE(base.has_value(), err);
    BOOST_REQUIRE((*base)["refs"].is_object());
    const std::int64_t base_tables =
        (*base)["refs"]["counts"]["tables"].get<std::int64_t>();
    BOOST_CHECK_GE(base_tables, 1);
    BOOST_CHECK((*base)["refs"]["counts"]["tables"] >= 1);
    BOOST_CHECK(!base->contains("refs_error"));
    BOOST_CHECK((*base)["refs"]["top_tables"].is_array());

    // Grow the module on the owner thread, then poll captures (same-name
    // overwrite keeps the ring bounded) until the growth shows up.
    BOOST_REQUIRE(manager->send("svc_mut", "grow", nlohmann::json::array()));
    std::optional<nlohmann::json> grown;
    for (int i = 0; i < 200 && !grown.has_value(); ++i) {
        auto attempt =
            manager->capture_inspect_snapshot("svc_mut", "rg", true, &err);
        BOOST_REQUIRE_MESSAGE(attempt.has_value(), err);
        if ((*attempt)["refs"]["counts"]["tables"].get<std::int64_t>() >=
            base_tables + 4) {
            grown = std::move(attempt);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(grown.has_value());
    // M.box plus its three leaves on top of the baseline count.
    BOOST_CHECK_EQUAL((*grown)["refs"]["counts"]["tables"].get<std::int64_t>(),
                      base_tables + 4);

    // Diff base vs grown: tables delta +4, M.box reported added.
    auto d1 = manager->diff_inspect_snapshots("svc_mut", "rb", "rg", &err);
    BOOST_REQUIRE_MESSAGE(d1.has_value(), err);
    BOOST_CHECK((*d1)["delta"]["refs"].is_object());
    BOOST_CHECK_EQUAL(
        (*d1)["delta"]["refs"]["counts"]["tables"].get<std::int64_t>(), 4);
    const auto& tops = (*d1)["delta"]["refs"]["top_tables"];
    BOOST_REQUIRE(tops.is_array());
    bool box_added = false;
    for (const auto& t : tops) {
        if (t["path"] == "M.box") {
            BOOST_CHECK_EQUAL(t["change"], "added");
            BOOST_CHECK_EQUAL(t["entries"], 3u);
            box_added = true;
        }
    }
    BOOST_CHECK(box_added);

    // Shrink, capture, diff again: M.box removed, tables back to baseline.
    BOOST_REQUIRE(manager->send("svc_mut", "shrink", nlohmann::json::array()));
    std::optional<nlohmann::json> shrunk;
    for (int i = 0; i < 200 && !shrunk.has_value(); ++i) {
        auto attempt =
            manager->capture_inspect_snapshot("svc_mut", "rs", true, &err);
        BOOST_REQUIRE_MESSAGE(attempt.has_value(), err);
        if ((*attempt)["refs"]["counts"]["tables"].get<std::int64_t>() ==
            base_tables) {
            shrunk = std::move(attempt);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(shrunk.has_value());
    auto d2 = manager->diff_inspect_snapshots("svc_mut", "rg", "rs", &err);
    BOOST_REQUIRE_MESSAGE(d2.has_value(), err);
    BOOST_CHECK_EQUAL(
        (*d2)["delta"]["refs"]["counts"]["tables"].get<std::int64_t>(), -4);
    const auto& tops2 = (*d2)["delta"]["refs"]["top_tables"];
    bool box_removed = false;
    for (const auto& t : tops2) {
        if (t["path"] == "M.box") {
            BOOST_CHECK_EQUAL(t["change"], "removed");
            box_removed = true;
        }
    }
    BOOST_CHECK(box_removed);

    // Mixed ends (plain vs refs-sampled) refuse to invent a baseline:
    // delta.refs is null.
    auto d3 = manager->diff_inspect_snapshots("svc_mut", "p0", "rb", &err);
    BOOST_REQUIRE_MESSAGE(d3.has_value(), err);
    BOOST_CHECK((*d3)["delta"]["refs"].is_null());

    manager->exit("svc_mut");
    bool exited = false;
    for (int i = 0; i < 200 && !exited; ++i) {
        exited = manager->query_service("svc_mut").empty();
        if (!exited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(exited);
}

// A capture with refs against a service wedged in a synchronous handler
// still freezes its L1 gauges; the graph sample lapses after the same 2s
// bounded wait and is recorded as refs_error on the stored snapshot.
BOOST_AUTO_TEST_CASE(InspectSnapshotRefsBusyOwnerRecordsError) {
    const fs::path churn2 =
        fs::temp_directory_path() / "shield_cov_l2_churn2.lua";
    std::ofstream(churn2) << "local M = {}\n"
                             "function M.churn(ctx)\n"
                             "  local t0 = os.clock()\n"
                             "  while os.clock() - t0 < 6 do end\n"
                             "  return 'done'\n"
                             "end\n"
                             "return M\n";
    auto spawned = manager->spawn(
        churn2.string(), R"({"name":"svc_churn2","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);
    bool published = false;
    for (int i = 0; i < 200 && !published; ++i) {
        published = !manager->query_service("svc_churn2").empty();
        if (!published) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_REQUIRE(published);

    BOOST_REQUIRE(
        manager->send("svc_churn2", "churn", nlohmann::json::array()));

    std::string err;
    const auto snap =
        manager->capture_inspect_snapshot("svc_churn2", "busy", true, &err);
    BOOST_REQUIRE_MESSAGE(snap.has_value(), err);
    // Gauges still captured; the graph sample is not.
    BOOST_CHECK((*snap)["refs"].is_null());
    BOOST_CHECK((*snap)["refs_error"].is_string());
    BOOST_CHECK((*snap)["refs_error"].get<std::string>().find(
                    "refs dispatch timeout") != std::string::npos);
    // And the record survives into the stored JSON (read back via diff).
    const auto d =
        manager->diff_inspect_snapshots("svc_churn2", "busy", "busy", &err);
    BOOST_REQUIRE_MESSAGE(d.has_value(), err);
    BOOST_CHECK((*d)["a"]["refs"].is_null());
    BOOST_CHECK((*d)["a"]["refs_error"].get<std::string>().find(
                    "refs dispatch timeout") != std::string::npos);

    // Let the busy handler finish so teardown does not race it.
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
}

// ---------------------------------------------------------------------------
// L2 timers / pending_calls detail projections (lua.inspect <svc>
// timers|pending_calls): registry-locked reads of the per-item bookkeeping.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(InspectTimersAndCallsDetail) {
    const fs::path slow = fs::temp_directory_path() / "shield_cov_l2_slow.lua";
    std::ofstream(slow) << "local M = {}\n"
                           "function M.slow(ctx) shield.sleep(600) return 'ok' "
                           "end\n"
                           "return M\n";
    // The busy service holds, at once: a repeating timer (fires several
    // times while the test polls, driving the next-fire advance), a
    // one-shot timer, a call suspended against the slow service (the
    // pending call) and a shield.sleep timer driven by the native-callback
    // registration path.
    const fs::path busy =
        fs::temp_directory_path() / "shield_cov_l2_detail_busy.lua";
    std::ofstream(busy) << "local M = {}\n"
                           "function M.on_init(args)\n"
                           "  shield.timer_once(700, function() end)\n"
                           "  shield.timer(250, function() end)\n"
                           "end\n"
                           "function M.nap(ctx)\n"
                           "  return shield.call('cov_l2_slow', 'slow')\n"
                           "end\n"
                           "function M.snooze(ctx)\n"
                           "  shield.sleep(500)\n"
                           "  return 'woke'\n"
                           "end\n"
                           "return M\n";

    auto slow_svc = manager->spawn(
        slow.string(), R"({"name":"cov_l2_slow","args":{},"config":{}})");
    BOOST_REQUIRE(slow_svc.success);
    auto busy_svc = manager->spawn(
        busy.string(), R"({"name":"cov_l2_busy","args":{},"config":{}})");
    BOOST_REQUIRE(busy_svc.success);

    // Fire-and-forget: nap suspends inside the cross-service call, snooze
    // inside shield.sleep — both keep their pending wait bookkeeping.
    BOOST_REQUIRE(
        manager->send(busy_svc.service_id, "nap", nlohmann::json::array()));
    BOOST_REQUIRE(
        manager->send(busy_svc.service_id, "snooze", nlohmann::json::array()));

    bool live = false;
    for (int i = 0; i < 400 && !live; ++i) {
        auto d = manager->service_detail(busy_svc.service_id);
        live = d.has_value() && (*d)["timers"].get<std::size_t>() >= 2 &&
               (*d)["pending_calls"].get<std::uint64_t>() >= 1;
        if (!live) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    BOOST_REQUIRE(live);

    std::string err;
    auto ghost_timers = manager->timer_inspect("ghost_detail", &err);
    BOOST_CHECK(!ghost_timers.has_value());
    BOOST_CHECK(err.find("not published") != std::string::npos);
    auto ghost_calls = manager->pending_calls_inspect("ghost_detail", &err);
    BOOST_CHECK(!ghost_calls.has_value());
    BOOST_CHECK(err.find("not published") != std::string::npos);

    const auto timers = manager->timer_inspect(busy_svc.service_id, &err);
    BOOST_REQUIRE_MESSAGE(timers.has_value(), err);
    BOOST_CHECK_EQUAL((*timers)["name"], "cov_l2_busy");
    BOOST_CHECK_GE((*timers)["timers"].get<std::uint64_t>(), 2u);
    BOOST_CHECK_GE((*timers)["repeating"].get<std::uint64_t>(), 1u);
    BOOST_CHECK_GE((*timers)["once"].get<std::uint64_t>(), 1u);
    BOOST_CHECK((*timers)["intervals_ms"].is_array());
    // Nearest due time sits within one interval of now: a small negative
    // value is the in-flight window between the CAF tick firing and the
    // actor-side next-fire advance.
    const int64_t fire_left = (*timers)["next_fire_ms_left"].get<int64_t>();
    BOOST_CHECK_GT(fire_left, -250);
    BOOST_CHECK_LE(fire_left, 250);

    const auto calls =
        manager->pending_calls_inspect(busy_svc.service_id, &err);
    BOOST_REQUIRE_MESSAGE(calls.has_value(), err);
    BOOST_CHECK_GE((*calls)["pending_calls"].get<std::uint64_t>(), 1u);
    const auto& items = (*calls)["calls"];
    BOOST_REQUIRE(items.is_array());
    BOOST_REQUIRE(!items.empty());
    // Both waits were issued by cov_l2_busy; neither is proxied; both are
    // still in flight with a full 5s budget left (ms_left = deadline - now,
    // so a wait inspected in the issuing millisecond reports exactly 5000 —
    // the upper bound is the budget, hence <= and not <).
    bool nap_seen = false;
    for (const auto& c : items) {
        BOOST_CHECK_EQUAL(c["caller"], "cov_l2_busy");
        BOOST_CHECK(c["proxied"] == false);
        BOOST_CHECK_GT(c["ms_left"].get<int64_t>(), 0);
        BOOST_CHECK_LE(c["ms_left"].get<int64_t>(), 5000);
        nap_seen = true;
    }
    BOOST_CHECK(nap_seen);

    // The callee side of the suspended call has no outgoing wait.
    const auto slow_calls =
        manager->pending_calls_inspect(slow_svc.service_id, &err);
    BOOST_REQUIRE_MESSAGE(slow_calls.has_value(), err);
    BOOST_CHECK_EQUAL((*slow_calls)["pending_calls"], 0u);
    BOOST_CHECK((*slow_calls)["calls"].empty());

    // Console projections carry the same detail.
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);
    dispatcher.dispatch(harness.session, "lua.inspect cov_l2_busy timers");
    std::string line = harness.read_line();
    auto resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["name"], "cov_l2_busy");
    BOOST_CHECK(resp["data"]["next_fire_ms_left"].is_number_integer());
    dispatcher.dispatch(harness.session,
                        "lua.inspect cov_l2_busy pending_calls");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["pending_calls"], (*calls)["pending_calls"]);

    // Empty projections keep the gauge-compatible numeric fields.
    dispatcher.dispatch(harness.session,
                        "lua.inspect cov_l2_slow pending_calls");
    line = harness.read_line();
    resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_CHECK_EQUAL(resp["data"]["pending_calls"], 0u);
    BOOST_CHECK(resp["data"]["calls"].is_array());

    // Let the snooze sleep, the nap call and the timers finish/die with the
    // incarnation so teardown does not race them.
    manager->exit(busy_svc.service_id);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
}

// >32 concurrent waits: the report caps at 32 entries with "truncated",
// and the nearest-deadline sort is exercised with both distinct deadlines
// (two waves, different call timeouts) and same-deadline ties (within one
// wave all waits are stamped in the same handler tick).
BOOST_AUTO_TEST_CASE(InspectPendingCallsTruncation) {
    // The waits are coroutine-path spawns: each suspends the caller through
    // suspend_for_call (same pending_calls bookkeeping as shield.call) while
    // the manager's spawn worker drains them one by one — no callee dispatch
    // involved. Wave A uses the default 10s budget, wave B 20s; the 5ms gap
    // between the waves keeps cross-wave deadlines distinct while waits
    // inside one wave are stamped within the same millisecond with
    // overwhelming probability.
    // The children sleep briefly in on_init: the spawn worker finishes one
    // child per interval, so the wait pool drains slowly and stays above
    // the truncation threshold for the whole assertion window.
    const fs::path child =
        fs::temp_directory_path() / "shield_cov_l2_tr_child.lua";
    std::ofstream(child) << "local M = {}\n"
                         << "function M.on_init(args)\n"
                         << "  shield.sleep(300)\n"
                         << "  return true\n"
                         << "end\n"
                         << "return M\n";

    const fs::path busy =
        fs::temp_directory_path() / "shield_cov_l2_tr_busy.lua";
    std::ofstream(busy) << "local M = {}\n"
                        << "local W = {}\n"
                        << "function M.flood_a(ctx)\n"
                        << "  for i = 1, 18 do\n"
                        << "    W[#W + 1] = coroutine.wrap(function()\n"
                        << "      shield.spawn('" << child.string()
                        << "', {name = 'tr_child_a_' .. i})\n"
                        << "    end)\n"
                        << "    W[#W]()\n"
                        << "  end\n"
                        << "end\n"
                        << "function M.flood_b(ctx)\n"
                        << "  for i = 1, 18 do\n"
                        << "    W[#W + 1] = coroutine.wrap(function()\n"
                        << "      shield.spawn('" << child.string()
                        << "', {name = 'tr_child_b_' .. i, timeout = 20000})\n"
                        << "    end)\n"
                        << "    W[#W]()\n"
                        << "  end\n"
                        << "end\n"
                        << "return M\n";

    auto busy_svc = manager->spawn(
        busy.string(), R"({"name":"cov_l2_tr_busy","args":{},"config":{}})");
    BOOST_REQUIRE(busy_svc.success);

    BOOST_REQUIRE(
        manager->send(busy_svc.service_id, "flood_a", nlohmann::json::array()));
    BOOST_REQUIRE(
        manager->send(busy_svc.service_id, "flood_b", nlohmann::json::array()));

    std::string err;
    bool flooded = false;
    std::uint64_t peak = 0;
    // Bounded by wall time, not by poll count: inspect only takes the
    // registry shared lock, so 2000 back-to-back polls can drain in a few
    // milliseconds — on a slower CAF scheduler (macOS runner) that ran out
    // before the flood messages even reached the actor (peak=0,
    // 2026-09-23). The pool stays above 35 for roughly the first child
    // sleep (~300ms), so a 2ms poll cadence inside a 15s ceiling
    // oversamples that window comfortably.
    const auto flood_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (int i = 0;
         !flooded && std::chrono::steady_clock::now() < flood_deadline; ++i) {
        auto d = manager->pending_calls_inspect(busy_svc.service_id, &err);
        const auto n =
            d.has_value() ? (*d)["pending_calls"].get<std::uint64_t>() : 0u;
        if (n > peak) {
            peak = n;
        }
        // 35 gives the assertion window headroom: the worker drains one
        // wait per child on_init sleep, so the pool stays above 33 through
        // both the direct inspect and the console projection below.
        flooded = n >= 35u;
        if (!flooded) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    BOOST_REQUIRE_MESSAGE(
        flooded, "expected 33+ pending waits, peak=" << peak << " err=" << err);

    const auto calls =
        manager->pending_calls_inspect(busy_svc.service_id, &err);
    BOOST_REQUIRE_MESSAGE(calls.has_value(), err);
    BOOST_CHECK_GE((*calls)["pending_calls"].get<std::uint64_t>(), 33u);
    BOOST_REQUIRE((*calls).contains("truncated"));
    BOOST_CHECK((*calls)["truncated"] == true);
    const auto& items = (*calls)["calls"];
    BOOST_REQUIRE(items.is_array());
    BOOST_CHECK_EQUAL(items.size(), 32u);
    // Sorted by nearest deadline; ties keep ascending session ids.
    int64_t prev_left = 0;
    std::uint64_t prev_session = 0;
    for (const auto& c : items) {
        const int64_t ms_left = c["ms_left"].get<int64_t>();
        const std::uint64_t session = c["session"].get<std::uint64_t>();
        BOOST_CHECK_EQUAL(c["caller"], "cov_l2_tr_busy");
        BOOST_CHECK(c["proxied"] == false);
        BOOST_CHECK_GT(ms_left, 0);
        BOOST_CHECK_GE(ms_left, prev_left);
        if (ms_left == prev_left) {
            BOOST_CHECK_GE(session, prev_session);
        }
        prev_left = ms_left;
        prev_session = session;
    }

    // The console projection carries the truncation marker too.
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);
    dispatcher.dispatch(harness.session,
                        "lua.inspect cov_l2_tr_busy pending_calls");
    std::string line = harness.read_line();
    auto resp = nlohmann::json::parse(line);
    BOOST_REQUIRE(resp["type"] == "result");
    BOOST_REQUIRE(resp["data"].contains("truncated"));
    BOOST_CHECK(resp["data"]["truncated"] == true);
    // The two reads race the live spawn-worker drain, so the totals may
    // differ by a child or two between them; the projection must still see
    // an overflowing pool, and its own total vs the capped list stays
    // consistent within one read.
    BOOST_CHECK_GE(resp["data"]["pending_calls"].get<std::uint64_t>(), 33u);
    BOOST_CHECK_GE(resp["data"]["pending_calls"].get<std::uint64_t>(),
                   resp["data"]["calls"].size());

    // No drain wait: the observation window already served its purpose, and
    // the explicit exit cancels the suspended waits and queued spawns (the
    // same teardown proven by the nap/snooze case above).
    manager->exit(busy_svc.service_id);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

// Branch coverage: a fourth lua.snapshot argument trips the args.size() > 3
// guard (usage is surfaced before any registry access), complementing the
// three-argument "not refs" shape covered in LuaSnapshotAndDiffCommands.
BOOST_AUTO_TEST_CASE(SnapshotUsageOnFourthArgument) {
    ConsoleHarness harness;
    shield::console::CommandDispatcher dispatcher;
    shield::console::LuaCommands cmds(*manager, *runtime);
    cmds.register_all(dispatcher);

    dispatcher.dispatch(harness.session, "lua.snapshot svc base refs extra");
    std::string line = harness.read_line();
    BOOST_REQUIRE(!line.empty());
    auto resp = nlohmann::json::parse(line);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("Usage") !=
                std::string::npos);
    BOOST_CHECK(harness.session->multiline_buffer().empty());
}

BOOST_AUTO_TEST_SUITE_END()
