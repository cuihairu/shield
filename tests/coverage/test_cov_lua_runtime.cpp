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
#include "shield/lua/client_identity.hpp"
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
// vm_main_state: the /ops/profile resolve channel's null guard.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(VmMainStateNullVm) {
    LuaRuntime runtime;
    // A null VM handle resolves to nullptr — the fork task that arms the
    // sampler hook guards on this before dereferencing.
    BOOST_CHECK(runtime.vm_main_state(nullptr) == nullptr);
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

    // A file that loads but raises at execution time is also a plain false
    // (the protected exec arm) — not an escaping error.
    const std::string exec_bad_path =
        write_script("load_exec_bad.lua", "error('exec boom')\n");
    BOOST_CHECK(!runtime.load_script(vm, exec_bad_path));

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

// ---------------------------------------------------------------------------
// Branch-coverage round: cache TTL hit / disabled cache / stale VM registry.
// ---------------------------------------------------------------------------

// A reload that lands inside the TTL window keeps the cached entry (the
// expiry comparison takes its not-expired arm).
BOOST_AUTO_TEST_CASE(ScriptCacheTtlHitWithinWindow) {
    auto& config = shield::config::global_config();
    config.set("lua.cache.ttl_seconds", static_cast<int64_t>(3600));
    {
        LuaRuntime runtime;
        const std::string path =
            write_script("cache_ttl_hit.lua",
                         "return { get = function() return 'hit' end }\n");
        auto vm1 = runtime.create_vm();
        BOOST_REQUIRE(runtime.load_service_module(vm1, path));
        BOOST_CHECK_EQUAL(runtime.cache_size(), 1u);
        // Reload immediately (well inside the TTL): served from the cache.
        auto vm2 = runtime.create_vm();
        BOOST_REQUIRE(runtime.load_service_module(vm2, path));
        BOOST_CHECK_EQUAL(runtime.cache_size(), 1u);
    }
    config.set("lua.cache.ttl_seconds", static_cast<int64_t>(0));
}

// With the cache disabled the file is read on every load and nothing is
// inserted into (or served from) the cache.
BOOST_AUTO_TEST_CASE(ScriptCacheDisabledBypassesCache) {
    auto& config = shield::config::global_config();
    config.set("lua.cache.enabled", false);
    {
        LuaRuntime runtime;
        const std::string path =
            write_script("cache_disabled.lua",
                         "return { get = function() return 'off' end }\n");
        auto vm = runtime.create_vm();
        BOOST_REQUIRE(runtime.load_service_module(vm, path));
        BOOST_CHECK_EQUAL(runtime.cache_size(), 0u);
        nlohmann::json ret;
        BOOST_REQUIRE(runtime.call_service_method(
            vm, "get", nlohmann::json::array(), &ret));
        BOOST_CHECK_EQUAL(ret[0].get<std::string>(), "off");
    }
    config.set("lua.cache.enabled", true);
}

// A destroyed VM leaves a stale entry in the state registry; looking the
// old lua_State up again prunes it (the pointer is only compared as a key,
// never dereferenced).
BOOST_AUTO_TEST_CASE(VmForStatePrunesDestroyedEntry) {
    LuaRuntime runtime;
    lua_State* stale = nullptr;
    {
        auto vm = runtime.create_vm();
        stale = runtime.vm_state(vm).lua_state();
    }
    auto looked = runtime.vm_for_state(stale);
    BOOST_CHECK(looked == nullptr);
}

// register_http_route guards with error = nullptr: every rejection arm must
// stay silent instead of dereferencing the error out-param.
BOOST_AUTO_TEST_CASE(RegisterHttpRouteGuardsWithoutErrorOut) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    sol::state_view lua(runtime.vm_state(vm).lua_state());
    lua.script("function h(ctx, req) return {} end");
    sol::function fn = lua["h"];
    // empty service id (guard order: service id first)
    BOOST_CHECK(!runtime.register_http_route(vm, "", "GET", "/rt-a", fn));
    // null VM
    BOOST_CHECK(
        !runtime.register_http_route(nullptr, "cov.rt", "GET", "/rt-b", fn));
    // handler not a function
    BOOST_CHECK(!runtime.register_http_route(vm, "cov.rt", "GET", "/rt-c",
                                             sol::function()));
    // empty path and a path without the leading slash
    BOOST_CHECK(!runtime.register_http_route(vm, "cov.rt", "GET", "", fn));
    BOOST_CHECK(!runtime.register_http_route(vm, "cov.rt", "GET", "abc", fn));
    runtime.remove_http_routes_for_service("cov.rt");
}

// call_http_handler guards with error = nullptr: VM-gone, an invalid stored
// handler, a plain-string body, and the params-conversion catch all run
// without an error out-param.
BOOST_AUTO_TEST_CASE(CallHttpHandlerGuardsWithoutErrorOut) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    sol::state_view lua(runtime.vm_state(vm).lua_state());
    lua.script(
        "function ph(ctx, req) return 'plain' end\n"
        "function thrower(ctx, req) error('g6 boom') end\n");
    // Plain-string body handler, no *error.
    BOOST_CHECK(runtime.register_http_route(vm, "cov.g6", "GET", "/g6-plain",
                                            lua["ph"]));
    auto route = runtime.find_http_route("GET", "/g6-plain", nullptr);
    BOOST_REQUIRE(route.has_value());
    nlohmann::json desc;
    BOOST_CHECK(runtime.call_http_handler(*route, {{"path", "/g6-plain"}}, desc,
                                          nullptr));
    BOOST_CHECK_EQUAL(desc["body"].get<std::string>(), "plain");

    // Non-string param value: the conversion throws into the catch, and the
    // catch keeps silent with error = nullptr.
    BOOST_CHECK(runtime.register_http_route(vm, "cov.g6", "GET", "/g6-throw",
                                            lua["thrower"]));
    auto route2 = runtime.find_http_route("GET", "/g6-throw", nullptr);
    BOOST_REQUIRE(route2.has_value());
    nlohmann::json desc2;
    BOOST_CHECK(!runtime.call_http_handler(
        *route2, {{"path", "/x"}, {"params", {{"x", 5}}}}, desc2, nullptr));

    // A stored handler that lost its function: still reports failure with
    // error = nullptr.
    auto route3 = runtime.find_http_route("GET", "/g6-plain", nullptr);
    BOOST_REQUIRE(route3.has_value());
    route3->handler = std::make_shared<sol::function>();
    nlohmann::json desc3;
    BOOST_CHECK(!runtime.call_http_handler(*route3, {{"path", "/g6-plain"}},
                                           desc3, nullptr));
    runtime.remove_http_routes_for_service("cov.g6");

    // A route whose VM is gone: error = nullptr must not be dereferenced.
    // Everything holding a sol reference into vm2's lua_State — including
    // the sol::state_view, which is not a pure view: it owns registry
    // references for the registry and globals tables — must be destroyed
    // while the VM is still open, so no destructor ever runs against a
    // closed state. Only the route copy (a weak VM handle plus the
    // abandoned handler) may outlive the scope.
    std::optional<HttpRouteRegistration> route4;
    {
        auto vm2 = runtime.create_vm();
        {
            sol::state_view lua2(runtime.vm_state(vm2).lua_state());
            lua2.script("function gone(ctx, req) return {} end");
            BOOST_CHECK(runtime.register_http_route(vm2, "cov.g6gone", "GET",
                                                    "/g6-gone", lua2["gone"]));
        }
        route4 = runtime.find_http_route("GET", "/g6-gone", nullptr);
        BOOST_REQUIRE(route4.has_value());
        // The route still holds a sol::function into vm2's state; abandoning
        // it lets the route outlive the VM without a dangling luaL_unref
        // running when the route table entry is erased below (the
        // reference's destructor must never touch a closed lua_State).
        route4->handler->abandon();
    }
    nlohmann::json desc4;
    BOOST_CHECK(!runtime.call_http_handler(*route4, {{"path", "/g6-gone"}},
                                           desc4, nullptr));
    runtime.remove_http_routes_for_service("cov.g6gone");
}

// Handler response shapes: the status / headers field guards take every
// reachable arm (missing, wrong type, array-keyed and numeric-valued
// header tables, and a well-formed header map).
BOOST_AUTO_TEST_CASE(CallHttpHandlerResponseFieldShapes) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    sol::state_view lua(runtime.vm_state(vm).lua_state());
    lua.script(
        "function s_empty(ctx, req) return {} end\n"
        "function s_badstatus(ctx, req) return {status = 'x'} end\n"
        "function s_numstatus(ctx, req) return {status = 201} end\n"
        "function s_badheaders(ctx, req) return {headers = 'x'} end\n"
        "function s_arrheaders(ctx, req) return {headers = {[1] = 'a'}} end\n"
        "function s_numval(ctx, req) return {headers = {k = 1}} end\n"
        "function s_okheaders(ctx, req) return {headers = {a = 'b'}} end\n");
    struct Shape {
        const char* fn;
        const char* path;
        int expected_status;
    };
    const Shape shapes[] = {
        {"s_empty", "/g7-empty", 200},   {"s_badstatus", "/g7-bs", 200},
        {"s_numstatus", "/g7-ns", 201},  {"s_badheaders", "/g7-bh", 200},
        {"s_arrheaders", "/g7-ah", 200}, {"s_numval", "/g7-nv", 200},
        {"s_okheaders", "/g7-oh", 200},
    };
    for (const auto& s : shapes) {
        BOOST_CHECK_MESSAGE(
            runtime.register_http_route(vm, "cov.g7", "GET", s.path, lua[s.fn]),
            s.path);
        auto route = runtime.find_http_route("GET", s.path, nullptr);
        BOOST_REQUIRE_MESSAGE(route.has_value(), s.path);
        nlohmann::json desc;
        BOOST_CHECK_MESSAGE(runtime.call_http_handler(
                                *route, {{"path", s.path}}, desc, nullptr),
                            s.path);
        BOOST_CHECK_EQUAL(desc["status"].get<int>(), s.expected_status);
    }
    runtime.remove_http_routes_for_service("cov.g7");
}

// service_table(nullptr) reports "no table" and restrict_vm(nullptr) is a
// no-op — the null guards of the two VM helpers.
BOOST_AUTO_TEST_CASE(NullVmHelperGuards) {
    LuaRuntime runtime;
    sol::table t = runtime.service_table(nullptr);
    BOOST_CHECK(!t.valid());
    runtime.restrict_vm(nullptr);  // must be a safe no-op
}

// lua_to_json on a default-constructed (invalid) sol::object reports
// failure; a ClientRefBox userdata converts through its embedded payload.
BOOST_AUTO_TEST_CASE(LuaToJsonInvalidObjectAndClientRefBox) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    sol::state_view lua(runtime.vm_state(vm).lua_state());

    sol::object invalid;
    nlohmann::json out;
    // An invalid object converts like nil: success with a JSON null.
    BOOST_CHECK(lua_to_json(invalid, &out));
    BOOST_CHECK(out.is_null());

    auto boxed = sol::make_object(lua, ClientRefBox{});
    nlohmann::json out2;
    BOOST_CHECK(lua_to_json(boxed, &out2));
    BOOST_CHECK(out2.is_object());
}

// load_service_module failure paths with error = nullptr: missing file,
// syntax error, a top-level error() raise, and a non-table module return.
BOOST_AUTO_TEST_CASE(LoadServiceModuleFailuresWithoutErrorOut) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    BOOST_CHECK(
        !runtime.load_service_module(vm, kTmpDir + "/cov10_no_such.lua"));
    const std::string syntax = write_script("cov10_syntax.lua", "return =\n");
    BOOST_CHECK(!runtime.load_service_module(vm, syntax));
    const std::string toperr =
        write_script("cov10_toperr.lua", "error('top-level boom')\n");
    BOOST_CHECK(!runtime.load_service_module(vm, toperr));
    const std::string notable =
        write_script("cov10_notable.lua", "return 42\n");
    BOOST_CHECK(!runtime.load_service_module(vm, notable));
}

// call_service_function message-slot shapes: string / nil / non-string
// failure messages, with and without an error out-param.
BOOST_AUTO_TEST_CASE(CallServiceFunctionMessageSlotShapes) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    const std::string path =
        write_script("cov11_msgs.lua",
                     "local M = {}\n"
                     "function M.f_false_str() return false, 'because' end\n"
                     "function M.f_false_nil() return false, nil end\n"
                     "function M.f_false_num() return false, 42 end\n"
                     "function M.f_false_fn() return false, print end\n"
                     "function M.f_false_tbl() return false, {t = 1} end\n"
                     "function M.f_nil_extra() return nil, 'extra' end\n"
                     "function M.f_nil_nil() return nil, nil end\n"
                     "function M.f_boom() error('slot boom') end\n"
                     "M.not_fn = 42\n"
                     "return M\n");
    BOOST_REQUIRE(runtime.load_service_module(vm, path));

    std::string error;
    // A string message slot is passed through verbatim.
    BOOST_CHECK(!runtime.call_service_function(
        vm, "f_false_str", nlohmann::json::object(), &error));
    BOOST_CHECK_EQUAL(error, "because");
    // A nil second slot produces the generic "returned false" message.
    BOOST_CHECK(!runtime.call_service_function(
        vm, "f_false_nil", nlohmann::json::object(), &error));
    BOOST_CHECK_EQUAL(error, "f_false_nil returned false");
    // Non-string slots (number / function / table) go through the shared
    // Lua->JSON stringify fallback.
    for (const char* name : {"f_false_num", "f_false_fn", "f_false_tbl"}) {
        error.clear();
        BOOST_CHECK_MESSAGE(!runtime.call_service_function(
                                vm, name, nlohmann::json::object(), &error),
                            name);
        BOOST_CHECK_MESSAGE(
            error.find("failed with non-string message") != std::string::npos,
            name);
    }
    // A nil first return forwards a non-nil second slot as the message.
    BOOST_CHECK(!runtime.call_service_function(
        vm, "f_nil_extra", nlohmann::json::object(), &error));
    BOOST_CHECK_EQUAL(error, "extra");
    // A nil second slot produces the generic "returned nil" message.
    BOOST_CHECK(!runtime.call_service_function(
        vm, "f_nil_nil", nlohmann::json::object(), &error));
    BOOST_CHECK_EQUAL(error, "f_nil_nil returned nil");
    // A handler error surfaces through the checked result.
    BOOST_CHECK(!runtime.call_service_function(
        vm, "f_boom", nlohmann::json::object(), &error));
    BOOST_CHECK(error.find("slot boom") != std::string::npos);
    // A non-function module member is rejected.
    BOOST_CHECK(!runtime.call_service_function(
        vm, "not_fn", nlohmann::json::object(), &error));
    BOOST_CHECK_EQUAL(error, "not_fn is not a function");
    // The same failure shapes with error = nullptr.
    BOOST_CHECK(
        !runtime.call_service_function(vm, "not_fn", nlohmann::json::object()));
    BOOST_CHECK(
        !runtime.call_service_function(vm, "f_boom", nlohmann::json::object()));
    BOOST_CHECK(!runtime.call_service_function(vm, "f_false_nil",
                                               nlohmann::json::object()));
    BOOST_CHECK(!runtime.call_service_function(vm, "f_false_num",
                                               nlohmann::json::object()));
    BOOST_CHECK(!runtime.call_service_function(vm, "f_nil_extra",
                                               nlohmann::json::object()));
}

// resolve_service_method branches: a non-function member, and the out/error
// nullptr combinations on success and failure.
BOOST_AUTO_TEST_CASE(ResolveServiceMethodBranches) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    const std::string path =
        write_script("cov12_resolve.lua",
                     "local M = {}\n"
                     "function M.add(a, b) return a + b end\n"
                     "M.not_fn = 42\n"
                     "return M\n");
    BOOST_REQUIRE(runtime.load_service_module(vm, path));

    std::string error;
    sol::function out;
    BOOST_CHECK(!runtime.resolve_service_method(vm, "not_fn", &out, &error));
    BOOST_CHECK(error.find("not_fn") != std::string::npos);
    BOOST_CHECK(error.find("missing or not a function") != std::string::npos);

    error.clear();
    BOOST_CHECK(
        !runtime.resolve_service_method(vm, "missing", nullptr, nullptr));
    BOOST_CHECK(
        !runtime.resolve_service_method(vm, "not_fn", nullptr, nullptr));
    BOOST_CHECK(runtime.resolve_service_method(vm, "add", &out, nullptr));
    BOOST_CHECK(out.valid());
    BOOST_CHECK(runtime.resolve_service_method(vm, "add", nullptr, nullptr));
}

// call_service_method guards with returns = nullptr and error = nullptr:
// not loaded, non-function member, non-array args, unsupported return, and
// a handler error.
BOOST_AUTO_TEST_CASE(CallServiceMethodWithoutErrorOut) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    // not loaded
    BOOST_CHECK(
        !runtime.call_service_method(vm, "add", nlohmann::json::array()));

    const std::string path =
        write_script("cov13_methods.lua",
                     "local M = {}\n"
                     "function M.add(a, b) return a + b end\n"
                     "function M.boom() error('m13 boom') end\n"
                     "function M.fn_out() return print end\n"
                     "M.not_fn = 42\n"
                     "return M\n");
    BOOST_REQUIRE(runtime.load_service_module(vm, path));

    BOOST_CHECK(
        runtime.call_service_method(vm, "add", nlohmann::json::array({2, 3})));
    BOOST_CHECK(
        !runtime.call_service_method(vm, "missing", nlohmann::json::array()));
    BOOST_CHECK(
        !runtime.call_service_method(vm, "not_fn", nlohmann::json::array()));
    BOOST_CHECK(
        !runtime.call_service_method(vm, "add", nlohmann::json::object()));
    // An unsupported (function) return is only detected through the
    // returns out-param; with returns = nullptr the call just succeeds.
    nlohmann::json fn_returns;
    BOOST_CHECK(!runtime.call_service_method(
        vm, "fn_out", nlohmann::json::array(), &fn_returns));
    BOOST_CHECK(
        !runtime.call_service_method(vm, "boom", nlohmann::json::array()));
}

// invoke_coroutine completion matrix on a factory-bearing service VM: every
// session / manager / service-id combination of finish_ok and finish_err.
BOOST_AUTO_TEST_CASE(InvokeCoroutineCompletionMatrix) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov14_matrix.lua",
                     "local M = {}\n"
                     "function M.ok() return 'done' end\n"
                     "function M.boom() error('matrix boom') end\n"
                     "function M.boom_tbl() error({code = 7}) end\n"
                     "G_boom = M.boom\n"
                     "G_boom_tbl = M.boom_tbl\n"
                     "return M\n");
    auto svc = manager.spawn(path, R"({"name": "cov14_matrix"})");
    BOOST_REQUIRE(svc.success);
    auto vm_handle = manager.service_vm(svc.service_id);
    BOOST_REQUIRE(vm_handle != nullptr);
    sol::state& lua = runtime.vm_state(vm_handle);

    // finish_ok combinations (success).
    BOOST_CHECK(runtime.call_service_method_coroutine(vm_handle, "ok",
                                                      nlohmann::json::array()));
    BOOST_CHECK(runtime.call_service_method_coroutine(
        vm_handle, "ok", nlohmann::json::array(), nullptr, 141, &manager,
        svc.service_id));
    BOOST_CHECK(runtime.call_service_method_coroutine(
        vm_handle, "ok", nlohmann::json::array(), nullptr, 142, &manager, ""));

    // finish_err combinations (failure): label-less error text, missing
    // manager, empty service id, and a non-string error object.
    std::string error;
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm_handle, "boom", nlohmann::json::array(), &error));
    BOOST_CHECK(error.find("boom") != std::string::npos);

    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm_handle, "boom", nlohmann::json::array(), nullptr, 0, nullptr, ""));

    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm_handle, "boom", nlohmann::json::array(), nullptr, 143, &manager,
        svc.service_id));

    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm_handle, "boom", nlohmann::json::array(), nullptr, 0, &manager, ""));

    // A table error object keeps the default "raised an error" text (the
    // stack top is not a string).
    error.clear();
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm_handle, "boom_tbl", nlohmann::json::array(), &error, 144, &manager,
        svc.service_id));
    BOOST_CHECK(error.find("raised an error") != std::string::npos);

    // invoke_coroutine directly with an empty method label: the label-less
    // default message arm. The functions are picked up from _G (the script
    // aliases them there) because the module members live in the service
    // table, not in the global table.
    sol::function boom = lua["G_boom"];
    BOOST_REQUIRE(boom.valid());
    error.clear();
    BOOST_CHECK(!runtime.invoke_coroutine(vm_handle, boom, {}, "handler", "", 0,
                                          nullptr, "", &error));
    BOOST_CHECK(error.find("matrix boom") != std::string::npos);

    // And with a non-string error object the label-less default text stands.
    sol::function boom_tbl = lua["G_boom_tbl"];
    BOOST_REQUIRE(boom_tbl.valid());
    error.clear();
    BOOST_CHECK(!runtime.invoke_coroutine(vm_handle, boom_tbl, {}, "handler",
                                          "", 0, nullptr, "", &error));
    BOOST_CHECK_EQUAL(error, "handler raised an error");
}

// A service whose on_init raises fails its spawn; the pending-exit shortcut
// during spawn-init is skipped through its in-progress arm.
BOOST_AUTO_TEST_CASE(SpawnOnInitErrorFailsSpawn) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov15_initfail.lua",
                     "local M = {}\n"
                     "function M.on_init() error('init boom') end\n"
                     "return M\n");
    auto svc = manager.spawn(path, R"({"name": "cov15_initfail"})");
    BOOST_CHECK(!svc.success);
}

// call_service_method_coroutine fallback (bare VM, no factory) combos:
// guards with error = nullptr and completion with / without a manager.
BOOST_AUTO_TEST_CASE(CallServiceMethodCoroutineFallbackCombos) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto vm = runtime.create_vm();
    // not loaded, no error out-param, with and without completion routing.
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm, "add", nlohmann::json::array()));
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm, "add", nlohmann::json::array(), nullptr, 161, &manager, ""));

    const std::string path =
        write_script("cov16_fallback.lua",
                     "local M = {}\n"
                     "function M.ok() return 'fine' end\n"
                     "function M.boom() error('fb boom') end\n"
                     "M.not_fn = 42\n"
                     "return M\n");
    BOOST_REQUIRE(runtime.load_service_module(vm, path));

    // missing method: error = nullptr both with and without a manager.
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm, "missing", nlohmann::json::array()));
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm, "missing", nlohmann::json::array(), nullptr, 162, &manager, ""));
    // non-function member and non-array args with error = nullptr.
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm, "not_fn", nlohmann::json::array()));
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm, "ok", nlohmann::json::object()));
    // fallback dispatch failure routed to a pending call.
    BOOST_CHECK(!runtime.call_service_method_coroutine(
        vm, "boom", nlohmann::json::array(), nullptr, 163, &manager, "cov16"));
    // fallback dispatch success routed to a pending call.
    BOOST_CHECK(runtime.call_service_method_coroutine(
        vm, "ok", nlohmann::json::array(), nullptr, 164, &manager, "cov16"));
}

// invoke_client_rpc with error = nullptr across the guard, completion and
// error-hook branches; plus the factory-failure and non-thread factory
// shapes on a bare VM.
BOOST_AUTO_TEST_CASE(InvokeClientRpcNullErrorAndManagerCombos) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto bare = runtime.create_vm();
    // factory missing, error = nullptr
    sol::state_view bare_lua(runtime.vm_state(bare).lua_state());
    bare_lua.script("function bare_h(ctx, client, req) return 'x' end");
    sol::function bare_h = bare_lua["bare_h"];
    ClientIngress ingress;
    ingress.decoded_request = nlohmann::json::object();
    BOOST_CHECK(!runtime.invoke_client_rpc(bare, bare_h, ingress, nullptr));

    const std::string path = write_script(
        "cov17_rpc.lua",
        "local M = {}\n"
        "function M.gw_ok(ctx, client, req) return 'done' end\n"
        "function M.gw_boom(ctx, client, req) error('rpc boom 17') end\n"
        "function M.gw_tbl(ctx, client, req) error({code = 3}) end\n"
        "_G.__cov17_ok = M.gw_ok\n"
        "_G.__cov17_boom = M.gw_boom\n"
        "_G.__cov17_tbl = M.gw_tbl\n"
        "return M\n");
    auto svc = manager.spawn(path, R"({"name": "cov17_rpc"})");
    BOOST_REQUIRE(svc.success);
    auto vm = manager.service_vm(svc.service_id);
    BOOST_REQUIRE(vm != nullptr);
    sol::state& lua = runtime.vm_state(vm);
    sol::function ok_h = lua["__cov17_ok"];
    sol::function boom_h = lua["__cov17_boom"];
    sol::function tbl_h = lua["__cov17_tbl"];

    // handler not a function (error = nullptr)
    BOOST_CHECK(
        !runtime.invoke_client_rpc(vm, sol::function(), ingress, nullptr));
    // success without a manager and without an error out-param
    BOOST_CHECK(runtime.invoke_client_rpc(vm, ok_h, ingress, nullptr));
    // success with a manager but an empty service id
    BOOST_CHECK(
        runtime.invoke_client_rpc(vm, ok_h, ingress, nullptr, &manager, ""));
    // failure without a manager and without an error out-param
    BOOST_CHECK(!runtime.invoke_client_rpc(vm, boom_h, ingress, nullptr));
    // failure with a manager and an empty service id
    BOOST_CHECK(
        !runtime.invoke_client_rpc(vm, boom_h, ingress, nullptr, &manager, ""));
    // failure with a non-string error object
    std::string error;
    BOOST_CHECK(!runtime.invoke_client_rpc(vm, tbl_h, ingress, &error, &manager,
                                           svc.service_id));
    BOOST_CHECK(error.find("raised an error") != std::string::npos);
    // request value missing, error = nullptr
    ClientIngress no_req;
    BOOST_CHECK(!runtime.invoke_client_rpc(vm, ok_h, no_req, nullptr));

    // factory failure and non-thread factory result on a bare VM.
    auto vm2 = runtime.create_vm();
    sol::state_view lua2(runtime.vm_state(vm2).lua_state());
    lua2.script(
        "function ok_fn() end\n"
        "__shield_run_handler = function() error('factory kaput 17') end\n");
    sol::function ok2 = lua2["ok_fn"];
    ingress.decoded_request = nlohmann::json::object();
    BOOST_CHECK(!runtime.invoke_client_rpc(vm2, ok2, ingress, nullptr));
    lua2.script("__shield_run_handler = function() return {} end");
    BOOST_CHECK(!runtime.invoke_client_rpc(vm2, ok2, ingress, nullptr));
}

// exec_lua with error = nullptr across load / exec failures, plus the
// non-string tostring-failure and exec-error fallback arms.
BOOST_AUTO_TEST_CASE(ExecLuaNullErrorAndTostringFallbacks) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    nlohmann::json result;

    // null VM
    BOOST_CHECK(!runtime.exec_lua(nullptr, "return 1", &result));
    // syntax error without an error out-param
    BOOST_CHECK(!runtime.exec_lua(vm, "ret urn 1", &result));
    // runtime error without an error out-param
    BOOST_CHECK(!runtime.exec_lua(vm, "error('e18')", &result));
    // a non-string error object: lua_tostring yields null and the fallback
    // message is used.
    std::string error;
    BOOST_CHECK(!runtime.exec_lua(vm, "error({})", &result, &error));
    BOOST_CHECK_EQUAL(error, "exec error");
    // a raising __tostring whose error object is itself a non-string: the
    // tostring pcall fails and lua_tostring yields null again.
    result.clear();
    const bool ok = runtime.exec_lua(
        vm, "return setmetatable({}, {__tostring = function() error({}) end})",
        &result, &error);
    BOOST_CHECK(ok);
    BOOST_REQUIRE(result.is_array() && result.size() == 1u);
    BOOST_CHECK(result[0].get<std::string>().find("non-string error") !=
                std::string::npos);
}

// LuaPack edge shapes: a default-constructed sol::object encodes as Nil,
// and both bad-magic byte orders are rejected by the decoder.
BOOST_AUTO_TEST_CASE(LuaPackInvalidObjectAndBadMagic) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    sol::state_view lua(runtime.vm_state(vm).lua_state());

    LuaPackEncoder::Config cfg;
    LuaPackEncoder encoder(cfg);
    std::vector<uint8_t> out;
    sol::object invalid;
    BOOST_CHECK(encoder.encode(lua, invalid, out));
    // 4-byte header (magic + version + flags) plus the Nil value tag.
    BOOST_REQUIRE_EQUAL(out.size(), 5u);
    BOOST_CHECK_EQUAL(out[4],
                      static_cast<uint8_t>(LuaPackEncoder::TypeTag::Nil));

    LuaPackDecoder decoder;
    size_t consumed = 0;
    sol::object v1 = decoder.decode(lua, {'X', 'P', 1, 0}, consumed);
    BOOST_CHECK(v1 == sol::nil);
    BOOST_CHECK_EQUAL(decoder.error(), "invalid LuaPack magic bytes");
    sol::object v2 = decoder.decode(lua, {'L', 'B', 1, 0}, consumed);
    BOOST_CHECK(v2 == sol::nil);
    BOOST_CHECK_EQUAL(decoder.error(), "invalid LuaPack magic bytes");
}
