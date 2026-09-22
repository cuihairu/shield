#define BOOST_TEST_MODULE OpsProfileTests
#include <boost/test/unit_test.hpp>
#include <string>
#include <vector>

// lua.hpp supplies the extern "C" block (lua.h alone does not in this tree).
#include <lua.hpp>

#include "shield/lua/profile_sampler.hpp"
#include "shield/lua/profile_session.hpp"

namespace {

using shield::lua::ProfileFrame;
using shield::lua::ProfileSampler;
using shield::lua::ProfileSession;
using shield::lua::ProfileSessionConfig;

ProfileFrame frame(const std::string& what, const std::string& name,
                   const std::string& source, int line, bool tail = false) {
    ProfileFrame f;
    f.what = what;
    f.name = name;
    f.source = source;
    f.line = line;
    f.tail = tail;
    return f;
}

/// Bare VM without sol: enough for hook sampling tests.
struct BareVM {
    lua_State* L;
    BareVM() : L(luaL_newstate()) {
        if (L != nullptr) {
            luaL_openlibs(L);
        }
    }
    ~BareVM() {
        if (L != nullptr) {
            lua_close(L);
        }
    }
};

/// Run `chunk` protected; BOOST-fail on any error.
void do_chunk(lua_State* L, const char* chunk) {
    if (luaL_dostring(L, chunk) != LUA_OK) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
        lua_pop(L, 1);
        BOOST_FAIL("lua chunk failed: " + err);
    }
}

/// True if any node in the aggregation tree is an anonymous Lua function
/// frame. Top-level chunks report what=="main"; a what=="Lua" frame with no
/// inferable name can only come from a coroutine body (coroutine.resume's C
/// frame lives on the resuming stack, not the coroutine's own stack).
bool tree_has_anon_lua_frame(const nlohmann::json& node) {
    if (node["what"] == "Lua" && node["name"] == "?") {
        return true;
    }
    for (const auto& child : node["children"]) {
        if (tree_has_anon_lua_frame(child)) {
            return true;
        }
    }
    return false;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(ops_profile_session)

BOOST_AUTO_TEST_CASE(EmptySessionExportsZeroReport) {
    ProfileSessionConfig cfg;
    cfg.service = "gateway";
    ProfileSession session(cfg);
    const auto report = session.finish_report(0);

    BOOST_TEST(session.finished());
    BOOST_TEST(report["service"] == "gateway");
    BOOST_TEST(report["total_samples"] == 0U);
    BOOST_TEST(report["truncated_frames"] == 0U);
    BOOST_TEST(report["dropped_samples"] == 0U);
    BOOST_TEST(report["frames"].is_array());
    BOOST_TEST(report["frames"].empty());
    // All config mirrors travel with the report.
    BOOST_TEST(report["duration_ms"] == cfg.duration_ms);
    BOOST_TEST(report["interval"] == cfg.interval);
}

BOOST_AUTO_TEST_CASE(SameStackRepeatedSamplesMergeIntoSinglePath) {
    ProfileSession session(ProfileSessionConfig{});
    const std::vector<ProfileFrame> stack = {
        frame("Lua", "workloop", "@gw.lua", 91),
        frame("Lua", "handler", "@gw.lua", 40),
    };
    session.add_sample(stack);
    session.add_sample(stack);
    session.add_sample(stack);

    BOOST_TEST(session.total_samples() == 3U);
    const auto report = session.finish_report(1);
    BOOST_TEST(report["frames"].size() == 1U);
    const auto& leaf = report["frames"][0];
    BOOST_TEST(leaf["name"] == "workloop");
    BOOST_TEST(leaf["hits"] == 3U);
    // pct sums to 100 at the top level with a single path.
    BOOST_TEST(leaf["pct"] == 100.0);
    BOOST_TEST(leaf["children"].size() == 1U);
    BOOST_TEST(leaf["children"][0]["hits"] == 3U);
    BOOST_TEST(leaf["children"][0]["name"] == "handler");
}

BOOST_AUTO_TEST_CASE(SharedPrefixForksIntoDistinctBranches) {
    ProfileSession session(ProfileSessionConfig{});
    // Shared hot frame, two different callees below it.
    session.add_sample({frame("Lua", "hot", "@a.lua", 10),
                        frame("Lua", "fast_path", "@a.lua", 20)});
    session.add_sample({frame("Lua", "hot", "@a.lua", 10),
                        frame("Lua", "slow_path", "@a.lua", 30)});
    session.add_sample({frame("Lua", "hot", "@a.lua", 10),
                        frame("Lua", "fast_path", "@a.lua", 20)});

    const auto report = session.finish_report(1);
    BOOST_TEST(report["frames"].size() == 1U);
    const auto& hot = report["frames"][0];
    BOOST_TEST(hot["hits"] == 3U);
    BOOST_TEST(hot["children"].size() == 2U);
    // Children are exported in hits-descending order: fast_path (2) first.
    BOOST_TEST(hot["children"][0]["name"] == "fast_path");
    BOOST_TEST(hot["children"][0]["hits"] == 2U);
    BOOST_TEST(hot["children"][1]["name"] == "slow_path");
    BOOST_TEST(hot["children"][1]["hits"] == 1U);
}

BOOST_AUTO_TEST_CASE(NameDegradedFramesAggregateUnderQuestionMark) {
    ProfileSession session(ProfileSessionConfig{});
    // Main chunk and tail-call frames have no inferable name (Task 1).
    session.add_sample({frame("main", "", "@main.lua", 7)});
    session.add_sample({frame("Lua", "", "@lib.lua", 3, /*tail=*/true),
                        frame("main", "", "@main.lua", 7)});

    const auto report = session.finish_report(1);
    BOOST_TEST(report["frames"].size() == 2U);  // two distinct leaf frames
    // Equal hits keep insertion order: the bare main-chunk leaf first...
    const auto& main_leaf = report["frames"][0];
    BOOST_TEST(main_leaf["what"] == "main");
    BOOST_TEST(main_leaf["name"] == "?");
    BOOST_TEST(main_leaf["hits"] == 1U);
    BOOST_TEST(main_leaf["children"].empty());
    // ...then the name-degraded tail frame: tail flag kept, name degrades
    // to "?", and the sample's main-chunk frame sits beneath it (different
    // leaf-first prefix, so it is a separate node from the bare leaf).
    const auto& tail_leaf = report["frames"][1];
    BOOST_TEST(tail_leaf["what"] == "Lua");
    BOOST_TEST(tail_leaf["name"] == "?");
    BOOST_TEST(tail_leaf["tail"] == true);
    BOOST_TEST(tail_leaf["children"][0]["what"] == "main");
    BOOST_TEST(tail_leaf["children"][0]["name"] == "?");
}

BOOST_AUTO_TEST_CASE(OversizedStackIsCountedAndTruncatedAtMaxDepth) {
    ProfileSessionConfig cfg;
    cfg.max_depth = 2;
    ProfileSession session(cfg);
    session.add_sample({frame("Lua", "leaf", "@a.lua", 1),
                        frame("Lua", "mid", "@a.lua", 2),
                        frame("Lua", "root", "@a.lua", 3)});

    BOOST_TEST(session.total_samples() == 1U);
    const auto report = session.finish_report(0);
    BOOST_TEST(report["dropped_samples"] == 1U);
    // Only the first max_depth frames made it into the tree.
    const auto& leaf = report["frames"][0];
    BOOST_TEST(leaf["name"] == "leaf");
    BOOST_TEST(leaf["children"][0]["name"] == "mid");
    BOOST_TEST(leaf["children"][0]["children"].empty());
}

BOOST_AUTO_TEST_CASE(NodeBudgetFoldsRemainingFramesWithoutDroppingSamples) {
    ProfileSessionConfig cfg;
    cfg.max_nodes = 2;  // root excluded from the budget
    ProfileSession session(cfg);
    // Sample 1 consumes two nodes (leaf + child); sample 2 shares the leaf
    // but needs a fresh child -> budget hits, remaining frame folds.
    session.add_sample({frame("Lua", "leaf", "@a.lua", 1),
                        frame("Lua", "child_a", "@a.lua", 2)});
    session.add_sample({frame("Lua", "leaf", "@a.lua", 1),
                        frame("Lua", "child_b", "@a.lua", 3)});

    BOOST_TEST(session.total_samples() == 2U);
    const auto report = session.finish_report(0);
    BOOST_TEST(report["truncated_frames"] == 1U);
    BOOST_TEST(report["dropped_samples"] == 0U);
    const auto& leaf = report["frames"][0];
    BOOST_TEST(leaf["hits"] == 2U);
    // child_b never became a node: leaf has only child_a beneath it.
    BOOST_TEST(leaf["children"].size() == 1U);
    BOOST_TEST(leaf["children"][0]["name"] == "child_a");
}

BOOST_AUTO_TEST_CASE(AddSampleAfterFinishIsIgnored) {
    ProfileSession session(ProfileSessionConfig{});
    session.finish_report(0);
    session.add_sample({frame("Lua", "late", "@a.lua", 1)});
    BOOST_TEST(session.total_samples() == 0U);
}

BOOST_AUTO_TEST_CASE(CFrameWithoutLineDegradesButAggregates) {
    ProfileSession session(ProfileSessionConfig{});
    const ProfileFrame c_frame = frame("C", "", "=[C]", -1);
    session.add_sample({c_frame});
    session.add_sample({c_frame});

    const auto report = session.finish_report(0);
    BOOST_TEST(report["frames"].size() == 1U);
    BOOST_TEST(report["frames"][0]["name"] == "?");
    BOOST_TEST(report["frames"][0]["line"] == -1);
    BOOST_TEST(report["frames"][0]["hits"] == 2U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ops_profile_sampler)

/// Task 1 anchor: a count hook with a small interval samples a busy loop,
/// the hot frame is the loop function itself, and uninstall restores the
/// previous (empty) hook so sampling stops.
BOOST_AUTO_TEST_CASE(SamplerRecordsBusyLoopHotspotAndStopsAfterUninstall) {
    BareVM vm;
    BOOST_REQUIRE(vm.L != nullptr);

    ProfileSessionConfig cfg;
    cfg.service = "gw";
    cfg.interval = 100;
    ProfileSession session(cfg);
    ProfileSampler sampler(session, [] { return std::vector<lua_State*>{}; });

    sampler.install(vm.L);
    // Plain assignment call (not `return workloop(...)` — that would be a
    // tail call, whose frame has no inferable name; Task 1 c3).
    do_chunk(vm.L,
             "local function workloop(n) "
             "  local s = 0 "
             "  for i = 1, n do s = s + i end "
             "  return s "
             "end "
             "local r = workloop(500000) "
             "return r");
    BOOST_TEST(session.total_samples() > 0U);

    sampler.uninstall(vm.L);
    const auto total_after_uninstall = session.total_samples();
    do_chunk(vm.L,
             "local function workloop(n) "
             "  local s = 0 "
             "  for i = 1, n do s = s + i end "
             "  return s "
             "end "
             "local r = workloop(500000) "
             "return r");
    BOOST_TEST(session.total_samples() == total_after_uninstall);

    // The hottest leaf is the running loop function: leaf-first order puts
    // workloop at frames[0] with the main chunk beneath it.
    const auto report = session.finish_report(0);
    BOOST_REQUIRE(report["frames"].size() >= 1U);
    const auto& leaf = report["frames"][0];
    BOOST_TEST(leaf["what"] == "Lua");
    BOOST_TEST(leaf["name"] == "workloop");
    const std::string leaf_source = leaf["source"];
    BOOST_TEST(leaf_source.find("[string") == 0U);
}

/// Task 1 b3 anchor: a suspended coroutine can be armed at install time and
/// samples on its next resume — without arming, its bytecode is invisible
/// to the main-state hook.
BOOST_AUTO_TEST_CASE(SamplerArmsSuspendedCoroutineBeforeResume) {
    BareVM vm;
    BOOST_REQUIRE(vm.L != nullptr);

    do_chunk(vm.L,
             "profile_co = coroutine.create(function(n) "
             "  local s = 0 "
             "  for i = 1, n do s = s + i end "
             "  coroutine.yield(s) "
             "  for i = 1, n do s = s + i end "
             "  return s "
             "end) "
             "local ok = coroutine.resume(profile_co, 1000) "
             "assert(ok)");

    // Resolve the suspended coroutine on this (owner) thread.
    lua_getglobal(vm.L, "profile_co");
    lua_State* co = lua_tothread(vm.L, -1);
    lua_pop(vm.L, 1);
    BOOST_REQUIRE(co != nullptr);

    ProfileSessionConfig cfg;
    cfg.service = "gw";
    cfg.interval = 50;
    ProfileSession session(cfg);
    ProfileSampler sampler(session,
                           [co] { return std::vector<lua_State*>{co}; });

    sampler.install(vm.L);
    do_chunk(vm.L,
             "local ok = coroutine.resume(profile_co, 500000) assert(ok)");
    sampler.uninstall(vm.L);

    // Samples came from the coroutine's second half: the leaf is its
    // anonymous body (no inferable name — Task 1), a frame shape the
    // main chunk alone never produces.
    BOOST_TEST(session.total_samples() > 0U);
    const auto report = session.finish_report(0);
    BOOST_REQUIRE(report["frames"].size() >= 1U);
    bool saw_co_frame = false;
    for (const auto& node : report["frames"]) {
        if (tree_has_anon_lua_frame(node)) {
            saw_co_frame = true;
        }
    }
    BOOST_TEST(saw_co_frame);
}

/// Task 1 b1 anchor: Lua 5.5 does not propagate hooks to coroutines created
/// after install. The periodic sweep (kRescanEvery hits) must reach the
/// provider and arm the new coroutine, otherwise its resume goes unsampled.
BOOST_AUTO_TEST_CASE(RescanSweepPicksUpCoroutineCreatedAfterInstall) {
    BareVM vm;
    BOOST_REQUIRE(vm.L != nullptr);

    // Resolve "profile_co" from the Lua registry namespace on each call so
    // the sweep sees the coroutine created after install.
    auto provider = [L = vm.L] {
        lua_getglobal(L, "profile_co");
        lua_State* co = lua_tothread(L, -1);
        lua_pop(L, 1);
        if (co == nullptr) {
            return std::vector<lua_State*>{};
        }
        return std::vector<lua_State*>{co};
    };

    ProfileSessionConfig cfg;
    cfg.service = "gw";
    cfg.interval = 50;
    ProfileSession session(cfg);
    ProfileSampler sampler(session, provider);
    sampler.install(vm.L);

    // Create a NEW coroutine after install (hooks do not propagate — b1),
    // park it at its first yield, burn enough main-state instructions to
    // cross kRescanEvery, then resume it: only the sweep can have armed it.
    do_chunk(vm.L,
             "profile_co = coroutine.create(function(n) "
             "  local s = 0 "
             "  for i = 1, n do s = s + i end "
             "  coroutine.yield(s) "
             "  for i = 1, n do s = s + i end "
             "  return s "
             "end) "
             "local ok = coroutine.resume(profile_co, 1000) "
             "assert(ok)");
    do_chunk(vm.L,
             "local s = 0 "
             "for i = 1, 100000 do s = s + i end");
    do_chunk(vm.L,
             "local ok = coroutine.resume(profile_co, 500000) assert(ok)");

    sampler.uninstall(vm.L);
    BOOST_TEST(session.total_samples() > 0U);
    const auto report = session.finish_report(0);
    // An anonymous Lua function frame can only come from the new
    // coroutine's body — main-chunk sampling reports what=="main".
    bool saw_co_frame = false;
    for (const auto& node : report["frames"]) {
        if (tree_has_anon_lua_frame(node)) {
            saw_co_frame = true;
        }
    }
    BOOST_TEST(saw_co_frame);
}

/// The sweep itself fires: enough hook hits cross kRescanEvery and invoke
/// the provider beyond the initial install-time call.
BOOST_AUTO_TEST_CASE(RescanSweepInvokesCoProvider) {
    BareVM vm;
    BOOST_REQUIRE(vm.L != nullptr);

    int provider_calls = 0;
    ProfileSessionConfig cfg;
    cfg.service = "gw";
    cfg.interval = 50;
    ProfileSession session(cfg);
    ProfileSampler sampler(session, [&] {
        ++provider_calls;
        return std::vector<lua_State*>{};
    });

    sampler.install(vm.L);  // first provider call
    do_chunk(vm.L,
             "local s = 0 "
             "for i = 1, 500000 do s = s + i end");  // >> kRescanEvery hits
    sampler.uninstall(vm.L);

    BOOST_TEST(provider_calls >= 2);
}

BOOST_AUTO_TEST_SUITE_END()
