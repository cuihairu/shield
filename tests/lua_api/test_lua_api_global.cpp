// LAPI-GL: shield_global P0 — the Lua facades against a real dispatch
// context: global data + local cache round trips, the exclusive/reader/
// writer lock matrices (including bounded acquire waits through
// shield.sleep), the leaderboard surface, normal/delay/reliable queues
// (timeout pops included) and the cron/interval/once scheduler delivering
// through the system-message channel.
//
// When SHIELD_ENABLE_GLOBAL is off, only the stub case compiles: every
// shield_global factory reports module_unavailable instead of being nil.
#define BOOST_TEST_MODULE LuaApiGlobalTests
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {

const std::string GLOBAL_SCRIPT = "../tests/lua_api/scripts/global_service.lua";

nlohmann::json service_opts(const std::string& name) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
}

bool wait_until(std::function<bool()> predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

CallResult call(LuaServiceManager& manager, const std::string& service,
                const std::string& method, nlohmann::json args) {
    return manager.call(service, method, std::move(args), 20000);
}

nlohmann::json sched_calls(LuaServiceManager& manager,
                           const std::string& service) {
    CallResult cr =
        call(manager, service, "sched_calls_json", nlohmann::json::array());
    if (cr.success && !cr.values.empty() && cr.values[0].is_array()) {
        return cr.values[0];
    }
    return nlohmann::json::array();
}

std::size_t sched_call_count(LuaServiceManager& manager,
                             const std::string& service,
                             const std::string& name) {
    std::size_t count = 0;
    for (const auto& entry : sched_calls(manager, service)) {
        if (entry.value("name", "") == name) {
            ++count;
        }
    }
    return count;
}

}  // namespace

#ifdef SHIELD_ENABLE_GLOBAL

#include "shield/global/global_manager.hpp"

namespace {

// Per-case manager singleton wired the way bootstrap wires it: scheduler
// task delivery rides the system-message channel, a gone service drops
// the task.
struct GlobalWorld {
    caf::actor_system_config caf_cfg;
    caf::actor_system system;
    LuaRuntime runtime;
    LuaServiceManager manager;
    shield::global::GlobalConfig config;
    shield::global::GlobalManager gm;

    explicit GlobalWorld(
        shield::global::GlobalConfig cfg = shield::global::GlobalConfig{})
        : system(caf_cfg),
          manager(runtime, system),
          config(std::move(cfg)),
          gm(config) {
        config.scheduler_tick_ms = 10;
        gm.set_task_fire_fn([this](const std::string& service_id,
                                   const std::string& task_name) -> bool {
            if (!manager.service_vm(service_id)) {
                return false;
            }
            std::string error;
            return manager.send_system(service_id, "on_scheduler_task",
                                       nlohmann::json::array({task_name}),
                                       &error);
        });
        gm.start();
        shield::global::GlobalManager::set_global(&gm);
    }
    ~GlobalWorld() {
        shield::global::GlobalManager::set_global(nullptr);
        gm.stop();
    }

    SpawnResult spawn(const std::string& name) {
        return manager.spawn(GLOBAL_SCRIPT, service_opts(name).dump());
    }
};

}  // namespace

// The CAF global meta objects must exist before the first actor_system (the
// GlobalWorld cases) is constructed.
struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

BOOST_AUTO_TEST_SUITE(LapiGlobalData)

BOOST_AUTO_TEST_CASE(LAPI_GL_01_DataFacadeRoundTrip) {
    GlobalWorld world;
    auto svc = world.spawn("gl_data");
    if (!svc.success) BOOST_TEST_MESSAGE("spawn error: " << svc.error_message);
    BOOST_REQUIRE(svc.success);

    CallResult r = call(world.manager, svc.service_id, "data_roundtrip",
                        nlohmann::json::array());
    if (!r.success) BOOST_TEST_MESSAGE("call error: " << r.error_message);
    BOOST_REQUIRE(r.success);
    const nlohmann::json& v = r.values[0];
    BOOST_CHECK_EQUAL(v["ok"], true);
    BOOST_CHECK_EQUAL(v["get_value"]["name"], "ada");
    BOOST_CHECK_EQUAL(v["get_value"]["level"], 3);
    // Lua nil assignments drop the key entirely: absence is the
    // serialized form of a nil read.
    BOOST_CHECK(!v.contains("missing"));
    BOOST_CHECK_EQUAL(v["counter"], 40);
    BOOST_CHECK_EQUAL(v["mget_a"], "1");
    BOOST_CHECK_EQUAL(v["mget_b_x"], true);
    BOOST_CHECK(!v.contains("mget_missing"));
    BOOST_CHECK_EQUAL(v["deleted"], true);
    BOOST_CHECK_EQUAL(v["deleted_again"], false);
    BOOST_CHECK_EQUAL(v["cached_1"], "v");
    BOOST_CHECK_EQUAL(v["cached_3"], "v");
    BOOST_CHECK_EQUAL(v["incr_error"], "invalid_value");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(LapiGlobalLocks)

BOOST_AUTO_TEST_CASE(LAPI_GL_02_ExclusiveLockMatrix) {
    GlobalWorld world;
    // No C++ preset holder: GL_03/GL_04 already cover cross-owner
    // contention, and this matrix expects an uncontended start (try1).
    auto svc = world.spawn("gl_lock");
    BOOST_REQUIRE(svc.success);

    CallResult r = call(world.manager, svc.service_id, "lock_matrix",
                        nlohmann::json::array({true}));
    if (!r.success) BOOST_TEST_MESSAGE("call error: " << r.error_message);
    BOOST_REQUIRE(r.success);
    const nlohmann::json& v = r.values[0];
    BOOST_CHECK_EQUAL(v["try1"], true);
    BOOST_CHECK_EQUAL(v["try2"], true);  // reentrant
    BOOST_CHECK_EQUAL(v["compete"], false);
    // First release drops one of the two reentrant holds; the C++ holder
    // still owns nothing here (different owner), so the second release
    // leaves the lock free.
    BOOST_CHECK_EQUAL(v["release1"], true);
    BOOST_CHECK_EQUAL(v["release2"], true);
    BOOST_CHECK_EQUAL(v["release_unheld"], false);
    // Lock fully released -> lock_info reports nothing -> owner() is nil
    // -> the boolean comparison serializes as false.
    BOOST_CHECK_EQUAL(v["owner_info"], false);
    BOOST_CHECK_EQUAL(v["ttl_unheld"], -1);
    BOOST_CHECK_EQUAL(v["try3"], true);
    BOOST_CHECK_EQUAL(v["extend"], true);
    BOOST_CHECK(v["ttl_held"]);
    BOOST_CHECK_EQUAL(v["with"][0], true);
    BOOST_CHECK_EQUAL(v["with"][1], "ran");
    BOOST_CHECK_EQUAL(v["released_after_with"], false);
    BOOST_CHECK_EQUAL(v["acquire_timeout"], true);
    BOOST_CHECK_EQUAL(v["acquire_after_release"], true);
    // The spinlock registry is independent of the mutex registry.
    BOOST_CHECK_EQUAL(v["spinlock_free"], true);
    BOOST_CHECK_EQUAL(v["spinlock_mutex_independent"], true);
    // Distributed twins (P0: same in-process backend).
    BOOST_CHECK_EQUAL(v["dist_try"], true);
    BOOST_CHECK_EQUAL(v["dist_compete"], false);
    BOOST_CHECK_EQUAL(v["dist_rw_read"], true);
    // The matrix released every hold: the registry is empty again.
    BOOST_CHECK_EQUAL(world.gm.mutex_registry_size("mutex"), 0u);
}

BOOST_AUTO_TEST_CASE(LAPI_GL_03_AcquireWaitsForContendedLock) {
    GlobalWorld world;
    BOOST_CHECK(world.gm.mutex_acquire("mutex", "wait_lock", "cpp-owner", 0) ==
                shield::global::LockStatus::kOk);
    // Free the lock while the Lua acquire loop is polling.
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        world.gm.mutex_release("mutex", "wait_lock", "cpp-owner");
    });
    auto svc = world.spawn("gl_wait");
    BOOST_REQUIRE(svc.success);
    CallResult r = call(world.manager, svc.service_id, "lock_wait",
                        nlohmann::json::array({120}));
    releaser.join();
    if (!r.success) BOOST_TEST_MESSAGE("call error: " << r.error_message);
    BOOST_REQUIRE(r.success);
    const nlohmann::json& v = r.values[0];
    // The acquire eventually succeeded and observed real elapsed time.
    BOOST_CHECK_EQUAL(v["ok"], true);
    BOOST_CHECK(v["waited_ms"].get<std::int64_t>() >= 100);
}

BOOST_AUTO_TEST_CASE(LAPI_GL_04_AcquireTimesOut) {
    GlobalWorld world;
    BOOST_CHECK(world.gm.mutex_acquire("mutex", "wait_lock", "cpp-owner", 0) ==
                shield::global::LockStatus::kOk);
    auto svc = world.spawn("gl_timeout");
    BOOST_REQUIRE(svc.success);
    CallResult r = call(world.manager, svc.service_id, "lock_wait",
                        nlohmann::json::array({80}));
    if (!r.success) BOOST_TEST_MESSAGE("call error: " << r.error_message);
    BOOST_REQUIRE(r.success);
    const nlohmann::json& v = r.values[0];
    BOOST_CHECK_EQUAL(v["ok"], false);
    BOOST_CHECK(v["waited_ms"].get<std::int64_t>() >= 80);
    world.gm.mutex_release("mutex", "wait_lock", "cpp-owner");
}

BOOST_AUTO_TEST_CASE(LAPI_GL_05_RwLockMatrix) {
    GlobalWorld world;
    auto svc = world.spawn("gl_rw");
    BOOST_REQUIRE(svc.success);
    CallResult r = call(world.manager, svc.service_id, "rwlock_matrix",
                        nlohmann::json::array());
    if (!r.success) BOOST_TEST_MESSAGE("call error: " << r.error_message);
    BOOST_REQUIRE(r.success);
    const nlohmann::json& v = r.values[0];
    BOOST_CHECK_EQUAL(v["read_shared"], true);
    // Docs: readers shared, writer exclusive -> readers block the writer.
    BOOST_CHECK_EQUAL(v["write_blocked_by_readers"], true);
    BOOST_CHECK_EQUAL(v["read_release1"], true);
    BOOST_CHECK_EQUAL(v["read_release2"], true);
    BOOST_CHECK_EQUAL(v["write_now_free"], true);
    BOOST_CHECK_EQUAL(v["read_blocked_by_writer"], true);
    BOOST_CHECK_EQUAL(v["write_reentrant"], true);
    BOOST_CHECK_EQUAL(v["write_release1"], true);
    BOOST_CHECK_EQUAL(v["write_release2"], true);
    BOOST_CHECK_EQUAL(v["read_after_write"], true);
    BOOST_CHECK_EQUAL(v["read_with"][0], true);
    BOOST_CHECK_EQUAL(v["read_with"][1], 7);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(LapiGlobalRank)

BOOST_AUTO_TEST_CASE(LAPI_GL_06_RankFacadeMatrix) {
    GlobalWorld world;
    auto svc = world.spawn("gl_rank");
    BOOST_REQUIRE(svc.success);
    CallResult r = call(world.manager, svc.service_id, "rank_matrix",
                        nlohmann::json::array());
    if (!r.success) BOOST_TEST_MESSAGE("call error: " << r.error_message);
    BOOST_REQUIRE(r.success);
    const nlohmann::json& v = r.values[0];
    BOOST_CHECK_EQUAL(v["count"], 4);
    BOOST_CHECK_EQUAL(v["top1"], "p2");
    BOOST_CHECK_EQUAL(v["top1_rank"], 1);
    BOOST_CHECK_EQUAL(v["top1_score"], 1100);
    BOOST_CHECK_EQUAL(v["top2"], "p1");  // tie 1000: uid asc
    BOOST_CHECK_EQUAL(v["position"], 3);
    BOOST_CHECK(!v.contains("position_missing"));
    BOOST_CHECK_EQUAL(v["score"], 1100);
    BOOST_CHECK(!v.contains("score_missing"));
    BOOST_CHECK_EQUAL(v["range1"], "p1");
    BOOST_CHECK_EQUAL(v["range_size"], 2);
    // by_score(950..1050) = p1(1000, rank2), p3(1000, rank3)
    BOOST_CHECK_EQUAL(v["by_score_size"], 2);
    BOOST_CHECK_EQUAL(v["by_score_first"], "p1");
    BOOST_CHECK_EQUAL(v["around_target"], "p2");
    BOOST_CHECK_EQUAL(v["around_above"], 0);
    BOOST_CHECK_EQUAL(v["around_below"], 1);
    BOOST_CHECK_EQUAL(v["around_below_uid"], "p1");
    BOOST_CHECK_EQUAL(v["removed"], true);
    BOOST_CHECK_EQUAL(v["removed_again"], false);
    BOOST_CHECK_EQUAL(v["count_after_remove"], 3);
    BOOST_CHECK_EQUAL(v["count_after_clear"], 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(LapiGlobalQueues)

BOOST_AUTO_TEST_CASE(LAPI_GL_07_QueueFacadeMatrix) {
    GlobalWorld world;
    auto svc = world.spawn("gl_queue");
    BOOST_REQUIRE(svc.success);
    CallResult r = call(world.manager, svc.service_id, "queue_matrix",
                        nlohmann::json::array());
    if (!r.success) BOOST_TEST_MESSAGE("call error: " << r.error_message);
    BOOST_REQUIRE(r.success);
    const nlohmann::json& v = r.values[0];
    BOOST_CHECK(!v.contains("pop_empty"));
    BOOST_CHECK_EQUAL(v["length"], 3);
    BOOST_CHECK_EQUAL(v["pop_a"], "a");
    // The delay queue had one ready entry (push_at in the past) and one
    // late entry; the ready one pops first.
    BOOST_CHECK_EQUAL(v["delay_pending"], 1);
    BOOST_CHECK_EQUAL(v["delay_ready"], 1);
    BOOST_CHECK_EQUAL(v["delay_pop_now"], "now");
    BOOST_CHECK_EQUAL(v["delay_pop_late"], "late");
    BOOST_CHECK_EQUAL(v["pop_b"], "b");
    BOOST_CHECK_EQUAL(v["length_after_purge"], 0);
}

BOOST_AUTO_TEST_CASE(LAPI_GL_08_ReliableQueueMatrix) {
    GlobalWorld world;
    auto svc = world.spawn("gl_reliable");
    BOOST_REQUIRE(svc.success);
    CallResult r = call(world.manager, svc.service_id, "reliable_matrix",
                        nlohmann::json::array());
    if (!r.success) BOOST_TEST_MESSAGE("call error: " << r.error_message);
    BOOST_REQUIRE(r.success);
    const nlohmann::json& v = r.values[0];
    BOOST_CHECK_EQUAL(v["pop_one"], "one");
    BOOST_CHECK(v["handle_id"].is_number());
    BOOST_CHECK_EQUAL(v["ack"], true);
    BOOST_CHECK_EQUAL(v["ack_again"], false);
    BOOST_CHECK_EQUAL(v["nack"], "requeued");
    BOOST_CHECK_EQUAL(v["redelivered"], "two");
    BOOST_CHECK_EQUAL(v["nack_dead"], "dead");
    BOOST_CHECK_EQUAL(v["dead_size"], 1);
    BOOST_CHECK_EQUAL(v["dead_msg"], "two");
    BOOST_CHECK_EQUAL(v["dead_size_after"], 0);
}

BOOST_AUTO_TEST_CASE(LAPI_GL_13_PriorityQueueMatrix) {
    GlobalWorld world;
    auto svc = world.spawn("gl_priority");
    BOOST_REQUIRE(svc.success);
    CallResult r = call(world.manager, svc.service_id, "priority_matrix",
                        nlohmann::json::array());
    if (!r.success) BOOST_TEST_MESSAGE("call error: " << r.error_message);
    BOOST_REQUIRE(r.success);
    const nlohmann::json& v = r.values[0];
    // Smaller priority pops first (urgent=1 < normal=5 < low=10); ties keep
    // FIFO order; the default level is the documented normal (5).
    BOOST_CHECK(!v.contains("pop_empty"));
    BOOST_CHECK_EQUAL(v["length"], 3);
    BOOST_CHECK_EQUAL(v["pop_urgent"], "urgent");
    BOOST_CHECK_EQUAL(v["fifo1"], "normal");
    BOOST_CHECK_EQUAL(v["fifo2"], "n1");
    BOOST_CHECK_EQUAL(v["pop_high"], "high");
    BOOST_CHECK_EQUAL(v["pop_n2"], "n2");
    BOOST_CHECK_EQUAL(v["pop_default"], "default1");
    BOOST_CHECK_EQUAL(v["length_drained"], 1);
    BOOST_CHECK_EQUAL(v["pop_low"], "low");
    BOOST_CHECK(!v.contains("drain_empty"));
    BOOST_CHECK_EQUAL(v["length_after_purge"], 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(LapiGlobalScheduler)

BOOST_AUTO_TEST_CASE(LAPI_GL_09_IntervalDeliversThroughSystemChannel) {
    GlobalWorld world;
    auto svc = world.spawn("gl_sched");
    BOOST_REQUIRE(svc.success);

    CallResult reg = call(world.manager, svc.service_id, "sched_register",
                          nlohmann::json::array({"interval", "beat", 40}));
    BOOST_REQUIRE(reg.success);
    BOOST_CHECK_EQUAL(reg.values[0]["ok"], true);

    // The task fires repeatedly on the service actor via
    // on_scheduler_task -> the per-VM forwarder -> the recorded callback.
    BOOST_CHECK(wait_until(
        [&] {
            return sched_call_count(world.manager, svc.service_id, "beat") >= 3;
        },
        std::chrono::seconds(10)));

    CallResult info = call(world.manager, svc.service_id, "sched_info",
                           nlohmann::json::array({"beat"}));
    BOOST_REQUIRE(info.success);
    BOOST_CHECK_EQUAL(info.values[0]["exists"], true);
    BOOST_CHECK_EQUAL(info.values[0]["type"], "interval");
    BOOST_CHECK_EQUAL(info.values[0]["status"], "active");
    BOOST_CHECK(info.values[0]["run_count"].get<std::int64_t>() >= 3);

    // pause stops the deliveries; resume re-arms from now.
    call(world.manager, svc.service_id, "sched_control",
         nlohmann::json::array({"pause", "beat"}));
    const auto frozen = sched_call_count(world.manager, svc.service_id, "beat");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    BOOST_CHECK_EQUAL(sched_call_count(world.manager, svc.service_id, "beat"),
                      frozen);
    call(world.manager, svc.service_id, "sched_control",
         nlohmann::json::array({"resume", "beat"}));
    BOOST_CHECK(wait_until(
        [&] {
            return sched_call_count(world.manager, svc.service_id, "beat") >
                   frozen;
        },
        std::chrono::seconds(10)));
    BOOST_CHECK_EQUAL(call(world.manager, svc.service_id, "sched_control",
                           nlohmann::json::array({"remove", "beat"}))
                          .values[0],
                      true);
    // The per-VM callback registry drops the entry with the task.
    CallResult count = call(world.manager, svc.service_id, "sched_count",
                            nlohmann::json::array());
    BOOST_REQUIRE(count.success);
    BOOST_CHECK_EQUAL(count.values[0], 0);
}

BOOST_AUTO_TEST_CASE(LAPI_GL_10_CronOnceAndValidation) {
    GlobalWorld world;
    auto svc = world.spawn("gl_sched2");
    BOOST_REQUIRE(svc.success);

    // A one-shot task fires once and reports status "done".
    BOOST_CHECK(call(world.manager, svc.service_id, "sched_register",
                     nlohmann::json::array({"once", "single", 30}))
                    .values[0]["ok"]);
    BOOST_CHECK(wait_until(
        [&] {
            return sched_call_count(world.manager, svc.service_id, "single") >=
                   1;
        },
        std::chrono::seconds(10)));
    CallResult done = call(world.manager, svc.service_id, "sched_info",
                           nlohmann::json::array({"single"}));
    BOOST_REQUIRE(done.success);
    BOOST_CHECK_EQUAL(done.values[0]["status"], "done");
    // trigger fires the callback again without rescheduling.
    BOOST_CHECK_EQUAL(call(world.manager, svc.service_id, "sched_control",
                           nlohmann::json::array({"trigger", "single"}))
                          .values[0],
                      true);
    BOOST_CHECK(wait_until(
        [&] {
            return sched_call_count(world.manager, svc.service_id, "single") >=
                   2;
        },
        std::chrono::seconds(5)));

    // An every-minute cron registers and reports a future next_run.
    BOOST_CHECK(call(world.manager, svc.service_id, "sched_register",
                     nlohmann::json::array({"cron", "tickler", "* * * * *"}))
                    .values[0]["ok"]);
    CallResult info = call(world.manager, svc.service_id, "sched_info",
                           nlohmann::json::array({"tickler"}));
    BOOST_REQUIRE(info.success);
    BOOST_CHECK_EQUAL(info.values[0]["type"], "cron");
    BOOST_CHECK(info.values[0]["next_run"].get<std::int64_t>() >
                shield::global::GlobalManager::now_ms());

    // Validation matrix: bad cron, duplicate name, non-function callback,
    // negative and fractional schedules.
    const std::vector<std::vector<nlohmann::json>> bad_cases = {
        {"cron", "b1", "* * * *"},   {"cron", "b2", "61 * * * *"},
        {"interval", "b3", -5},      {"interval", "b4", 1.5},
        {"interval", "tickler", 10},  // duplicate name
    };
    for (const auto& args : bad_cases) {
        CallResult bad =
            call(world.manager, svc.service_id, "sched_bad_register",
                 nlohmann::json::array({args[0], args[1], args[2], "fn"}));
        BOOST_REQUIRE(bad.success);
        BOOST_CHECK_EQUAL(bad.values[0]["ok"], false);
        BOOST_CHECK_EQUAL(bad.values[0]["code"], "invalid_argument");
    }
    CallResult no_fn =
        call(world.manager, svc.service_id, "sched_bad_register",
             nlohmann::json::array({"interval", "b5", 10, "none"}));
    BOOST_REQUIRE(no_fn.success);
    BOOST_CHECK_EQUAL(no_fn.values[0]["ok"], false);
    BOOST_CHECK_EQUAL(no_fn.values[0]["code"], "invalid_argument");
}

BOOST_AUTO_TEST_CASE(LAPI_GL_11_RateLimiterMatrix) {
    GlobalWorld world;
    auto svc = world.spawn("gl_rate");
    BOOST_REQUIRE(svc.success);
    CallResult r = call(world.manager, svc.service_id, "rate_limiter_matrix",
                        nlohmann::json::array());
    if (!r.success) BOOST_TEST_MESSAGE("call error: " << r.error_message);
    BOOST_REQUIRE(r.success);
    const nlohmann::json& v = r.values[0];
    // Token bucket: burst 5 drained by 5 allows, then denied until refill
    // (rate 10/s makes the deny stable); keys are independent.
    BOOST_CHECK_EQUAL(v["allow1"], true);
    BOOST_CHECK_EQUAL(v["remaining_after4"], 1);
    BOOST_CHECK_EQUAL(v["allow5"], true);
    BOOST_CHECK_EQUAL(v["remaining_after5"], 0);
    BOOST_CHECK_EQUAL(v["allow6"], false);
    BOOST_CHECK_EQUAL(v["other_key"], true);
    // rate 1000/s: a bounded wait succeeds once a token refills.
    BOOST_CHECK_EQUAL(v["wait_ok"], true);
    // Sliding window: exact per-window count, fresh keys start full.
    BOOST_CHECK_EQUAL(v["sliding_remaining"], 1);
    BOOST_CHECK_EQUAL(v["sliding3"], true);
    BOOST_CHECK_EQUAL(v["sliding4"], false);
    BOOST_CHECK_EQUAL(v["sliding_fresh"], 3);
}

BOOST_AUTO_TEST_SUITE_END()

#else  // SHIELD_ENABLE_GLOBAL

// The global-enabled build has no global fixture of its own; the stub build
// needs the CAF global meta objects before the actor_system below can exist.
struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

BOOST_AUTO_TEST_SUITE(LapiGlobalStub)

BOOST_AUTO_TEST_CASE(LAPI_GL_12_StubReportsModuleUnavailable) {
    caf::actor_system_config caf_cfg;
    caf::actor_system system(caf_cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto svc = manager.spawn(GLOBAL_SCRIPT, service_opts("gl_stub").dump());
    if (!svc.success) BOOST_TEST_MESSAGE("spawn error: " << svc.error_message);
    BOOST_REQUIRE(svc.success);

    // Every factory degrades to the stable module_unavailable code.
    CallResult probe =
        call(manager, svc.service_id, "stub_probe", nlohmann::json::array());
    BOOST_REQUIRE(probe.success);
    const nlohmann::json& codes = probe.values[0];
    for (const std::string& name :
         {"global", "mutex", "rwlock", "spinlock", "distributed_mutex",
          "distributed_rwlock", "rank", "queue", "delay_queue",
          "priority_queue", "reliable_queue", "scheduler", "rate_limiter"}) {
        BOOST_CHECK_EQUAL(codes[name].get<std::string>(), "module_unavailable");
    }
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // SHIELD_ENABLE_GLOBAL
