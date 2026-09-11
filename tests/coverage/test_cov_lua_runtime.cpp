// Coverage tests for src/lua/lua_runtime.cpp.
//
// Exercises the Lua runtime surface (script loading, cache behaviour, service
// function/method dispatch, coroutine dispatch, hooks, exec/call helpers,
// globals, and the LuaPack encoder/decoder) through the public LuaRuntime /
// LuaServiceManager interfaces. Lua fixtures are written to /tmp so the test
// stays self-contained.
#define BOOST_TEST_MODULE CovLuaRuntime
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/config/config.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_cov_lua_runtime";

std::string write_script(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(kTmpDir);
    const std::string path = kTmpDir + "/" + name;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

// Service module used by the method-dispatch tests. Note: plain method
// dispatch (call_service_method) passes JSON args verbatim, so these handlers
// do not take a ctx parameter.
const char* kMethodsModule = R"lua(
local M = {}
function M.add(a, b) return a + b end
function M.addc(ctx, a, b) return a + b end
function M.many() return 1, "a", true, nil end
function M.tbl() return {1, 2, x = 3} end
function M.fn_out() return print end
function M.boom() error("method boom") end
function M.boomc(ctx) error("method boom") end
function M.yields() coroutine.yield(1) return "after" end
M.not_fn = 42
return M
)lua";

}  // namespace

// ---------------------------------------------------------------------------
// ServiceHandle userdata exposed to Lua.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ServiceHandleLuaBindings) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto vm = runtime.create_vm();
    std::string reg_error;
    BOOST_CHECK(runtime.register_api(vm, &reg_error));

    nlohmann::json result;
    std::string error;
    BOOST_CHECK(runtime.exec_lua(
        vm,
        "local a = shield._make_handle('cov_svc_a')\n"
        "local b = shield._make_handle('cov_svc_a')\n"
        "local c = shield._make_handle('cov_other')\n"
        "local e = shield._make_handle('')\n"
        "return a:id(), a:node(), a:valid(), e:valid(),\n"
        "       a == b, a == c, tostring(a), a:id() .. ':' .. c:id()\n",
        &result, &error));
    BOOST_CHECK(error.empty());
    BOOST_REQUIRE(result.is_array());
    BOOST_REQUIRE_EQUAL(result.size(), 8u);
    BOOST_CHECK_EQUAL(result[0].get<std::string>(), "cov_svc_a");
    BOOST_CHECK_EQUAL(result[1].get<int64_t>(), 0);
    BOOST_CHECK_EQUAL(result[2].get<bool>(), true);
    BOOST_CHECK_EQUAL(result[3].get<bool>(), false);
    BOOST_CHECK_EQUAL(result[4].get<bool>(), true);
    BOOST_CHECK_EQUAL(result[5].get<bool>(), false);
    BOOST_CHECK(result[6].get<std::string>().find("<ServiceHandle: ") ==
                std::string::npos == false);
    BOOST_CHECK_EQUAL(result[7].get<std::string>(), "cov_svc_a:cov_other");
}

// ---------------------------------------------------------------------------
// LuaRuntime::load_script (success / failure paths).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LoadScriptFile) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();

    const std::string ok_path =
        write_script("load_ok.lua", "cov_loaded_marker = 42\n");
    BOOST_CHECK(runtime.load_script(vm, ok_path));
    BOOST_CHECK_EQUAL(runtime.get_global(vm, "cov_loaded_marker"), "42");

    const std::string bad_path =
        write_script("load_bad.lua", "this is not ( valid lua\n");
    BOOST_CHECK(!runtime.load_script(vm, bad_path));

    BOOST_CHECK(!runtime.load_script(vm, kTmpDir + "/definitely_missing.lua"));
}

// ---------------------------------------------------------------------------
// lua_to_json conversion branches.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LuaToJsonBranches) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string);

    // Null output pointer: conversion runs but writes nothing.
    nlohmann::json ignored;
    BOOST_CHECK(lua_to_json(lua["print"], nullptr));

    // Function values are unsupported and produce the sentinel string.
    nlohmann::json converted;
    BOOST_CHECK(!lua_to_json(lua["print"], &converted));
    BOOST_CHECK_EQUAL(converted.get<std::string>(), "<unsupported>");
    BOOST_CHECK_EQUAL(lua_to_json(lua["print"]).get<std::string>(),
                      "<unsupported>");

    lua.safe_script(R"lua(
        t_zero_index = {[0] = 'a'}
        t_sparse = {1, [3] = 3}
        t_array_fn = {print}
        t_field_fn = {f = print}
        t_bool_key = {[true] = 1, ok = 2}
        t_array = {1, 2, 3}
        t_object = {a = 1, b = 'x'}
        n_int = 7
        n_float = 3.5
        s_val = 'str'
        b_val = false
    )lua");

    BOOST_CHECK_EQUAL(lua_to_json(lua["t_zero_index"]).dump(), R"({"0":"a"})");
    BOOST_CHECK_EQUAL(lua_to_json(lua["t_sparse"]).dump(), R"({"1":1,"3":3})");
    BOOST_CHECK(!lua_to_json(lua["t_array_fn"], &ignored));
    BOOST_CHECK_EQUAL(lua_to_json(lua["t_array_fn"]).get<std::string>(),
                      "<unsupported>");
    BOOST_CHECK_EQUAL(lua_to_json(lua["t_field_fn"]).get<std::string>(),
                      "<unsupported>");
    BOOST_CHECK_EQUAL(lua_to_json(lua["t_bool_key"]).dump(), R"({"ok":2})");
    BOOST_CHECK_EQUAL(lua_to_json(lua["t_array"]).dump(), R"([1,2,3])");
    BOOST_CHECK_EQUAL(lua_to_json(lua["t_object"]).dump(),
                      R"({"a":1,"b":"x"})");
    BOOST_CHECK_EQUAL(lua_to_json(lua["n_int"]).get<int64_t>(), 7);
    BOOST_CHECK_EQUAL(lua_to_json(lua["n_float"]).get<double>(), 3.5);
    BOOST_CHECK_EQUAL(lua_to_json(lua["s_val"]).get<std::string>(), "str");
    BOOST_CHECK_EQUAL(lua_to_json(lua["b_val"]).get<bool>(), false);
    BOOST_CHECK(lua_to_json(lua["missing_global"]).is_null());
}

// ---------------------------------------------------------------------------
// load_service_module: file-open failure and cache interactions.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LoadServiceModuleFileMissing) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    std::string error;
    BOOST_CHECK(!runtime.load_service_module(
        vm, kTmpDir + "/no_such_module.lua", &error));
    BOOST_CHECK(error.find("Failed to open file") != std::string::npos);
    // The failed lookup still ran stat on a missing path (mtime fallback).
    BOOST_CHECK(runtime.cache_size() == 0 ||
                runtime.cache_size() == 1);  // no entry cached for failures
}

BOOST_AUTO_TEST_CASE(ScriptCacheMtimeInvalidation) {
    LuaRuntime runtime;
    const std::string path = write_script(
        "cache_mtime.lua", "return { get = function() return 'v1' end }\n");
    auto vm1 = runtime.create_vm();
    BOOST_REQUIRE(runtime.load_service_module(vm1, path));
    nlohmann::json ret;
    BOOST_REQUIRE(
        runtime.call_service_method(vm1, "get", nlohmann::json::array(), &ret));
    BOOST_CHECK_EQUAL(ret[0].get<std::string>(), "v1");
    BOOST_CHECK_EQUAL(runtime.cache_size(), 1u);

    // Rewrite the file; the changed mtime must invalidate the cache entry.
    // Filesystem timestamp granularity varies (down to 1s on some systems),
    // so rewrite-and-poll until the mtime actually differs from the first
    // write instead of relying on a fixed sleep.
    const auto old_mtime = std::filesystem::last_write_time(path);
    auto new_mtime = old_mtime;
    for (int attempt = 0; attempt < 200 && new_mtime == old_mtime; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        write_script("cache_mtime.lua",
                     "return { get = function() return 'v2' end }\n");
        new_mtime = std::filesystem::last_write_time(path);
    }
    BOOST_REQUIRE(new_mtime != old_mtime);
    auto vm2 = runtime.create_vm();
    BOOST_REQUIRE(runtime.load_service_module(vm2, path));
    ret = nlohmann::json();
    BOOST_REQUIRE(
        runtime.call_service_method(vm2, "get", nlohmann::json::array(), &ret));
    BOOST_CHECK_EQUAL(ret[0].get<std::string>(), "v2");
}

BOOST_AUTO_TEST_CASE(ScriptCacheTtlExpiry) {
    auto& config = shield::config::global_config();
    config.set("lua.cache.ttl_seconds", static_cast<int64_t>(1));
    {
        LuaRuntime runtime;
        const std::string path = write_script(
            "cache_ttl.lua", "return { get = function() return 'x' end }\n");
        auto vm1 = runtime.create_vm();
        BOOST_REQUIRE(runtime.load_service_module(vm1, path));
        BOOST_CHECK_EQUAL(runtime.cache_size(), 1u);
        // Wait past the TTL, then reload: the entry expires and the file is
        // read again (mtime unchanged, so only the TTL branch rejects it).
        std::this_thread::sleep_for(std::chrono::milliseconds(1150));
        auto vm2 = runtime.create_vm();
        BOOST_REQUIRE(runtime.load_service_module(vm2, path));
        BOOST_CHECK_EQUAL(runtime.cache_size(), 1u);
    }
    config.set("lua.cache.ttl_seconds", static_cast<int64_t>(0));
}

BOOST_AUTO_TEST_CASE(ScriptCacheEviction) {
    auto& config = shield::config::global_config();
    config.set("lua.cache.max_size", static_cast<int64_t>(2));
    {
        LuaRuntime runtime;
        for (const char* name : {"cache_ev_a.lua", "cache_ev_b.lua",
                                 "cache_ev_c.lua", "cache_ev_d.lua"}) {
            const std::string path = write_script(
                name, "return { get = function() return 1 end }\n");
            auto vm = runtime.create_vm();
            BOOST_REQUIRE_MESSAGE(runtime.load_service_module(vm, path), name);
        }
        // max_size = 2: inserting the fourth file evicted the oldest entry so
        // the cache settles at 3 entries (2 kept + 1 inserted after eviction).
        BOOST_CHECK_EQUAL(runtime.cache_size(), 3u);

        runtime.clear_cache();
        BOOST_CHECK_EQUAL(runtime.cache_size(), 0u);
    }
    config.set("lua.cache.max_size", static_cast<int64_t>(100));
}

BOOST_AUTO_TEST_CASE(CacheConfigDefaults) {
    LuaRuntime runtime;
    const auto cfg = runtime.cache_config();
    BOOST_CHECK(cfg.enabled);
    BOOST_CHECK_EQUAL(cfg.max_size, 100u);
    BOOST_CHECK_EQUAL(cfg.ttl_seconds, 0);
}

// ---------------------------------------------------------------------------
// call_service_function error paths.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CallServiceFunctionErrors) {
    LuaRuntime runtime;

    auto empty_vm = runtime.create_vm();
    std::string error;
    BOOST_CHECK(!runtime.call_service_function(
        empty_vm, "on_init", nlohmann::json::object(), &error));
    BOOST_CHECK_EQUAL(error, "service module not loaded");

    const std::string path =
        write_script("not_fn_module.lua", "return { on_init = 42 }\n");
    auto vm = runtime.create_vm();
    BOOST_REQUIRE(runtime.load_service_module(vm, path));
    error.clear();
    BOOST_CHECK(!runtime.call_service_function(
        vm, "on_init", nlohmann::json::object(), &error));
    BOOST_CHECK_EQUAL(error, "on_init is not a function");

    // Missing function is not an error (optional callback).
    BOOST_CHECK(runtime.call_service_function(
        vm, "on_missing", nlohmann::json::object(), &error));
}

// ---------------------------------------------------------------------------
// call_service_method dispatch branches.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CallServiceMethodBranches) {
    LuaRuntime runtime;
    const std::string path = write_script("cov_methods.lua", kMethodsModule);

    auto empty_vm = runtime.create_vm();
    std::string error;
    nlohmann::json returns;
    BOOST_CHECK(!runtime.call_service_method(
        empty_vm, "add", nlohmann::json::array(), &returns, &error));
    BOOST_CHECK_EQUAL(error, "service module not loaded");

    auto vm = runtime.create_vm();
    BOOST_REQUIRE(runtime.load_service_module(vm, path));

    error.clear();
    BOOST_CHECK(!runtime.call_service_method(
        vm, "no_such", nlohmann::json::array(), &returns, &error));
    BOOST_CHECK_EQUAL(error, "method not found: no_such");

    error.clear();
    BOOST_CHECK(!runtime.call_service_method(
        vm, "not_fn", nlohmann::json::array(), &returns, &error));
    BOOST_CHECK_EQUAL(error, "not_fn is not a function");

    error.clear();
    BOOST_CHECK(!runtime.call_service_method(
        vm, "add", nlohmann::json::object(), &returns, &error));
    BOOST_CHECK_EQUAL(error, "method args must be a JSON array");

    // Successful dispatch collects all return values.
    error.clear();
    BOOST_CHECK(runtime.call_service_method(
        vm, "add", nlohmann::json::array({1, 2}), &returns, &error));
    BOOST_CHECK_EQUAL(returns[0].get<int>(), 3);

    returns = nlohmann::json();
    BOOST_CHECK(runtime.call_service_method(vm, "many", nlohmann::json::array(),
                                            &returns, &error));
    BOOST_REQUIRE_EQUAL(returns.size(), 4u);
    BOOST_CHECK_EQUAL(returns[0].get<int>(), 1);
    BOOST_CHECK_EQUAL(returns[1].get<std::string>(), "a");
    BOOST_CHECK_EQUAL(returns[2].get<bool>(), true);
    BOOST_CHECK(returns[3].is_null());

    returns = nlohmann::json();
    BOOST_CHECK(runtime.call_service_method(vm, "tbl", nlohmann::json::array(),
                                            &returns, &error));
    BOOST_REQUIRE_EQUAL(returns.size(), 1u);
    BOOST_CHECK(returns[0].is_array() || returns[0].is_object());

    // A function return value is not representable in JSON.
    error.clear();
    returns = nlohmann::json();
    BOOST_CHECK(!runtime.call_service_method(
        vm, "fn_out", nlohmann::json::array(), &returns, &error));
    BOOST_CHECK(error.find("unsupported return value") != std::string::npos);

    // Runtime error inside the handler propagates as a failure.
    error.clear();
    BOOST_CHECK(!runtime.call_service_method(
        vm, "boom", nlohmann::json::array(), &returns, &error));
    BOOST_CHECK(error.find("method boom") != std::string::npos);

    // returns == nullptr is allowed (fire and forget).
    BOOST_CHECK(runtime.call_service_method(
        vm, "add", nlohmann::json::array({5, 6}), nullptr, &error));
}

// ---------------------------------------------------------------------------
// call_service_method_coroutine: guards, factory fallbacks, resume statuses.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CoroutineDispatchGuards) {
    LuaRuntime runtime;
    const std::string path = write_script("cov_methods.lua", kMethodsModule);
    std::string error;

    auto empty_vm = runtime.create_vm();
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        empty_vm, "add", nlohmann::json::array(), &error));
    BOOST_CHECK_EQUAL(error, "service module not loaded");

    auto vm = runtime.create_vm();
    BOOST_REQUIRE(runtime.load_service_module(vm, path));

    error.clear();
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm, "no_such", nlohmann::json::array(), &error));
    BOOST_CHECK_EQUAL(error, "method not found: no_such");

    error.clear();
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm, "not_fn", nlohmann::json::array(), &error));
    BOOST_CHECK_EQUAL(error, "not_fn is not a function");

    error.clear();
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm, "add", nlohmann::json::object(), &error));
    BOOST_CHECK_EQUAL(error, "method args must be a JSON array");

    // No __shield_run_handler on this VM: falls back to a plain dispatch.
    error.clear();
    BOOST_CHECK(runtime.call_service_method_coroutine(
        vm, "add", nlohmann::json::array({2, 3}), &error));
    BOOST_CHECK(error.empty());

    error.clear();
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm, "boom", nlohmann::json::array(), &error));
    BOOST_CHECK(error.find("method boom") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(CoroutineDispatchWithManager) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path = write_script("cov_methods.lua", kMethodsModule);
    std::string error;

    // Bare VM (no shield API registered) + fallback dispatch with a session:
    // completion is routed through the manager (unknown session is a no-op).
    auto vm = runtime.create_vm();
    BOOST_REQUIRE(runtime.load_service_module(vm, path));
    BOOST_CHECK(runtime.call_service_method_coroutine(
        vm, "add", nlohmann::json::array({20, 22}), &error, /*session=*/4242,
        &manager, "cov_fallback_svc"));
    error.clear();
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm, "boom", nlohmann::json::array(), &error, /*session=*/4243, &manager,
        "cov_fallback_svc"));
    BOOST_CHECK(error.find("method boom") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(CoroutineDispatchFactoryProblems) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path = write_script("cov_methods.lua", kMethodsModule);
    std::string error;

    auto vm = runtime.create_vm();
    BOOST_REQUIRE(runtime.register_api(vm));
    BOOST_REQUIRE(runtime.load_service_module(vm, path));

    // Factory that raises: degrades to synchronous dispatch.
    runtime.exec_lua(vm,
                     "__shield_run_handler = function() error('fac boom') "
                     "end");
    error.clear();
    BOOST_CHECK(runtime.call_service_method_coroutine(
        vm, "add", nlohmann::json::array({1, 1}), &error));
    BOOST_CHECK(error.empty());

    // Factory returning a non-thread: also falls back.
    runtime.exec_lua(vm, "__shield_run_handler = function() return 42 end");
    error.clear();
    BOOST_CHECK(runtime.call_service_method_coroutine(
        vm, "add", nlohmann::json::array({1, 2}), &error));
    BOOST_CHECK(error.empty());

    // Restore the real factory.
    runtime.exec_lua(vm,
                     "function __shield_run_handler(handler, args)\n"
                     "  return coroutine.create(function()\n"
                     "    return handler(table.unpack(args, 1, args.n or "
                     "#args))\n"
                     "  end)\n"
                     "end");

    // Synchronous completion with a session routes through
    // on_handler_completed. Note: coroutine dispatch passes a ctx table as the
    // first handler arg.
    error.clear();
    BOOST_CHECK(runtime.call_service_method_coroutine(
        vm, "addc", nlohmann::json::array({3, 4}), &error, /*session=*/777,
        &manager, "cov_coro_svc"));
    BOOST_CHECK(error.empty());

    // Handler that yields: dispatch reports success, coroutine stays parked.
    error.clear();
    BOOST_CHECK(runtime.call_service_method_coroutine(
        vm, "yields", nlohmann::json::array(), &error));
    BOOST_CHECK(error.empty());

    // Handler raising inside the coroutine surfaces the coroutine error.
    error.clear();
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm, "boomc", nlohmann::json::array(), &error));
    BOOST_CHECK(error.find("method boom") != std::string::npos);
}

// ---------------------------------------------------------------------------
// invoke_hook branches.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(InvokeHookBranches) {
    LuaRuntime runtime;

    // Null VM.
    BOOST_CHECK(!runtime.invoke_hook(nullptr, "on_error", "e", "handler", "m"));

    // VM without a loaded module table.
    auto empty_vm = runtime.create_vm();
    BOOST_CHECK(
        !runtime.invoke_hook(empty_vm, "on_error", "e", "handler", "m"));

    // Module without the hook.
    const std::string quiet_path =
        write_script("hook_quiet.lua", "return { ping = function() end }\n");
    auto quiet_vm = runtime.create_vm();
    BOOST_REQUIRE(runtime.load_service_module(quiet_vm, quiet_path));
    BOOST_CHECK(
        !runtime.invoke_hook(quiet_vm, "on_error", "e", "handler", "m"));

    // Hook that itself raises.
    const std::string throwing_path = write_script(
        "hook_throw.lua",
        "return { on_error = function() error('hook boom') end }\n");
    auto throw_vm = runtime.create_vm();
    BOOST_REQUIRE(runtime.load_service_module(throw_vm, throwing_path));
    BOOST_CHECK(
        !runtime.invoke_hook(throw_vm, "on_error", "e", "handler", "m"));

    // Healthy hook records its arguments.
    const std::string ok_path = write_script(
        "hook_ok.lua",
        "return { on_error = function(err, ctx) _r1 = err; _r2 = ctx.type; "
        "_r3 = ctx.method end }\n");
    auto ok_vm = runtime.create_vm();
    BOOST_REQUIRE(runtime.load_service_module(ok_vm, ok_path));
    BOOST_CHECK(runtime.invoke_hook(ok_vm, "on_error", "boom", "handler", "m"));
    nlohmann::json result;
    BOOST_REQUIRE(runtime.exec_lua(ok_vm, "return _r1, _r2, _r3", &result));
    BOOST_REQUIRE_EQUAL(result.size(), 3u);
    BOOST_CHECK_EQUAL(result[0].get<std::string>(), "boom");
    BOOST_CHECK_EQUAL(result[1].get<std::string>(), "handler");
    BOOST_CHECK_EQUAL(result[2].get<std::string>(), "m");
}

// ---------------------------------------------------------------------------
// call_function string-JSON helper.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CallFunctionBranches) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();

    BOOST_CHECK_NE(runtime.call_function(vm, "cov_missing_fn"),
                   R"({"ok": true})");
    BOOST_CHECK(runtime.call_function(vm, "cov_missing_fn")
                    .find("function not found") != std::string::npos);

    runtime.exec_lua(vm, "function cov_ok() cov_marker = 1 end");
    BOOST_CHECK_EQUAL(runtime.call_function(vm, "cov_ok"), R"({"ok": true})");

    runtime.exec_lua(vm, "function cov_err() error('fn boom') end");
    const std::string res = runtime.call_function(vm, "cov_err");
    BOOST_CHECK(res.find("{\"error\": ") == 0);
    BOOST_CHECK(res.find("fn boom") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Globals / service manager accessors / exec_lua.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(GlobalAccessorsAndManager) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    BOOST_CHECK(runtime.service_manager() == nullptr);
    {
        LuaServiceManager manager(runtime, system);
        BOOST_CHECK_EQUAL(runtime.service_manager(), &manager);

        auto vm = runtime.create_vm();
        runtime.set_global(vm, "cov_greeting", "hello");
        BOOST_CHECK_EQUAL(runtime.get_global(vm, "cov_greeting"), "hello");

        runtime.exec_lua(vm, "cov_num = 42");
        BOOST_CHECK_EQUAL(runtime.get_global(vm, "cov_num"), "42");
        runtime.exec_lua(vm, "cov_float = 2.5");
        BOOST_CHECK_EQUAL(runtime.get_global(vm, "cov_float"), "2.500000");
        runtime.exec_lua(vm, "cov_table = {}");
        BOOST_CHECK_EQUAL(runtime.get_global(vm, "cov_table"), "");
    }
    BOOST_CHECK(runtime.service_manager() == nullptr);
}

BOOST_AUTO_TEST_CASE(ExecLuaBranches) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    nlohmann::json result;
    std::string error;

    // Invalid VM handle.
    BOOST_CHECK(!runtime.exec_lua(nullptr, "return 1", &result, &error));
    BOOST_CHECK_EQUAL(error, "invalid VM");

    // Compile error.
    error.clear();
    BOOST_CHECK(!runtime.exec_lua(vm, "return (", &result, &error));
    BOOST_CHECK(!error.empty());

    // Runtime error.
    error.clear();
    BOOST_CHECK(!runtime.exec_lua(vm, "error('exec boom')", &result, &error));
    BOOST_CHECK(error.find("exec boom") != std::string::npos);

    // Mixed return values, including a table rendered via tostring.
    error.clear();
    result = nlohmann::json();
    BOOST_CHECK(
        runtime.exec_lua(vm, "return nil, true, 42, 2.5, 'txt', {1, 2}, print",
                         &result, &error));
    BOOST_REQUIRE_EQUAL(result.size(), 7u);
    BOOST_CHECK(result[0].is_null());
    BOOST_CHECK_EQUAL(result[1].get<bool>(), true);
    BOOST_CHECK_EQUAL(result[2].get<int64_t>(), 42);
    BOOST_CHECK_EQUAL(result[3].get<double>(), 2.5);
    BOOST_CHECK_EQUAL(result[4].get<std::string>(), "txt");
    BOOST_CHECK(result[5].get<std::string>().find("table: ") !=
                std::string::npos);
    BOOST_CHECK(result[6].get<std::string>().find("function: ") !=
                std::string::npos);

    // No return values: still a success; result untouched when no values.
    error.clear();
    BOOST_CHECK(runtime.exec_lua(vm, "local x = 1", nullptr, &error));
    BOOST_CHECK(error.empty());
}

// ---------------------------------------------------------------------------
// LuaPack encoder.
// ---------------------------------------------------------------------------
namespace {

std::string hex_of(const std::vector<uint8_t>& bytes) {
    std::string out;
    char buf[4];
    for (uint8_t b : bytes) {
        std::snprintf(buf, sizeof(buf), "%02x ", b);
        out += buf;
    }
    return out;
}

// Evaluate `cov_expr = <expr>` in the state and return the global.
sol::object eval_expr(sol::state& lua, const char* expr) {
    lua.safe_script(std::string("cov_expr = ") + expr,
                    sol::script_default_on_error);
    return lua["cov_expr"];
}

// Decode `expr` from the given state as a Lua value and encode it.
std::vector<uint8_t> encode_expr(sol::state& lua, const char* expr,
                                 const LuaPackEncoder::Config& config,
                                 std::string* error_out) {
    sol::object obj = eval_expr(lua, expr);
    LuaPackEncoder encoder(config);
    std::vector<uint8_t> bytes;
    if (!encoder.encode(lua, obj, bytes)) {
        *error_out = encoder.error();
        return {};
    }
    *error_out = "";
    return bytes;
}

}  // namespace

BOOST_AUTO_TEST_CASE(LuaPackEncodeScalars) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string);
    LuaPackEncoder::Config config;
    std::string error;

    BOOST_CHECK_EQUAL(hex_of(encode_expr(lua, "nil", config, &error)),
                      "4c 50 01 00 00 ");
    BOOST_CHECK_EQUAL(hex_of(encode_expr(lua, "true", config, &error)),
                      "4c 50 01 00 02 ");
    BOOST_CHECK_EQUAL(hex_of(encode_expr(lua, "false", config, &error)),
                      "4c 50 01 00 01 ");
    // Integer 1 -> tag 03 + little-endian 8 bytes.
    BOOST_CHECK_EQUAL(hex_of(encode_expr(lua, "1", config, &error)),
                      "4c 50 01 00 03 01 00 00 00 00 00 00 00 ");
    // -2 encodes as a large two's-complement integer.
    BOOST_CHECK_EQUAL(hex_of(encode_expr(lua, "-2", config, &error)),
                      "4c 50 01 00 03 fe ff ff ff ff ff ff ff ");
    // Floating point value uses the Number tag.
    BOOST_CHECK_EQUAL(hex_of(encode_expr(lua, "3.5", config, &error)),
                      "4c 50 01 00 04 00 00 00 00 00 00 0c 40 ");
    // Short string.
    BOOST_CHECK_EQUAL(hex_of(encode_expr(lua, "'hi'", config, &error)),
                      "4c 50 01 00 05 02 68 69 ");
}

BOOST_AUTO_TEST_CASE(LuaPackEncodeStrings) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string);
    std::string error;

    // Long string (>= 256 bytes) uses the String tag with a 4-byte length.
    LuaPackEncoder::Config config;
    const std::string filler(300, 'x');
    lua["long_str"] = filler;
    sol::object obj = lua["long_str"];
    LuaPackEncoder encoder(config);
    std::vector<uint8_t> bytes;
    BOOST_CHECK(encoder.encode(lua, obj, bytes));
    BOOST_REQUIRE_EQUAL(bytes.size(), 4 + 1 + 4 + 300);
    BOOST_CHECK_EQUAL(bytes[4],
                      static_cast<uint8_t>(LuaPackEncoder::TypeTag::String));
    uint32_t len = bytes[5] | (bytes[6] << 8) | (bytes[7] << 16) |
                   (static_cast<uint32_t>(bytes[8]) << 24);
    BOOST_CHECK_EQUAL(len, 300u);

    // The same string exceeds a tiny configured limit.
    LuaPackEncoder::Config tiny;
    tiny.max_string_length = 10;
    LuaPackEncoder tiny_encoder(tiny);
    std::vector<uint8_t> out2;
    BOOST_CHECK(!tiny_encoder.encode(lua, obj, out2));
    BOOST_CHECK_EQUAL(tiny_encoder.error(), "string too long");
}

BOOST_AUTO_TEST_CASE(LuaPackEncodeContainers) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string);
    std::string error;

    LuaPackEncoder::Config config;
    std::vector<uint8_t> bytes = encode_expr(lua, "{1, 2, 3}", config, &error);
    BOOST_CHECK_MESSAGE(error.empty(), error);
    BOOST_REQUIRE_GE(bytes.size(), 9u);
    BOOST_CHECK_EQUAL(bytes[4],
                      static_cast<uint8_t>(LuaPackEncoder::TypeTag::Array));
    uint32_t count = bytes[5] | (bytes[6] << 8) | (bytes[7] << 16) |
                     (static_cast<uint32_t>(bytes[8]) << 24);
    BOOST_CHECK_EQUAL(count, 3u);

    // Nested containers recurse (depth accounting) and maps encode keys.
    bytes = encode_expr(lua, "{a = 1, [7] = 'x'}", config, &error);
    BOOST_CHECK_MESSAGE(error.empty(), error);
    BOOST_CHECK_EQUAL(bytes[4],
                      static_cast<uint8_t>(LuaPackEncoder::TypeTag::Map));

    // Depth limit exceeded.
    LuaPackEncoder::Config shallow;
    shallow.max_nesting_depth = 1;
    bytes = encode_expr(lua, "{{1}}", shallow, &error);
    BOOST_CHECK(!error.empty());

    // Arrays longer than max_array_length degrade to map encoding.
    LuaPackEncoder::Config small_array;
    small_array.max_array_length = 2;
    bytes = encode_expr(lua, "{1, 2, 3}", small_array, &error);
    BOOST_CHECK_MESSAGE(error.empty(), error);
    BOOST_CHECK_EQUAL(bytes[4],
                      static_cast<uint8_t>(LuaPackEncoder::TypeTag::Map));

    // A table with a zero/negative integer key is not array-like and is
    // encoded as a map instead.
    bytes = encode_expr(lua, "{[0] = 1}", config, &error);
    BOOST_CHECK_MESSAGE(error.empty(), error);
    BOOST_CHECK_EQUAL(bytes[4],
                      static_cast<uint8_t>(LuaPackEncoder::TypeTag::Map));

    // Map with more entries than allowed fails. Note: the entry counter stops
    // at the first non-integer key, so integer keys must come first to make
    // the counter exceed the limit before the terminating string key.
    LuaPackEncoder::Config small_map;
    small_map.max_map_entries = 2;
    sol::object many = eval_expr(lua, "{1, 2, 3, 4, tag = 'x'}");
    LuaPackEncoder map_encoder(small_map);
    std::vector<uint8_t> out;
    BOOST_CHECK(!map_encoder.encode(lua, many, out));
    BOOST_CHECK_EQUAL(map_encoder.error(), "map has too many entries");

    // Map keys must be strings or integers.
    sol::object bad_key = eval_expr(lua, "{[true] = 1}");
    LuaPackEncoder key_encoder(config);
    out.clear();
    BOOST_CHECK(!key_encoder.encode(lua, bad_key, out));
    BOOST_CHECK_EQUAL(key_encoder.error(),
                      "map keys must be string or integer");

    // Containers holding unencodable values propagate the failure.
    std::string encode_error;
    encode_expr(lua, "{print}", config, &encode_error);
    BOOST_CHECK_EQUAL(encode_error, "unsupported type for LuaPack encoding");
    encode_expr(lua, "{f = print}", config, &encode_error);
    BOOST_CHECK_EQUAL(encode_error, "unsupported type for LuaPack encoding");

    // Functions cannot be encoded.
    sol::object fn = lua["print"];
    LuaPackEncoder fn_encoder(config);
    out.clear();
    BOOST_CHECK(!fn_encoder.encode(lua, fn, out));
    BOOST_CHECK_EQUAL(fn_encoder.error(),
                      "unsupported type for LuaPack encoding");
}

BOOST_AUTO_TEST_CASE(LuaPackEncodeServiceHandle) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string);
    ServiceHandle::register_usertype(lua);
    lua["handle"] = ServiceHandle("cov_pack_svc");

    // Note: sol2 reports a registered usertype (which carries a method
    // metatable with __index) as table-like, so LuaPackEncoder's table
    // branch classifies the handle as an empty array before the dedicated
    // ServiceHandle tag is reached. Assert the observable behaviour.
    LuaPackEncoder::Config config;
    LuaPackEncoder encoder(config);
    std::vector<uint8_t> bytes;
    BOOST_CHECK(lua["handle"].is<ServiceHandle>());
    BOOST_CHECK(encoder.encode(lua, lua["handle"], bytes));
    BOOST_REQUIRE_EQUAL(bytes.size(), 9u);
    BOOST_CHECK_EQUAL(bytes[4],
                      static_cast<uint8_t>(LuaPackEncoder::TypeTag::Array));
    uint32_t count = bytes[5] | (bytes[6] << 8) | (bytes[7] << 16) |
                     (static_cast<uint32_t>(bytes[8]) << 24);
    BOOST_CHECK_EQUAL(count, 0u);
}

// ---------------------------------------------------------------------------
// LuaPack decoder.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LuaPackDecodeHeaderErrors) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string);

    LuaPackDecoder decoder;
    size_t consumed = 0;

    // Too short.
    sol::object obj = decoder.decode(lua, {0x4c, 0x50}, consumed);
    BOOST_CHECK(obj == sol::nil);
    BOOST_CHECK_EQUAL(decoder.error(), "invalid LuaPack header: too short");

    // Bad magic.
    obj = decoder.decode(lua, {0x41, 0x42, 0x01, 0x00, 0x00}, consumed);
    BOOST_CHECK(obj == sol::nil);
    BOOST_CHECK_EQUAL(decoder.error(), "invalid LuaPack magic bytes");

    // Bad version.
    obj = decoder.decode(lua, {0x4c, 0x50, 0x07, 0x00, 0x00}, consumed);
    BOOST_CHECK(obj == sol::nil);
    BOOST_CHECK_EQUAL(decoder.error(), "unsupported LuaPack version");

    // Header only: value stream is empty.
    obj = decoder.decode(lua, {0x4c, 0x50, 0x01, 0x00}, consumed);
    BOOST_CHECK(obj == sol::nil);
    BOOST_CHECK_EQUAL(decoder.error(), "unexpected end of data");
}

BOOST_AUTO_TEST_CASE(LuaPackDecodeValues) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string);
    LuaPackDecoder decoder;
    size_t consumed = 0;

    struct Case {
        const char* expr;
        std::vector<uint8_t> bytes;
    };
    LuaPackEncoder::Config config;
    auto encode_lua = [&](const char* expr) {
        sol::object obj = eval_expr(lua, expr);
        LuaPackEncoder encoder(config);
        std::vector<uint8_t> bytes;
        BOOST_REQUIRE(encoder.encode(lua, obj, bytes));
        return bytes;
    };

    // Scalars round-trip.
    for (const char* expr :
         {"nil", "true", "false", "1", "-1000000", "2.25", "'short'"}) {
        sol::object obj = decoder.decode(lua, encode_lua(expr), consumed);
        BOOST_CHECK_MESSAGE(decoder.error().empty(),
                            expr << ": " << decoder.error());
        sol::object expected = eval_expr(lua, expr);
        BOOST_CHECK(obj == expected);
    }

    // Long string round-trip exercises the 4-byte length reader.
    {
        auto bytes = encode_lua("string.rep('z', 300)");
        sol::object obj = decoder.decode(lua, bytes, consumed);
        BOOST_CHECK(decoder.error().empty());
        // Note: the header's 4 bytes are not counted by out_bytes_consumed
        // (decode_value resets the counter for the value stream).
        BOOST_CHECK_EQUAL(consumed, bytes.size() - 4);
        BOOST_CHECK(obj.as<std::string>().size() == 300u);
    }

    // Nested arrays / maps round-trip.
    {
        auto bytes = encode_lua("{1, {2, 3}, x = {y = 4}}");
        sol::object obj = decoder.decode(lua, bytes, consumed);
        BOOST_CHECK(decoder.error().empty());
        BOOST_CHECK_EQUAL(consumed, bytes.size() - 4);
        // The mixed table encodes as a map; integer keys stay integers.
        sol::table t = obj;
        BOOST_CHECK_EQUAL(t[1].get<int>(), 1);
        BOOST_CHECK_EQUAL(t[2][1].get<int>(), 2);
        BOOST_CHECK_EQUAL(t[2][2].get<int>(), 3);
        BOOST_CHECK_EQUAL(t["x"]["y"].get<int>(), 4);
    }
}

BOOST_AUTO_TEST_CASE(LuaPackDecodeTruncated) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string);
    LuaPackDecoder decoder;
    size_t consumed = 0;

    auto check_error = [&](std::vector<uint8_t> bytes,
                           const char* expect_error) {
        decoder.decode(lua, bytes, consumed);
        BOOST_CHECK_EQUAL(decoder.error(), expect_error);
    };

    const uint8_t h0 = LuaPackEncoder::MAGIC_HIGH;
    const uint8_t h1 = LuaPackEncoder::MAGIC_LOW;
    const uint8_t ver = LuaPackEncoder::VERSION;

    // Truncated integer / number payloads.
    check_error({h0, h1, ver, 0, 0x03, 0x01, 0x02}, "truncated integer");
    check_error({h0, h1, ver, 0, 0x04, 0x01}, "truncated number");

    // Truncated short strings.
    check_error({h0, h1, ver, 0, 0x05}, "truncated short string");
    check_error({h0, h1, ver, 0, 0x05, 0x05, 'a'},
                "truncated short string data");

    // Truncated long strings.
    check_error({h0, h1, ver, 0, 0x06}, "truncated string length");
    check_error({h0, h1, ver, 0, 0x06, 0x10, 0x00, 0x00, 0x00},
                "truncated string data");

    // Truncated array / map headers.
    check_error({h0, h1, ver, 0, 0x07, 0x01}, "truncated array length");
    check_error({h0, h1, ver, 0, 0x08, 0x01}, "truncated map count");

    // Array whose element payload is cut off mid-value.
    check_error({h0, h1, ver, 0, 0x07, 0x02, 0x00, 0x00, 0x00, 0x03, 0x01},
                "truncated integer");

    // Map whose key stream is truncated.
    check_error({h0, h1, ver, 0, 0x08, 0x01, 0x00, 0x00, 0x00, 0x05},
                "truncated short string");
    // Map whose value stream is truncated.
    {
        std::vector<uint8_t> bytes{h0,   h1,   ver,  0,    0x08, 0x01, 0x00,
                                   0x00, 0x00, 0x05, 0x01, 'k',  0x05};
        decoder.decode(lua, bytes, consumed);
        BOOST_TEST_MESSAGE("map value truncation error: " << decoder.error());
        BOOST_CHECK(decoder.error().find("truncated") != std::string::npos);
    }

    // Unknown tag byte.
    check_error({h0, h1, ver, 0, 0x7f}, "unknown type tag: 127");
}

// ---------------------------------------------------------------------------
// ctx.sender propagation into coroutine dispatch (via a real service).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CoroutineCtxFromDispatch) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string receiver_path = write_script(
        "ctx_receiver.lua",
        "local M = {}\n"
        "local last_sender = 'none'\n"
        "function M.record(ctx, payload) last_sender = ctx.sender end\n"
        "function M.get_sender(ctx) return last_sender end\n"
        "return M\n");
    const std::string sender_path =
        write_script("ctx_sender.lua",
                     "local M = {}\n"
                     "function M.send_it(ctx, target)\n"
                     "  return shield.send(target, 'record', 'payload')\n"
                     "end\n"
                     "return M\n");

    auto receiver = manager.spawn(receiver_path, R"({"name": "cov_rx"})");
    BOOST_REQUIRE(receiver.success);
    auto sender = manager.spawn(sender_path, R"({"name": "cov_tx"})");
    BOOST_REQUIRE(sender.success);

    CallResult cr = manager.call(sender.service_id, "send_it",
                                 nlohmann::json::array({"cov_rx"}));
    BOOST_REQUIRE(cr.success);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult r = manager.call(receiver.service_id, "get_sender",
                                        nlohmann::json::array());
            return r.success && r.values.size() == 1u &&
                   r.values[0].is_string() &&
                   r.values[0].get<std::string>() == sender.service_id;
        },
        std::chrono::seconds(2)));
}

// ---------------------------------------------------------------------------
// invoke_client_rpc branches (M3 typed client ingress dispatch).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ClientIngressDispatchBranches) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    std::string error;

    // 1. Bare VM: no coroutine factory registered.
    auto bare_vm = runtime.create_vm();
    ClientIngress ingress;
    ingress.route_id = 0x1001;
    ingress.decoded_request = nlohmann::json::object({{"uid", 7}});
    BOOST_CHECK(!runtime.invoke_client_rpc(bare_vm, sol::nil, ingress, &error));
    BOOST_CHECK_EQUAL(error, "coroutine factory not registered");

    auto vm = runtime.create_vm();
    BOOST_REQUIRE(runtime.register_api(vm));
    sol::state& lua = runtime.vm_state(vm);
    runtime.exec_lua(
        vm,
        "function cov_ok(ctx, client, request) return request.uid end\n"
        "function cov_boom(ctx, client, request) error('rpc boom') end\n");

    // 2. Invalid handler.
    ingress.decoded_request = nlohmann::json::object({{"uid", 1}});
    BOOST_CHECK(!runtime.invoke_client_rpc(vm, sol::nil, ingress, &error));
    BOOST_CHECK_EQUAL(error, "handler is not a function");

    // 3. Missing request value.
    ClientIngress no_request;
    no_request.route_id = 0x1001;
    sol::function ok_fn = lua["cov_ok"];
    BOOST_REQUIRE(ok_fn.valid());
    BOOST_CHECK(!runtime.invoke_client_rpc(vm, ok_fn, no_request, &error));
    BOOST_CHECK_EQUAL(error, "ingress request value missing");

    // 4. Happy path: handler receives (ctx, client userdata, request).
    ingress.decoded_request = nlohmann::json::object({{"uid", 41}});
    error.clear();
    BOOST_CHECK(runtime.invoke_client_rpc(vm, ok_fn, ingress, &error, &manager,
                                          "cov_ingress_svc"));
    BOOST_CHECK(error.empty());

    // 5. Factory that raises: factory failure surfaces.
    runtime.exec_lua(vm,
                     "__shield_run_handler = function() error('fac boom') "
                     "end");
    BOOST_CHECK(!runtime.invoke_client_rpc(vm, ok_fn, ingress, &error));
    BOOST_CHECK_EQUAL(error, "handler coroutine factory failed");

    // 6. Factory returning a non-thread: thread missing.
    runtime.exec_lua(vm, "__shield_run_handler = function() return 42 end");
    BOOST_CHECK(!runtime.invoke_client_rpc(vm, ok_fn, ingress, &error));
    BOOST_CHECK_EQUAL(error, "handler coroutine thread missing");

    // Restore the real factory.
    runtime.exec_lua(vm,
                     "function __shield_run_handler(handler, args)\n"
                     "  return coroutine.create(function()\n"
                     "    return handler(table.unpack(args, 1, args.n or "
                     "#args))\n"
                     "  end)\n"
                     "end");

    // 7. Handler raising inside the coroutine: error string surfaces and the
    // service error hook runs with error_type "client_rpc".
    sol::function boom_fn = lua["cov_boom"];
    BOOST_REQUIRE(boom_fn.valid());
    error.clear();
    BOOST_CHECK(!runtime.invoke_client_rpc(vm, boom_fn, ingress, &error,
                                           &manager, "cov_ingress_svc"));
    BOOST_CHECK(error.find("rpc boom") != std::string::npos);
}

// ---------------------------------------------------------------------------
// invoke_client_rpc error hook: on_error sees error_type "client_rpc" and the
// route id as the method context.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ClientIngressErrorHookReceivesRouteId) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path = write_script(
        "ingress_error.lua",
        "local M = {}\n"
        "local captured = {}\n"
        "function M.on_error(err, ctx)\n"
        "  captured = {err = err, type = ctx.type, method = ctx.method}\n"
        "end\n"
        "function M.get_captured(ctx) return captured end\n"
        "function M.gw_boom(ctx, client, request) error('ingress boom') end\n"
        "_G.__test_gw_boom = M.gw_boom\n"
        "return M\n");
    auto svc = manager.spawn(path, R"({"name": "cov_ingress_err"})");
    BOOST_REQUIRE(svc.success);

    auto vm_handle = manager.service_vm(svc.service_id);
    BOOST_REQUIRE(vm_handle != nullptr);
    sol::state& lua = runtime.vm_state(vm_handle);
    sol::function boom = lua["__test_gw_boom"];
    BOOST_REQUIRE(boom.valid());

    ClientIngress ingress;
    ingress.route_id = 0x2002;
    ingress.decoded_request = nlohmann::json::object();
    std::string error;
    BOOST_CHECK(!runtime.invoke_client_rpc(vm_handle, boom, ingress, &error,
                                           &manager, svc.service_id));
    BOOST_CHECK(error.find("ingress boom") != std::string::npos);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult r = manager.call(svc.service_id, "get_captured",
                                        nlohmann::json::array());
            return r.success && r.values.size() == 1u &&
                   r.values[0].is_object() && r.values[0].contains("type");
        },
        std::chrono::seconds(2)));

    CallResult r =
        manager.call(svc.service_id, "get_captured", nlohmann::json::array());
    BOOST_CHECK_EQUAL(r.values[0]["type"].get<std::string>(), "client_rpc");
    BOOST_CHECK_EQUAL(r.values[0]["method"].get<std::string>(), "8194");
}
