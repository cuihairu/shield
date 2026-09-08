// Coverage tests (round 2) for src/lua/lua_service.cpp: registry lookups,
// hostless fork enqueue, call routing errors, register_name guards,
// shutdown_all budget forcing, error message extraction, and spawn option
// edge cases.
#define BOOST_TEST_MODULE CovLuaService2
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_cov_lua_service2";

std::string write_script(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(kTmpDir);
    const std::string path = kTmpDir + "/" + name;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

std::string opts_for(const std::string& name,
                     nlohmann::json extra = nlohmann::json::object()) {
    nlohmann::json opts = {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        opts[it.key()] = it.value();
    }
    return opts.dump();
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

}  // namespace

// ---------------------------------------------------------------------------
// Registry lookups for unknown services.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RegistryLookupsForUnknownServices) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    BOOST_CHECK_EQUAL(manager.pending_task_count("no_such_service"), 0u);
    BOOST_CHECK(manager.service_vm("no_such_service") == nullptr);
    BOOST_CHECK_EQUAL(manager.query_service("no_such_name"), "");
    BOOST_CHECK(manager.list_services().empty());

    // Hostless fork with no live service actor is dropped.
    BOOST_CHECK_EQUAL(manager.enqueue_forked_task("", [] {}), 0u);
}

// ---------------------------------------------------------------------------
// call(): routing errors for missing targets and reserved method names.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CallRoutingErrors) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov2_echo.lua",
                     "local M = {}\n"
                     "function M.echo(ctx, v) return v end\n"
                     "return M\n");
    auto svc = manager.spawn(path, opts_for("cov2_echo"));
    BOOST_REQUIRE(svc.success);

    // Missing target.
    {
        auto res = manager.call("ghost", "echo", nlohmann::json::array());
        BOOST_CHECK(!res.success);
        BOOST_CHECK(res.error_message.find("service not found") !=
                    std::string::npos);
    }

    // Reserved method name is rejected by validation.
    {
        auto res = manager.call(svc.service_id, "on_reserved",
                                nlohmann::json::array());
        BOOST_CHECK(!res.success);
    }

    // Missing method on a live service.
    {
        auto res = manager.call(svc.service_id, "missing_method",
                                nlohmann::json::array());
        BOOST_CHECK(!res.success);
        BOOST_CHECK(res.error_message.find("method not found") !=
                    std::string::npos);
    }

    // send() to unknown service reports an error.
    {
        std::string error;
        BOOST_CHECK(
            !manager.send("ghost", "echo", nlohmann::json::array(), &error));
        BOOST_CHECK(!error.empty());
    }

    // send_system() to unknown service reports an error.
    {
        std::string error;
        BOOST_CHECK(!manager.send_system("ghost", "on_connect",
                                         nlohmann::json::array(), &error));
        BOOST_CHECK(!error.empty());
    }

    // send_call_request() to unknown service fails.
    {
        std::string error;
        BOOST_CHECK(!manager.send_call_request(
            "ghost", "echo", nlohmann::json::array(), 123, &error));
    }
}

// ---------------------------------------------------------------------------
// register_name guards outside a dispatch context.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RegisterNameGuards) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    std::string error;
    BOOST_CHECK(!manager.register_name("cov2_alias", &error));
    BOOST_CHECK(error.find("current service context") != std::string::npos);

    BOOST_CHECK(!manager.unregister_name("cov2_alias", &error));
    BOOST_CHECK(error.find("current service context") != std::string::npos);
}

// ---------------------------------------------------------------------------
// shutdown_all with a tiny budget takes the force path for slow services.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ShutdownAllBudgetForcePath) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov2_slow_exit.lua",
                     "local M = {}\n"
                     "function M.on_exit(reason)\n"
                     "  local t0 = os.clock()\n"
                     "  while os.clock() - t0 < 0.25 do end\n"
                     "end\n"
                     "return M\n");
    for (int i = 0; i < 3; ++i) {
        auto svc =
            manager.spawn(path, opts_for("cov2_slow_" + std::to_string(i)));
        BOOST_REQUIRE(svc.success);
    }

    // Budget of 1ms: the first slow on_exit exhausts it; the rest are forced.
    manager.shutdown_all("budget_test", 1);
    BOOST_CHECK(manager.list_services().empty());
}

// ---------------------------------------------------------------------------
// spawn(): name collisions and option parsing edges.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SpawnOptionEdges) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov2_plain.lua", "local M = {}\nreturn M\n");

    auto first = manager.spawn(path, opts_for("cov2_dup"));
    BOOST_REQUIRE(first.success);

    // Duplicate name.
    auto dup = manager.spawn(path, opts_for("cov2_dup"));
    BOOST_CHECK(!dup.success);
    BOOST_CHECK(dup.error_message.find("already exists") != std::string::npos);

    // Invalid name characters.
    auto bad = manager.spawn(path, opts_for("cov2 bad name!"));
    BOOST_CHECK(!bad.success);

    // Reserved shield.* prefix.
    auto reserved = manager.spawn(path, opts_for("shield.reserved"));
    BOOST_CHECK(!reserved.success);

    // Missing module file.
    auto missing =
        manager.spawn("/nonexistent/module.lua", opts_for("cov2_missing"));
    BOOST_CHECK(!missing.success);

    // opts that is not an object.
    auto bad_opts = manager.spawn(path, "[1,2,3]");
    BOOST_CHECK(!bad_opts.success);

    manager.shutdown_all("done");
}

// ---------------------------------------------------------------------------
// exec_lua through the manager (used by the console eval command).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ManagerExecLua) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov2_exec_target.lua", "local M = {}\nreturn M\n");
    auto svc = manager.spawn(path, opts_for("cov2_exec_target"));
    BOOST_REQUIRE(svc.success);

    nlohmann::json result;
    std::string error;
    BOOST_CHECK(
        manager.exec_lua(svc.service_id, "return 1+1", &result, &error));
    BOOST_REQUIRE(result.is_array());
    BOOST_CHECK_EQUAL(result[0].get<int64_t>(), 2);

    BOOST_CHECK(!manager.exec_lua(svc.service_id, "error('svc2 boom')", &result,
                                  &error));
    BOOST_CHECK(error.find("svc2 boom") != std::string::npos);

    // Unknown service id.
    BOOST_CHECK(!manager.exec_lua("ghost", "return 1", &result, &error));

    manager.shutdown_all("done");
}

// ---------------------------------------------------------------------------
// exit(): exiting an unknown service is a no-op; exiting twice is safe.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ExitIdempotence) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    // Unknown service: no crash.
    manager.exit("ghost", "normal");

    const std::string path =
        write_script("cov2_exit_target.lua", "local M = {}\nreturn M\n");
    auto svc = manager.spawn(path, opts_for("cov2_exit_target"));
    BOOST_REQUIRE(svc.success);

    manager.exit(svc.service_id, "first");
    BOOST_CHECK(wait_until([&] { return manager.list_services().empty(); },
                           std::chrono::seconds(5)));
    // Second exit of the same id is a no-op.
    manager.exit(svc.service_id, "second");
}
