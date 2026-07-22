// [SHIELD_LUA] Tests for AD-07: layered business-time clock (mock time).
//
// Verifies that:
// - shield.now() reads the business-time clock (wall-clock UTC by default).
// - shield.monotonic() reads real monotonic time (not adjustable).
// - os.time() / os.date() (no-arg) are redirected to the business clock.
// - os.time(table) / os.date(fmt, t) / os.clock() are NOT redirected.
// - MockClock can be injected to control time for testing.

#define BOOST_TEST_MODULE LuaApiClockTests
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <ctime>
#include <string>

#include "shield/lua/clock.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";

nlohmann::json opts_for(const std::string& name) {
    return {{"name", name},
            {"args", nlohmann::json::object()},
            {"config", nlohmann::json::object()}};
}

SpawnResult spawn_echo(LuaServiceManager& manager, const std::string& name) {
    return manager.spawn(TEST_SCRIPTS_DIR + "echo.lua", opts_for(name).dump());
}

// Helper: run Lua code in a spawned service, return values via out-param.
bool run_lua(LuaServiceManager& manager, const std::string& sid,
             const std::string& code, nlohmann::json& out) {
    return manager.exec_lua(sid, code, &out);
}

// Helper: run Lua code, expect success, return first value as int64_t.
int64_t lua_int(LuaServiceManager& manager, const std::string& sid,
                const std::string& code) {
    nlohmann::json vals;
    BOOST_REQUIRE(run_lua(manager, sid, code, vals));
    BOOST_REQUIRE(vals.is_array());
    BOOST_REQUIRE_GE(vals.size(), 1u);
    return vals[0].get<int64_t>();
}

// Helper: run Lua code, expect success, return first value as bool.
bool lua_bool(LuaServiceManager& manager, const std::string& sid,
              const std::string& code) {
    nlohmann::json vals;
    BOOST_REQUIRE(run_lua(manager, sid, code, vals));
    BOOST_REQUIRE(vals.is_array());
    BOOST_REQUIRE_GE(vals.size(), 1u);
    return vals[0].get<bool>();
}

// Helper: run Lua code, expect success, return first value as double.
double lua_double(LuaServiceManager& manager, const std::string& sid,
                  const std::string& code) {
    nlohmann::json vals;
    BOOST_REQUIRE(run_lua(manager, sid, code, vals));
    BOOST_REQUIRE(vals.is_array());
    BOOST_REQUIRE_GE(vals.size(), 1u);
    return vals[0].get<double>();
}

// Helper: run Lua code, expect success, return first value as int.
int lua_int32(LuaServiceManager& manager, const std::string& sid,
              const std::string& code) {
    nlohmann::json vals;
    BOOST_REQUIRE(run_lua(manager, sid, code, vals));
    BOOST_REQUIRE(vals.is_array());
    BOOST_REQUIRE_GE(vals.size(), 1u);
    return vals[0].get<int>();
}
}  // namespace

BOOST_AUTO_TEST_SUITE(ClockTests)

// LCLK-01: Default SystemClock — shield.now() ≈ real wall-clock UTC ms.
BOOST_AUTO_TEST_CASE(NowReturnsWallClockUtcMs) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto spawn = spawn_echo(manager, "clk01");
    BOOST_REQUIRE(spawn.success);

    auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    int64_t now_val = lua_int(manager, spawn.service_id, "return shield.now()");
    auto after = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();

    // Allow 500ms tolerance for test execution.
    BOOST_CHECK_GE(now_val, before - 500);
    BOOST_CHECK_LE(now_val, after + 500);
}

// LCLK-02: Default — shield.now()/1000 ≈ os.time() (within 1s tolerance).
BOOST_AUTO_TEST_CASE(NowAndOsTimeAgree) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto spawn = spawn_echo(manager, "clk02");
    BOOST_REQUIRE(spawn.success);

    int64_t now_ms = lua_int(manager, spawn.service_id, "return shield.now()");
    int64_t os_time_s = lua_int(manager, spawn.service_id, "return os.time()");
    int64_t now_s = now_ms / 1000;
    BOOST_CHECK_CLOSE(static_cast<double>(now_s),
                      static_cast<double>(os_time_s), 1.0);
}

// LCLK-03: MockClock injection — shield.now() returns set value.
BOOST_AUTO_TEST_CASE(MockClockSetNow) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto spawn = spawn_echo(manager, "clk03");
    BOOST_REQUIRE(spawn.success);

    auto mock = std::make_shared<MockClock>();
    mock->set_now(1700000000000LL);  // fixed UTC ms
    manager.attach_clock(mock);

    int64_t val = lua_int(manager, spawn.service_id, "return shield.now()");
    BOOST_CHECK_EQUAL(val, 1700000000000LL);
}

// LCLK-04: MockClock advance — shield.now() advances with clock.
BOOST_AUTO_TEST_CASE(MockClockAdvances) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto spawn = spawn_echo(manager, "clk04");
    BOOST_REQUIRE(spawn.success);

    auto mock = std::make_shared<MockClock>();
    mock->set_now(1000000LL);
    manager.attach_clock(mock);

    BOOST_CHECK_EQUAL(lua_int(manager, spawn.service_id, "return shield.now()"),
                      1000000LL);

    mock->advance(500LL);
    BOOST_CHECK_EQUAL(lua_int(manager, spawn.service_id, "return shield.now()"),
                      1000500LL);
}

// LCLK-05: os.time() no-arg reads MockClock.
BOOST_AUTO_TEST_CASE(OsTimeNoArgReadsMockClock) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto spawn = spawn_echo(manager, "clk05");
    BOOST_REQUIRE(spawn.success);

    auto mock = std::make_shared<MockClock>();
    // 1700000000000 ms = 1700000000 s
    mock->set_now(1700000000000LL);
    manager.attach_clock(mock);

    int64_t val = lua_int(manager, spawn.service_id, "return os.time()");
    BOOST_CHECK_EQUAL(val, 1700000000LL);
}

// LCLK-06: os.time(table) is NOT redirected — original conversion preserved.
BOOST_AUTO_TEST_CASE(OsTimeTableNotRedirected) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto spawn = spawn_echo(manager, "clk06");
    BOOST_REQUIRE(spawn.success);

    auto mock = std::make_shared<MockClock>();
    mock->set_now(0);  // If redirected, os.time() would return 0.
    manager.attach_clock(mock);

    // os.time({year=2025,month=1,day=1,hour=0,min=0,sec=0}) should return
    // the real timestamp for that date, not 0.
    int64_t ts =
        lua_int(manager, spawn.service_id,
                "return os.time({year=2025,month=1,day=1,hour=0,min=0,sec=0})");
    // 2025-01-01 00:00:00 UTC ≈ 1735689600
    BOOST_CHECK_GT(ts, 1700000000LL);
}

// LCLK-07: os.date(fmt) no-arg reads MockClock.
BOOST_AUTO_TEST_CASE(OsDateNoArgReadsMockClock) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto spawn = spawn_echo(manager, "clk07");
    BOOST_REQUIRE(spawn.success);

    auto mock = std::make_shared<MockClock>();
    // 2023-11-14 22:13:20 UTC = 1700000000 seconds
    mock->set_now(1700000000000LL);
    manager.attach_clock(mock);

    int year =
        lua_int32(manager, spawn.service_id, "return os.date('!*t').year");
    BOOST_CHECK_EQUAL(year, 2023);
}

// LCLK-08: os.date(fmt, t) with explicit time is NOT redirected.
BOOST_AUTO_TEST_CASE(OsDateWithExplicitTimeNotRedirected) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto spawn = spawn_echo(manager, "clk08");
    BOOST_REQUIRE(spawn.success);

    auto mock = std::make_shared<MockClock>();
    mock->set_now(0);  // would give epoch if redirected
    manager.attach_clock(mock);

    // Pass an explicit time (1735689600 = 2025-01-01 00:00:00 UTC).
    int year = lua_int32(manager, spawn.service_id,
                         "return os.date('!*t', 1735689600).year");
    BOOST_CHECK_EQUAL(year, 2025);
}

// LCLK-09: os.clock() is NOT hooked — still returns real CPU time.
BOOST_AUTO_TEST_CASE(OsClockNotHooked) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto spawn = spawn_echo(manager, "clk09");
    BOOST_REQUIRE(spawn.success);

    auto mock = std::make_shared<MockClock>();
    mock->set_now(99999999999999LL);  // absurd value — os.clock must ignore
    manager.attach_clock(mock);

    double cpu = lua_double(manager, spawn.service_id, "return os.clock()");
    // CPU time is always a small non-negative number (seconds since process
    // start), never the absurd mock value.
    BOOST_CHECK_GE(cpu, 0.0);
    BOOST_CHECK_LT(cpu, 10.0);
}

// LCLK-10: shield.monotonic() is NOT affected by MockClock.
BOOST_AUTO_TEST_CASE(MonotonicNotAdjustable) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto spawn = spawn_echo(manager, "clk10");
    BOOST_REQUIRE(spawn.success);

    auto mock = std::make_shared<MockClock>();
    mock->set_now(0);
    manager.attach_clock(mock);

    auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    int64_t mono =
        lua_int(manager, spawn.service_id, "return shield.monotonic()");
    auto after_mono = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();

    BOOST_CHECK_GE(mono, before);
    BOOST_CHECK_LE(mono, after_mono);
}

// LCLK-11: Business-logic mock test — cache expiry driven by os.time().
// Mirrors the pattern in scripts/shield_database_integration.lua:298:
//   if cache_entry.expires > os.time() then ... end
BOOST_AUTO_TEST_CASE(BusinessLogicRespectsMockTime) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto spawn = spawn_echo(manager, "clk11");
    BOOST_REQUIRE(spawn.success);

    auto mock = std::make_shared<MockClock>();
    mock->set_now(1000000000000LL);  // 1000000000 s
    manager.attach_clock(mock);

    // Setup: create cache entry with expiry = now + 60s.
    int64_t expiry = lua_int(manager, spawn.service_id,
                             "cache_entry = { expires = os.time() + 60 }\n"
                             "return cache_entry.expires");
    BOOST_CHECK_EQUAL(expiry, 1000000000LL + 60LL);

    // Check: before expiry — should be valid (expires > now).
    BOOST_CHECK_EQUAL(lua_bool(manager, spawn.service_id,
                               "return cache_entry.expires > os.time()"),
                      true);

    // Advance clock past expiry.
    mock->advance(61000LL);  // 61 seconds later

    // Check: after expiry — should be expired (expires <= now).
    BOOST_CHECK_EQUAL(lua_bool(manager, spawn.service_id,
                               "return cache_entry.expires > os.time()"),
                      false);
}

// LCLK-12: Full game business logic test via Lua service script.
// Spawns clock_business.lua which exercises cache/daily-checkin/cooldown
// using os.time/shield.now — all redirected to the business clock.
// MockClock is advanced from C++ to drive time-based game logic.
BOOST_AUTO_TEST_CASE(GameBusinessLogicViaLuaScript) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto spawn = manager.spawn(TEST_SCRIPTS_DIR + "clock_business.lua",
                               opts_for("biz").dump());
    BOOST_REQUIRE(spawn.success);
    const auto& sid = spawn.service_id;

    // Fixed start time: 2025-01-15 10:00:00 UTC = 1736938800 seconds.
    auto mock = std::make_shared<MockClock>();
    int64_t start_ms = 1736938800000LL;
    mock->set_now(start_ms);
    manager.attach_clock(mock);

    // --- Cache expiry (os.time) ---
    // Store "player:1" with 60s TTL.
    CallResult r =
        manager.call(sid, "set_cache", R"(["player:1", "data_A", 60])"_json);
    BOOST_REQUIRE(r.success);

    // Read back immediately — should be valid.
    r = manager.call(sid, "get_cache", R"(["player:1"])"_json);
    BOOST_REQUIRE(r.success);
    BOOST_CHECK_EQUAL(r.values[0].get<std::string>(), "data_A");

    // Advance 30s — still valid.
    mock->advance(30000LL);
    r = manager.call(sid, "get_cache", R"(["player:1"])"_json);
    BOOST_REQUIRE(r.success);
    BOOST_CHECK_EQUAL(r.values[0].get<std::string>(), "data_A");

    // Advance another 31s (total 61s) — expired.
    mock->advance(31000LL);
    r = manager.call(sid, "get_cache", R"(["player:1"])"_json);
    BOOST_REQUIRE(r.success);
    BOOST_CHECK(r.values[0].is_null());

    // Cache should be auto-evicted.
    r = manager.call(sid, "cache_size", nlohmann::json::array());
    BOOST_REQUIRE(r.success);
    BOOST_CHECK_EQUAL(r.values[0].get<int>(), 0);

    // --- Daily check-in (os.date) ---
    // First checkin — should succeed.
    r = manager.call(sid, "checkin", nlohmann::json::array());
    BOOST_REQUIRE(r.success);
    BOOST_CHECK_EQUAL(r.values[0].get<bool>(), true);
    BOOST_CHECK_EQUAL(r.values[1].get<std::string>(), "2025-01-15");

    // Same day — already checked in.
    r = manager.call(sid, "checkin", nlohmann::json::array());
    BOOST_REQUIRE(r.success);
    BOOST_CHECK_EQUAL(r.values[0].get<bool>(), false);

    // Advance to next day (2025-01-16 00:00:01 UTC).
    // 2025-01-16 00:00:01 UTC = 1737024001000 ms.
    int64_t next_day_ms = 1737024001000LL;
    mock->set_now(next_day_ms);

    r = manager.call(sid, "checkin", nlohmann::json::array());
    BOOST_REQUIRE(r.success);
    BOOST_CHECK_EQUAL(r.values[0].get<bool>(), true);
    BOOST_CHECK_EQUAL(r.values[1].get<std::string>(), "2025-01-16");

    // --- Cooldown (shield.now) ---
    int64_t cooldown_ms = 5000LL;  // 5-second cooldown.
    // First action — no cooldown yet.
    r = manager.call(sid, "do_action", R"([5000])"_json);
    BOOST_REQUIRE(r.success);
    BOOST_CHECK_EQUAL(r.values[0].get<bool>(), true);

    // Immediately — should be on cooldown.
    r = manager.call(sid, "do_action", R"([5000])"_json);
    BOOST_REQUIRE(r.success);
    BOOST_CHECK_EQUAL(r.values[0].get<bool>(), false);

    // Advance 3s — still on cooldown.
    mock->advance(3000LL);
    r = manager.call(sid, "do_action", R"([5000])"_json);
    BOOST_REQUIRE(r.success);
    BOOST_CHECK_EQUAL(r.values[0].get<bool>(), false);

    // Advance 3s more (total 6s) — cooldown expired.
    mock->advance(3000LL);
    r = manager.call(sid, "do_action", R"([5000])"_json);
    BOOST_REQUIRE(r.success);
    BOOST_CHECK_EQUAL(r.values[0].get<bool>(), true);
}

BOOST_AUTO_TEST_SUITE_END()
