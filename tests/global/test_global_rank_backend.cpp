// GlobalRankBackend tests: in-process leaderboard semantics (score desc /
// uid asc ties, global 1-based ranks, window clamping), GlobalManager
// delegation, backend config selection, and the redis++ backend (one ZSET
// per board) against a self-spawned redis-server. Constructed directly from
// GlobalConfig: no global config store (except the parsing suite), no Lua,
// no bootstrap.
#define BOOST_TEST_MODULE GlobalRankBackendTests
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/global/global_manager.hpp"
#include "shield/global/global_rank_backend.hpp"

using namespace std::chrono_literals;
using shield::global::GlobalConfig;
using shield::global::GlobalManager;
using shield::global::GlobalRankBackend;
using shield::global::RankAround;
using shield::global::RankEntry;
using shield::global::RedisDataBackendConfig;
using shield::global::validate_global_config;

namespace {

bool wait_until(const std::function<bool()>& predicate, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

// ---------------------------------------------------------------------------
// Self-spawned redis-server (same pattern as the data and lock backend
// suites: probe a free port, fork/exec a throwaway server, RAII teardown).
// ---------------------------------------------------------------------------

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int probe_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return -1;
    }
    const int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool tcp_accepts(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    const bool ok =
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

struct RedisServer {
    int port = -1;
    pid_t pid = -1;
    ~RedisServer() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }
};

// Spawn a throwaway redis-server on a random free port; nullptr when the
// binary is unavailable (CI installs redis-server on the global legs).
std::unique_ptr<RedisServer> spawn_redis_server() {
    auto srv = std::make_unique<RedisServer>();
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int port = probe_free_port();
        if (port <= 0) return nullptr;
        const pid_t pid = ::fork();
        if (pid < 0) return nullptr;
        if (pid == 0) {
            const int devnull = ::open("/dev/null", O_WRONLY);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            const auto port_str = std::to_string(port);
            ::execlp("redis-server", "redis-server", "--port", port_str.c_str(),
                     "--save", "", "--appendonly", "no",
                     static_cast<char*>(nullptr));
            _exit(127);
        }
        // Readiness: poll until the port accepts.
        for (int i = 0; i < 100 && !tcp_accepts(port); ++i) {
            std::this_thread::sleep_for(20ms);
        }
        if (tcp_accepts(port)) {
            srv->port = port;
            srv->pid = pid;
            return srv;
        }
        ::kill(pid, SIGTERM);
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
    return nullptr;
}

RedisDataBackendConfig redis_cfg(const RedisServer& srv,
                                 const std::string& prefix) {
    RedisDataBackendConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = srv.port;
    cfg.prefix = prefix;
    return cfg;
}
#endif  // _WIN32

}  // namespace

// ---------------------------------------------------------------------------
// In-process backend (P0 semantics)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(InProcessRankBackendSuite)

BOOST_AUTO_TEST_CASE(RankUpdateScoreAndTieBreak) {
    auto backend = shield::global::make_inprocess_rank_backend();
    BOOST_CHECK(!backend->rank_score("b", "ghost").has_value());
    // Order: score desc; ties break on uid asc.
    backend->rank_update("b", "carol", 30);
    backend->rank_update("b", "alice", 30);
    backend->rank_update("b", "bob", 50);
    backend->rank_update("b", "dave", 10);
    BOOST_CHECK(backend->rank_score("b", "bob").has_value());
    BOOST_CHECK_EQUAL(*backend->rank_score("b", "bob"), 50.0);
    BOOST_CHECK(!backend->rank_score("b", "ghost").has_value());
    BOOST_CHECK_EQUAL(backend->rank_position("b", "bob").value(), 1u);
    BOOST_CHECK_EQUAL(backend->rank_position("b", "alice").value(), 2u);
    BOOST_CHECK_EQUAL(backend->rank_position("b", "carol").value(), 3u);
    BOOST_CHECK_EQUAL(backend->rank_position("b", "dave").value(), 4u);
    BOOST_CHECK(!backend->rank_position("b", "ghost").has_value());
    // Updating a member repositions it.
    backend->rank_update("b", "dave", 99);
    BOOST_CHECK_EQUAL(backend->rank_position("b", "dave").value(), 1u);
    BOOST_CHECK_EQUAL(backend->rank_position("b", "bob").value(), 2u);
}

BOOST_AUTO_TEST_CASE(RankTopAndRangeWindows) {
    auto backend = shield::global::make_inprocess_rank_backend();
    for (int i = 1; i <= 5; ++i) {
        backend->rank_update("b", "u" + std::to_string(i), i);
    }
    // Top ranks carry global positions and clamp to the board size.
    const auto top3 = backend->rank_top("b", 3);
    BOOST_REQUIRE_EQUAL(top3.size(), 3u);
    BOOST_CHECK_EQUAL(top3[0].uid, "u5");
    BOOST_CHECK_EQUAL(top3[0].rank, 1u);
    BOOST_CHECK_EQUAL(top3[2].uid, "u3");
    BOOST_CHECK_EQUAL(top3[2].rank, 3u);
    BOOST_CHECK_EQUAL(backend->rank_top("b", 99).size(), 5u);
    BOOST_CHECK_EQUAL(backend->rank_top("b", 0).size(), 0u);
    // Ranges are 1-based inclusive windows with global ranks.
    const auto range = backend->rank_range("b", 2, 4);
    BOOST_REQUIRE_EQUAL(range.size(), 3u);
    BOOST_CHECK_EQUAL(range[0].uid, "u4");
    BOOST_CHECK_EQUAL(range[0].rank, 2u);
    BOOST_CHECK_EQUAL(range[2].rank, 4u);
    // Invalid windows are empty; out-of-range bounds clamp.
    BOOST_CHECK_EQUAL(backend->rank_range("b", 0, 3).size(), 0u);
    BOOST_CHECK_EQUAL(backend->rank_range("b", 4, 2).size(), 0u);
    BOOST_CHECK_EQUAL(backend->rank_range("b", 4, 99).size(), 2u);
    BOOST_CHECK_EQUAL(backend->rank_range("ghost", 1, 2).size(), 0u);
}

BOOST_AUTO_TEST_CASE(RankAroundWindow) {
    auto backend = shield::global::make_inprocess_rank_backend();
    for (int i = 1; i <= 5; ++i) {
        backend->rank_update("b", "u" + std::to_string(i), i);
    }
    const auto around = backend->rank_around("b", "u3", 1);
    BOOST_REQUIRE(around.target.has_value());
    BOOST_CHECK_EQUAL(around.target->uid, "u3");
    BOOST_CHECK_EQUAL(around.target->rank, 3u);
    BOOST_REQUIRE_EQUAL(around.above.size(), 1u);
    BOOST_CHECK_EQUAL(around.above[0].uid, "u4");
    BOOST_CHECK_EQUAL(around.above[0].rank, 2u);
    BOOST_REQUIRE_EQUAL(around.below.size(), 1u);
    BOOST_CHECK_EQUAL(around.below[0].uid, "u2");
    BOOST_CHECK_EQUAL(around.below[0].rank, 4u);
    // The window clamps at the board edges; every entry keeps global ranks.
    const auto head = backend->rank_around("b", "u5", 2);
    BOOST_CHECK(head.above.empty());
    BOOST_REQUIRE_EQUAL(head.below.size(), 2u);
    BOOST_CHECK_EQUAL(head.below[0].uid, "u4");
    // A missing member yields an empty report.
    const auto missing = backend->rank_around("b", "ghost", 1);
    BOOST_CHECK(!missing.target.has_value());
    BOOST_CHECK(missing.above.empty());
    BOOST_CHECK(missing.below.empty());
}

BOOST_AUTO_TEST_CASE(RankRangeByScoreAndCounts) {
    auto backend = shield::global::make_inprocess_rank_backend();
    backend->rank_update("b", "a", 10);
    backend->rank_update("b", "b", 20);
    backend->rank_update("b", "c", 30);
    // The score window is inclusive on both ends, rank order, global ranks.
    const auto window = backend->rank_range_by_score("b", 10, 20);
    BOOST_REQUIRE_EQUAL(window.size(), 2u);
    BOOST_CHECK_EQUAL(window[0].uid, "b");
    BOOST_CHECK_EQUAL(window[0].rank, 2u);
    BOOST_CHECK_EQUAL(window[1].uid, "a");
    BOOST_CHECK_EQUAL(window[1].rank, 3u);
    BOOST_CHECK_EQUAL(backend->rank_range_by_score("b", 21, 20).size(), 0u);
    BOOST_CHECK_EQUAL(backend->rank_range_by_score("ghost", 0, 99).size(), 0u);
    BOOST_CHECK_EQUAL(backend->rank_count("b"), 3u);
    BOOST_CHECK_EQUAL(backend->rank_count("ghost"), 0u);
    BOOST_CHECK_EQUAL(backend->rank_board_count(), 1u);
    BOOST_CHECK_EQUAL(backend->rank_total_members(), 3u);
    // Remove drops one member; the last removal empties the board out of
    // the non-empty board count.
    BOOST_CHECK(backend->rank_remove("b", "b"));
    BOOST_CHECK(!backend->rank_remove("b", "ghost"));
    BOOST_CHECK(!backend->rank_remove("ghost", "a"));
    BOOST_CHECK_EQUAL(backend->rank_count("b"), 2u);
    backend->rank_clear("b");
    BOOST_CHECK_EQUAL(backend->rank_count("b"), 0u);
    BOOST_CHECK_EQUAL(backend->rank_board_count(), 0u);
    BOOST_CHECK_EQUAL(backend->rank_total_members(), 0u);
    // A second board shows up in the totals.
    backend->rank_update("b2", "x", 1);
    BOOST_CHECK_EQUAL(backend->rank_board_count(), 1u);
    BOOST_CHECK_EQUAL(backend->rank_total_members(), 1u);
}

BOOST_AUTO_TEST_CASE(RankMupdateBulk) {
    auto backend = shield::global::make_inprocess_rank_backend();
    backend->rank_mupdate("b", {{"a", 1.0}, {"b", 2.0}, {"c", 3.0}});
    BOOST_CHECK_EQUAL(backend->rank_count("b"), 3u);
    BOOST_CHECK_EQUAL(*backend->rank_score("b", "b"), 2.0);
    // mupdate overwrites existing members in place.
    backend->rank_mupdate("b", {{"a", 99.0}});
    BOOST_CHECK_EQUAL(*backend->rank_score("b", "a"), 99.0);
    BOOST_CHECK_EQUAL(backend->rank_position("b", "a").value(), 1u);
    // An empty update list touches nothing.
    backend->rank_mupdate("b", {});
    BOOST_CHECK_EQUAL(backend->rank_count("b"), 3u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// GlobalManager delegation: the manager owns a rank backend and routes
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(ManagerDelegationSuite)

BOOST_AUTO_TEST_CASE(UnknownRankBackendFallsBackToProcessMemory) {
    GlobalConfig config;
    config.rank_backend = "bogus";
    GlobalManager gm(config);
    gm.rank_update("b", "u1", 1);
    gm.rank_update("b", "u2", 2);
    BOOST_CHECK_EQUAL(gm.rank_count("b"), 2u);
}

BOOST_AUTO_TEST_CASE(NullRankBackendSwapIsIgnored) {
    GlobalManager gm(GlobalConfig{});
    gm.set_rank_backend(nullptr);
    gm.rank_update("b", "u1", 1);
    BOOST_CHECK_EQUAL(gm.rank_count("b"), 1u);
}

BOOST_AUTO_TEST_CASE(ManagerProcessRanksEndToEnd) {
    GlobalManager gm(GlobalConfig{});
    gm.rank_update("b", "a", 10);
    gm.rank_update("b", "b", 20);
    gm.rank_mupdate("b", {{"c", 30.0}});
    BOOST_CHECK_EQUAL(gm.rank_position("b", "a").value(), 3u);
    const auto top = gm.rank_top("b", 2);
    BOOST_REQUIRE_EQUAL(top.size(), 2u);
    BOOST_CHECK_EQUAL(top[0].uid, "c");
    const auto around = gm.rank_around("b", "b", 1);
    BOOST_REQUIRE(around.target.has_value());
    BOOST_CHECK_EQUAL(around.target->rank, 2u);
    const auto window = gm.rank_range_by_score("b", 10, 20);
    BOOST_REQUIRE_EQUAL(window.size(), 2u);
    BOOST_CHECK_EQUAL(gm.rank_count("b"), 3u);
    BOOST_CHECK_EQUAL(gm.rank_board_count(), 1u);
    BOOST_CHECK_EQUAL(gm.rank_total_members(), 3u);
    BOOST_CHECK(gm.rank_remove("b", "c"));
    gm.rank_clear("b");
    BOOST_CHECK_EQUAL(gm.rank_total_members(), 0u);
}

/// A canned-response fake: the manager must route every rank op into
/// whatever backend is installed. State is shared with the test so the
/// manager owning the backend can never dangle the test's view of it.
struct FakeRankState {
    std::mutex mu;
    std::size_t update_calls = 0;
    std::size_t mupdate_calls = 0;
    std::size_t score_calls = 0;
    std::size_t position_calls = 0;
    std::size_t top_calls = 0;
    std::size_t range_calls = 0;
    std::size_t range_by_score_calls = 0;
    std::size_t around_calls = 0;
    std::size_t count_calls = 0;
    std::size_t remove_calls = 0;
    std::size_t clear_calls = 0;
    std::size_t board_count_calls = 0;
    std::size_t total_members_calls = 0;
    std::optional<double> next_score;
    std::optional<std::uint64_t> next_position;
    RankEntry top_entry{"fake-uid", 7.0, 1};
};

class FakeRankBackend final : public GlobalRankBackend {
public:
    explicit FakeRankBackend(std::shared_ptr<FakeRankState> state)
        : state_(std::move(state)) {}

    void rank_update(const std::string& /*board*/, const std::string& /*uid*/,
                     double /*score*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->update_calls;
    }

    void rank_mupdate(
        const std::string& /*board*/,
        const std::vector<std::pair<std::string, double>>& /*updates*/)
        override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->mupdate_calls;
    }

    std::optional<double> rank_score(const std::string& /*board*/,
                                     const std::string& /*uid*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->score_calls;
        return state_->next_score;
    }

    std::optional<std::uint64_t> rank_position(
        const std::string& /*board*/, const std::string& /*uid*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->position_calls;
        return state_->next_position;
    }

    std::vector<RankEntry> rank_top(const std::string& /*board*/,
                                    std::size_t /*n*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->top_calls;
        return {state_->top_entry};
    }

    std::vector<RankEntry> rank_range(const std::string& /*board*/,
                                      std::uint64_t /*from*/,
                                      std::uint64_t /*to*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->range_calls;
        return {};
    }

    std::vector<RankEntry> rank_range_by_score(const std::string& /*board*/,
                                               double /*lo*/,
                                               double /*hi*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->range_by_score_calls;
        return {};
    }

    RankAround rank_around(const std::string& /*board*/,
                           const std::string& /*uid*/,
                           std::size_t /*n*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->around_calls;
        return {};
    }

    std::size_t rank_count(const std::string& /*board*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->count_calls;
        return 42;
    }

    bool rank_remove(const std::string& /*board*/,
                     const std::string& /*uid*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->remove_calls;
        return true;
    }

    void rank_clear(const std::string& /*board*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->clear_calls;
    }

    std::size_t rank_board_count() override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->board_count_calls;
        return 5;
    }

    std::size_t rank_total_members() override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->total_members_calls;
        return 6;
    }

private:
    std::shared_ptr<FakeRankState> state_;
};

BOOST_AUTO_TEST_CASE(SetRankBackendRoutesEveryOp) {
    GlobalManager gm(GlobalConfig{});
    auto state = std::make_shared<FakeRankState>();
    state->next_score = 7.5;
    state->next_position = 4u;
    gm.set_rank_backend(std::make_unique<FakeRankBackend>(state));

    gm.rank_update("b", "u1", 1);
    BOOST_CHECK_EQUAL(state->update_calls, 1u);
    gm.rank_mupdate("b", {{"u2", 2.0}});
    BOOST_CHECK_EQUAL(state->mupdate_calls, 1u);
    const auto score = gm.rank_score("b", "u1");
    BOOST_REQUIRE(score.has_value());
    BOOST_CHECK_EQUAL(*score, 7.5);
    BOOST_CHECK_EQUAL(state->score_calls, 1u);
    const auto position = gm.rank_position("b", "u1");
    BOOST_REQUIRE(position.has_value());
    BOOST_CHECK_EQUAL(*position, 4u);
    BOOST_CHECK_EQUAL(state->position_calls, 1u);
    const auto top = gm.rank_top("b", 3);
    BOOST_REQUIRE_EQUAL(top.size(), 1u);
    BOOST_CHECK_EQUAL(top[0].uid, "fake-uid");
    BOOST_CHECK_EQUAL(top[0].rank, 1u);
    BOOST_CHECK_EQUAL(state->top_calls, 1u);
    BOOST_CHECK_EQUAL(gm.rank_range("b", 1, 2).size(), 0u);
    BOOST_CHECK_EQUAL(state->range_calls, 1u);
    BOOST_CHECK_EQUAL(gm.rank_range_by_score("b", 0, 9).size(), 0u);
    BOOST_CHECK_EQUAL(state->range_by_score_calls, 1u);
    const auto around = gm.rank_around("b", "u1", 1);
    BOOST_CHECK(!around.target.has_value());
    BOOST_CHECK_EQUAL(state->around_calls, 1u);
    BOOST_CHECK_EQUAL(gm.rank_count("b"), 42u);
    BOOST_CHECK_EQUAL(state->count_calls, 1u);
    BOOST_CHECK(gm.rank_remove("b", "u1"));
    BOOST_CHECK_EQUAL(state->remove_calls, 1u);
    gm.rank_clear("b");
    BOOST_CHECK_EQUAL(state->clear_calls, 1u);
    BOOST_CHECK_EQUAL(gm.rank_board_count(), 5u);
    BOOST_CHECK_EQUAL(state->board_count_calls, 1u);
    BOOST_CHECK_EQUAL(gm.rank_total_members(), 6u);
    BOOST_CHECK_EQUAL(state->total_members_calls, 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Backend selection config
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(BackendConfigSuite)

BOOST_AUTO_TEST_CASE(ParsesRankBackendKeys) {
    auto& cfg = shield::config::global_config();
    cfg.set("global.rank_backend", "redis");
    cfg.set("global.redis.host", "127.0.0.1");
    cfg.set("global.redis.port", 7200);
    GlobalConfig parsed;
    BOOST_REQUIRE(GlobalConfig::from_global_config(&parsed, nullptr));
    BOOST_CHECK_EQUAL(parsed.rank_backend, "redis");
    BOOST_CHECK(validate_global_config(parsed, nullptr));
    // Restore the defaults so later suites parse the stock config.
    cfg.set("global.rank_backend", "");
    cfg.set("global.redis.host", "");
    cfg.set("global.redis.port", 6379);
}

BOOST_AUTO_TEST_CASE(ValidatesUnknownRankBackend) {
    GlobalConfig config;
    config.rank_backend = "etcd";
    std::string error;
    BOOST_CHECK(!validate_global_config(config, &error));
    BOOST_CHECK_NE(error.find("global.rank_backend"), std::string::npos);
    BOOST_CHECK(!validate_global_config(config, nullptr));
    config.rank_backend = "";
    BOOST_CHECK(validate_global_config(config, nullptr));
}

BOOST_AUTO_TEST_CASE(ValidatesRankRedisNeedsHost) {
    GlobalConfig config;
    config.rank_backend = "redis";
    config.redis_host = "";
    std::string error;
    BOOST_CHECK(!validate_global_config(config, &error));
    BOOST_CHECK_NE(error.find("global.redis.host"), std::string::npos);
    BOOST_CHECK(!validate_global_config(config, nullptr));
    config.redis_host = "127.0.0.1";
    BOOST_CHECK(validate_global_config(config, nullptr));
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// redis++ backend against a self-spawned redis-server (Linux/CI legs)
// ---------------------------------------------------------------------------

#ifndef _WIN32

BOOST_AUTO_TEST_SUITE(RedisRankBackendSuite)

BOOST_AUTO_TEST_CASE(RedisRankUpdateScoreBasics) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:grtest:" + std::to_string(::getpid()) + ":basic";
    auto backend =
        shield::global::make_redis_rank_backend(redis_cfg(*srv, prefix));
    BOOST_CHECK(!backend->rank_score("b", "ghost").has_value());
    BOOST_CHECK_EQUAL(backend->rank_count("b"), 0u);
    backend->rank_update("b", "alice", 30);
    backend->rank_update("b", "bob", 50);
    BOOST_CHECK_EQUAL(*backend->rank_score("b", "bob"), 50.0);
    // The write is server-side: a fresh backend handle sees it too.
    auto other =
        shield::global::make_redis_rank_backend(redis_cfg(*srv, prefix));
    BOOST_CHECK_EQUAL(other->rank_count("b"), 2u);
    BOOST_CHECK_EQUAL(*other->rank_score("b", "alice"), 30.0);
    // Updating a member overwrites its score.
    backend->rank_update("b", "alice", 99);
    BOOST_CHECK_EQUAL(*backend->rank_score("b", "alice"), 99.0);
    BOOST_CHECK_EQUAL(backend->rank_count("b"), 2u);
}

BOOST_AUTO_TEST_CASE(RedisRankOrderingAndTieBreak) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:grtest:" + std::to_string(::getpid()) + ":order";
    auto backend =
        shield::global::make_redis_rank_backend(redis_cfg(*srv, prefix));
    backend->rank_update("b", "carol", 30);
    backend->rank_update("b", "alice", 30);
    backend->rank_update("b", "bob", 50);
    backend->rank_update("b", "dave", 10);
    // Score desc, ties break on uid asc (ZSET order).
    BOOST_CHECK_EQUAL(backend->rank_position("b", "bob").value(), 1u);
    BOOST_CHECK_EQUAL(backend->rank_position("b", "alice").value(), 2u);
    BOOST_CHECK_EQUAL(backend->rank_position("b", "carol").value(), 3u);
    BOOST_CHECK_EQUAL(backend->rank_position("b", "dave").value(), 4u);
    backend->rank_update("b", "dave", 99);
    BOOST_CHECK_EQUAL(backend->rank_position("b", "dave").value(), 1u);
    BOOST_CHECK_EQUAL(backend->rank_position("b", "bob").value(), 2u);
}

BOOST_AUTO_TEST_CASE(RedisRankTopAndRangeWindows) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:grtest:" + std::to_string(::getpid()) + ":window";
    auto backend =
        shield::global::make_redis_rank_backend(redis_cfg(*srv, prefix));
    for (int i = 1; i <= 5; ++i) {
        backend->rank_update("b", "u" + std::to_string(i), i);
    }
    const auto top3 = backend->rank_top("b", 3);
    BOOST_REQUIRE_EQUAL(top3.size(), 3u);
    BOOST_CHECK_EQUAL(top3[0].uid, "u5");
    BOOST_CHECK_EQUAL(top3[0].rank, 1u);
    BOOST_CHECK_EQUAL(top3[2].uid, "u3");
    BOOST_CHECK_EQUAL(top3[2].rank, 3u);
    BOOST_CHECK_EQUAL(backend->rank_top("b", 99).size(), 5u);
    BOOST_CHECK_EQUAL(backend->rank_top("b", 0).size(), 0u);
    const auto range = backend->rank_range("b", 2, 4);
    BOOST_REQUIRE_EQUAL(range.size(), 3u);
    BOOST_CHECK_EQUAL(range[0].uid, "u4");
    BOOST_CHECK_EQUAL(range[0].rank, 2u);
    BOOST_CHECK_EQUAL(range[2].rank, 4u);
    BOOST_CHECK_EQUAL(backend->rank_range("b", 0, 3).size(), 0u);
    BOOST_CHECK_EQUAL(backend->rank_range("b", 4, 2).size(), 0u);
    BOOST_CHECK_EQUAL(backend->rank_range("b", 4, 99).size(), 2u);
    BOOST_CHECK_EQUAL(backend->rank_range("ghost", 1, 2).size(), 0u);
}

BOOST_AUTO_TEST_CASE(RedisRankAroundWindow) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:grtest:" + std::to_string(::getpid()) + ":around";
    auto backend =
        shield::global::make_redis_rank_backend(redis_cfg(*srv, prefix));
    for (int i = 1; i <= 5; ++i) {
        backend->rank_update("b", "u" + std::to_string(i), i);
    }
    const auto around = backend->rank_around("b", "u3", 1);
    BOOST_REQUIRE(around.target.has_value());
    BOOST_CHECK_EQUAL(around.target->uid, "u3");
    BOOST_CHECK_EQUAL(around.target->rank, 3u);
    BOOST_REQUIRE_EQUAL(around.above.size(), 1u);
    BOOST_CHECK_EQUAL(around.above[0].uid, "u4");
    BOOST_CHECK_EQUAL(around.above[0].rank, 2u);
    BOOST_REQUIRE_EQUAL(around.below.size(), 1u);
    BOOST_CHECK_EQUAL(around.below[0].uid, "u2");
    BOOST_CHECK_EQUAL(around.below[0].rank, 4u);
    const auto head = backend->rank_around("b", "u5", 2);
    BOOST_CHECK(head.above.empty());
    BOOST_REQUIRE_EQUAL(head.below.size(), 2u);
    BOOST_CHECK_EQUAL(head.below[0].uid, "u4");
    BOOST_CHECK_EQUAL(head.below[0].rank, 2u);
    const auto missing = backend->rank_around("b", "ghost", 1);
    BOOST_CHECK(!missing.target.has_value());
    BOOST_CHECK(missing.above.empty());
    BOOST_CHECK(missing.below.empty());
}

BOOST_AUTO_TEST_CASE(RedisRankRangeByScore) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:grtest:" + std::to_string(::getpid()) + ":byscore";
    auto backend =
        shield::global::make_redis_rank_backend(redis_cfg(*srv, prefix));
    backend->rank_update("b", "a", 10);
    backend->rank_update("b", "b", 20);
    backend->rank_update("b", "c", 30);
    const auto window = backend->rank_range_by_score("b", 10, 20);
    BOOST_REQUIRE_EQUAL(window.size(), 2u);
    BOOST_CHECK_EQUAL(window[0].uid, "b");
    BOOST_CHECK_EQUAL(window[0].rank, 2u);
    BOOST_CHECK_EQUAL(window[1].uid, "a");
    BOOST_CHECK_EQUAL(window[1].rank, 3u);
    BOOST_CHECK_EQUAL(backend->rank_range_by_score("b", 21, 20).size(), 0u);
    BOOST_CHECK_EQUAL(backend->rank_range_by_score("ghost", 0, 99).size(), 0u);
}

BOOST_AUTO_TEST_CASE(RedisRankRemoveClearCounts) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:grtest:" + std::to_string(::getpid()) + ":counts";
    auto backend =
        shield::global::make_redis_rank_backend(redis_cfg(*srv, prefix));
    backend->rank_update("b", "a", 10);
    backend->rank_update("b", "b", 20);
    backend->rank_update("b2", "x", 1);
    BOOST_CHECK_EQUAL(backend->rank_count("b"), 2u);
    BOOST_CHECK_EQUAL(backend->rank_board_count(), 2u);
    BOOST_CHECK_EQUAL(backend->rank_total_members(), 3u);
    BOOST_CHECK(backend->rank_remove("b", "a"));
    BOOST_CHECK(!backend->rank_remove("b", "ghost"));
    BOOST_CHECK(!backend->rank_remove("ghost", "a"));
    BOOST_CHECK_EQUAL(backend->rank_count("b"), 1u);
    // Clear drops the board; the other board keeps its member.
    backend->rank_clear("b");
    BOOST_CHECK_EQUAL(backend->rank_count("b"), 0u);
    BOOST_CHECK_EQUAL(backend->rank_board_count(), 1u);
    BOOST_CHECK_EQUAL(backend->rank_total_members(), 1u);
    // mupdate fills and overwrites through redis too.
    backend->rank_mupdate("b", {{"p", 1.0}, {"q", 2.0}});
    backend->rank_mupdate("b", {{"p", 5.0}});
    BOOST_CHECK_EQUAL(backend->rank_count("b"), 2u);
    BOOST_CHECK_EQUAL(*backend->rank_score("b", "p"), 5.0);
}

BOOST_AUTO_TEST_CASE(RedisRankPrefixIsolation) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string base =
        "shield:grtest:" + std::to_string(::getpid()) + ":iso";
    auto a =
        shield::global::make_redis_rank_backend(redis_cfg(*srv, base + "a"));
    auto b =
        shield::global::make_redis_rank_backend(redis_cfg(*srv, base + "b"));
    a->rank_update("shared", "u1", 1);
    BOOST_CHECK_EQUAL(b->rank_count("shared"), 0u);
    b->rank_update("shared", "u2", 2);
    BOOST_CHECK_EQUAL(a->rank_count("shared"), 1u);
    BOOST_CHECK_EQUAL(*a->rank_score("shared", "u1"), 1.0);
    BOOST_CHECK(!a->rank_score("shared", "u2").has_value());
    BOOST_CHECK_EQUAL(a->rank_board_count(), 1u);
    BOOST_CHECK_EQUAL(b->rank_board_count(), 1u);
}

BOOST_AUTO_TEST_CASE(RedisRankClosedPortThrows) {
    const int closed_port = probe_free_port();
    BOOST_REQUIRE(closed_port > 0);
    RedisDataBackendConfig config;
    config.host = "127.0.0.1";
    config.port = closed_port;
    config.prefix = "shield:grtest:closed";
    auto backend = shield::global::make_redis_rank_backend(config);
    BOOST_CHECK_THROW(backend->rank_update("b", "u", 1), std::exception);
    BOOST_CHECK_THROW(backend->rank_board_count(), std::exception);
}

BOOST_AUTO_TEST_CASE(ManagerWithRedisRankConfigEndToEnd) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    GlobalConfig config;
    config.rank_backend = "redis";
    config.redis_host = "127.0.0.1";
    config.redis_port = srv->port;
    config.redis_prefix =
        "shield:grtest:" + std::to_string(::getpid()) + ":mgr";
    GlobalManager gm(config);

    gm.rank_update("b", "a", 10);
    gm.rank_update("b", "b", 20);
    BOOST_CHECK_EQUAL(gm.rank_position("b", "b").value(), 1u);
    BOOST_CHECK_EQUAL(gm.rank_count("b"), 2u);
    const auto top = gm.rank_top("b", 1);
    BOOST_REQUIRE_EQUAL(top.size(), 1u);
    BOOST_CHECK_EQUAL(top[0].uid, "b");
    BOOST_CHECK(gm.rank_remove("b", "a"));
    gm.rank_clear("b");
    BOOST_CHECK_EQUAL(gm.rank_total_members(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // _WIN32
