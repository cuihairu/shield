// GlobalLockBackend tests: in-process lock semantics (lazy TTL takeover,
// reentrant holds, extend arms), GlobalManager delegation, backend config
// selection, and the redis++ backend (Lua-scripted HASH per lock) against a
// self-spawned redis-server. Constructed directly from GlobalConfig: no
// global config store (except the parsing suite), no Lua, no bootstrap.
#define BOOST_TEST_MODULE GlobalLockBackendTests
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/global/global_lock_backend.hpp"
#include "shield/global/global_manager.hpp"

using namespace std::chrono_literals;
using shield::global::GlobalConfig;
using shield::global::GlobalLockBackend;
using shield::global::GlobalManager;
using shield::global::LockInfo;
using shield::global::LockStatus;
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
// Self-spawned redis-server (same pattern as the data backend suite:
// probe a free port, fork/exec a throwaway server, RAII teardown).
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

BOOST_AUTO_TEST_SUITE(InProcessLockBackendSuite)

BOOST_AUTO_TEST_CASE(LockAcquireReleaseBasics) {
    auto backend = shield::global::make_inprocess_lock_backend();
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-a", 0) ==
                LockStatus::kOk);
    // A second owner is busy while the first holds the lock.
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-b", 0) ==
                LockStatus::kBusy);
    // Release by a non-holder fails without touching the entry.
    BOOST_CHECK(backend->mutex_release("mutex", "m1", "owner-b") ==
                LockStatus::kNotOwner);
    // Same-owner reentrant acquire counts up.
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-a", 0) ==
                LockStatus::kOk);
    const auto info = backend->mutex_info("mutex", "m1");
    BOOST_CHECK(info.exists);
    BOOST_CHECK_EQUAL(info.owner, "owner-a");
    BOOST_CHECK_EQUAL(info.count, 2u);
    BOOST_CHECK_EQUAL(info.ttl_remaining_ms, 0u);  // ttl 0 = immortal
    // Releases decrement; the entry disappears at zero.
    BOOST_CHECK(backend->mutex_release("mutex", "m1", "owner-a") ==
                LockStatus::kOk);
    const auto alive = backend->mutex_info("mutex", "m1");
    BOOST_CHECK(alive.exists);
    BOOST_CHECK_EQUAL(alive.count, 1u);
    BOOST_CHECK_EQUAL(alive.owner, "owner-a");
    BOOST_CHECK(backend->mutex_release("mutex", "m1", "owner-a") ==
                LockStatus::kOk);
    BOOST_CHECK(!backend->mutex_info("mutex", "m1").exists);
    // Releasing a released lock fails.
    BOOST_CHECK(backend->mutex_release("mutex", "m1", "owner-a") ==
                LockStatus::kNotOwner);
    // The freed name goes to the next acquirer.
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-b", 0) ==
                LockStatus::kOk);
}

BOOST_AUTO_TEST_CASE(LockTtlLazyTakeover) {
    auto backend = shield::global::make_inprocess_lock_backend();
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-a", 40) ==
                LockStatus::kOk);
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-b", 0) ==
                LockStatus::kBusy);
    // Lazy expiry: the stale holder loses the lock to the next acquirer.
    BOOST_CHECK(wait_until(
        [&] {
            return backend->mutex_acquire("mutex", "m1", "owner-b", 0) ==
                   LockStatus::kOk;
        },
        3000));
    // Takeover resets the hold count and mints a fresh holder.
    const auto info = backend->mutex_info("mutex", "m1");
    BOOST_CHECK(info.exists);
    BOOST_CHECK_EQUAL(info.owner, "owner-b");
    BOOST_CHECK_EQUAL(info.count, 1u);
    // The previous holder no longer owns the name.
    BOOST_CHECK(backend->mutex_release("mutex", "m1", "owner-a") ==
                LockStatus::kNotOwner);
}

BOOST_AUTO_TEST_CASE(LockExtendRefreshesExpiry) {
    auto backend = shield::global::make_inprocess_lock_backend();
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-a", 200) ==
                LockStatus::kOk);
    BOOST_CHECK(backend->mutex_info("mutex", "m1").ttl_remaining_ms > 0);
    // The holder pushes the deadline out; the short first ttl no longer bites.
    BOOST_CHECK(backend->mutex_extend("mutex", "m1", "owner-a", 3000));
    std::this_thread::sleep_for(400ms);
    const auto info = backend->mutex_info("mutex", "m1");
    BOOST_CHECK(info.exists);
    BOOST_CHECK(info.ttl_remaining_ms > 0);
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-b", 0) ==
                LockStatus::kBusy);
    // Extend arms: a non-holder and a missing lock both fail.
    BOOST_CHECK(!backend->mutex_extend("mutex", "m1", "owner-b", 1000));
    BOOST_CHECK(!backend->mutex_extend("mutex", "ghost", "owner-a", 1000));
    // An expired-but-registered lock no longer extends: it is dead.
    BOOST_CHECK(backend->mutex_acquire("mutex", "m2", "owner-a", 40) ==
                LockStatus::kOk);
    std::this_thread::sleep_for(80ms);
    BOOST_CHECK(!backend->mutex_extend("mutex", "m2", "owner-a", 1000));
    // Extend with ttl 0 clears the deadline: the lock becomes immortal.
    BOOST_CHECK(backend->mutex_extend("mutex", "m1", "owner-a", 0));
    std::this_thread::sleep_for(80ms);
    BOOST_CHECK_EQUAL(backend->mutex_info("mutex", "m1").ttl_remaining_ms, 0u);
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-b", 0) ==
                LockStatus::kBusy);
}

BOOST_AUTO_TEST_CASE(LockInfoShapeAndMissingLocks) {
    auto backend = shield::global::make_inprocess_lock_backend();
    const auto missing = backend->mutex_info("mutex", "ghost");
    BOOST_CHECK(!missing.exists);
    BOOST_CHECK(missing.owner.empty());
    BOOST_CHECK_EQUAL(missing.count, 0u);
    BOOST_CHECK_EQUAL(missing.acquired_at_ms, 0u);
    BOOST_CHECK_EQUAL(missing.ttl_remaining_ms, 0u);
    // A live lock carries owner/count/acquired_at and a positive ttl.
    const auto before = GlobalManager::now_ms();
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-a", 5000) ==
                LockStatus::kOk);
    const auto info = backend->mutex_info("mutex", "m1");
    BOOST_CHECK(info.exists);
    BOOST_CHECK_EQUAL(info.owner, "owner-a");
    BOOST_CHECK_EQUAL(info.count, 1u);
    BOOST_CHECK(info.acquired_at_ms >= before);
    BOOST_CHECK(info.ttl_remaining_ms > 0);
    BOOST_CHECK(info.ttl_remaining_ms <= 5000u);
}

BOOST_AUTO_TEST_CASE(LockRegistryIsolationAndSizes) {
    auto backend = shield::global::make_inprocess_lock_backend();
    BOOST_CHECK_EQUAL(backend->mutex_registry_size("mutex"), 0u);
    BOOST_CHECK_EQUAL(backend->mutex_registry_size("spinlock"), 0u);
    // The same name in different registries is a different lock.
    BOOST_CHECK(backend->mutex_acquire("mutex", "shared-name", "o1", 0) ==
                LockStatus::kOk);
    BOOST_CHECK(backend->mutex_acquire("spinlock", "shared-name", "o2", 0) ==
                LockStatus::kOk);
    BOOST_CHECK_EQUAL(backend->mutex_registry_size("mutex"), 1u);
    BOOST_CHECK_EQUAL(backend->mutex_registry_size("spinlock"), 1u);
    // Unknown registry names share the mutex table (facade behavior).
    BOOST_CHECK(backend->mutex_acquire("distributed_mutex", "other", "o3", 0) ==
                LockStatus::kOk);
    BOOST_CHECK_EQUAL(backend->mutex_registry_size("mutex"), 2u);
    BOOST_CHECK_EQUAL(backend->mutex_registry_size("spinlock"), 1u);
    BOOST_CHECK(backend->mutex_release("mutex", "shared-name", "o1") ==
                LockStatus::kOk);
    BOOST_CHECK(backend->mutex_release("spinlock", "shared-name", "o2") ==
                LockStatus::kOk);
    BOOST_CHECK(backend->mutex_release("distributed_mutex", "other", "o3") ==
                LockStatus::kOk);
    BOOST_CHECK_EQUAL(backend->mutex_registry_size("mutex"), 0u);
    BOOST_CHECK_EQUAL(backend->mutex_registry_size("spinlock"), 0u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// GlobalManager delegation: the manager owns a lock backend and routes
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(ManagerDelegationSuite)

BOOST_AUTO_TEST_CASE(UnknownLockBackendFallsBackToProcessMemory) {
    GlobalConfig config;
    config.lock_backend = "bogus";
    GlobalManager gm(config);
    BOOST_CHECK(gm.mutex_acquire("mutex", "m1", "o1", 0) == LockStatus::kOk);
    BOOST_CHECK(gm.mutex_acquire("mutex", "m1", "o2", 0) == LockStatus::kBusy);
    BOOST_CHECK_EQUAL(gm.mutex_registry_size("mutex"), 1u);
}

BOOST_AUTO_TEST_CASE(NullLockBackendSwapIsIgnored) {
    GlobalManager gm(GlobalConfig{});
    gm.set_lock_backend(nullptr);
    BOOST_CHECK(gm.mutex_acquire("mutex", "m1", "o1", 0) == LockStatus::kOk);
    BOOST_CHECK(gm.mutex_release("mutex", "m1", "o1") == LockStatus::kOk);
}

BOOST_AUTO_TEST_CASE(ManagerProcessLocksEndToEnd) {
    GlobalManager gm(GlobalConfig{});
    BOOST_CHECK(gm.mutex_acquire("mutex", "m1", "o1", 0) == LockStatus::kOk);
    BOOST_CHECK(gm.mutex_acquire("spinlock", "m1", "o1", 0) == LockStatus::kOk);
    BOOST_CHECK(gm.mutex_acquire("mutex", "m1", "o2", 0) == LockStatus::kBusy);
    const auto info = gm.mutex_info("mutex", "m1");
    BOOST_CHECK(info.exists);
    BOOST_CHECK_EQUAL(info.owner, "o1");
    BOOST_CHECK_EQUAL(gm.mutex_registry_size("mutex"), 1u);
    BOOST_CHECK_EQUAL(gm.mutex_registry_size("spinlock"), 1u);
    BOOST_CHECK(gm.mutex_extend("mutex", "m1", "o1", 1000));
    BOOST_CHECK(gm.mutex_release("mutex", "m1", "o1") == LockStatus::kOk);
    BOOST_CHECK(!gm.mutex_info("mutex", "m1").exists);
    BOOST_CHECK_EQUAL(gm.mutex_registry_size("mutex"), 0u);
}

/// A canned-response fake: the manager must route every lock op into
/// whatever backend is installed. State is shared with the test so the
/// manager owning the backend can never dangle the test's view of it.
struct FakeLockState {
    std::mutex mu;
    std::size_t acquire_calls = 0;
    std::size_t release_calls = 0;
    std::size_t extend_calls = 0;
    std::size_t info_calls = 0;
    std::size_t size_calls = 0;
    LockStatus next_acquire = LockStatus::kOk;
    LockStatus next_release = LockStatus::kOk;
    bool next_extend = true;
    LockInfo info{};
};

class FakeLockBackend final : public GlobalLockBackend {
public:
    explicit FakeLockBackend(std::shared_ptr<FakeLockState> state)
        : state_(std::move(state)) {}

    LockStatus mutex_acquire(const std::string& /*registry*/,
                             const std::string& /*name*/,
                             const std::string& /*owner*/,
                             std::uint64_t /*ttl_ms*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->acquire_calls;
        return state_->next_acquire;
    }

    LockStatus mutex_release(const std::string& /*registry*/,
                             const std::string& /*name*/,
                             const std::string& /*owner*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->release_calls;
        return state_->next_release;
    }

    bool mutex_extend(const std::string& /*registry*/,
                      const std::string& /*name*/, const std::string& /*owner*/,
                      std::uint64_t /*ttl_ms*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->extend_calls;
        return state_->next_extend;
    }

    LockInfo mutex_info(const std::string& /*registry*/,
                        const std::string& /*name*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->info_calls;
        return state_->info;
    }

    std::size_t mutex_registry_size(const std::string& /*registry*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        ++state_->size_calls;
        return 7;
    }

private:
    std::shared_ptr<FakeLockState> state_;
};

BOOST_AUTO_TEST_CASE(SetLockBackendRoutesEveryOp) {
    GlobalManager gm(GlobalConfig{});
    auto state = std::make_shared<FakeLockState>();
    state->info.exists = true;
    state->info.owner = "fake-owner";
    state->info.count = 3;
    gm.set_lock_backend(std::make_unique<FakeLockBackend>(state));

    BOOST_CHECK(gm.mutex_acquire("mutex", "m1", "o1", 100) == LockStatus::kOk);
    BOOST_CHECK_EQUAL(state->acquire_calls, 1u);
    // Busy semantics come from the backend, not the manager.
    state->next_acquire = LockStatus::kBusy;
    BOOST_CHECK(gm.mutex_acquire("mutex", "m1", "o2", 0) == LockStatus::kBusy);
    BOOST_CHECK_EQUAL(state->acquire_calls, 2u);
    BOOST_CHECK(gm.mutex_extend("mutex", "m1", "o1", 100));
    BOOST_CHECK_EQUAL(state->extend_calls, 1u);
    state->next_extend = false;
    BOOST_CHECK(!gm.mutex_extend("mutex", "m1", "o1", 100));
    const auto info = gm.mutex_info("mutex", "m1");
    BOOST_CHECK(info.exists);
    BOOST_CHECK_EQUAL(info.owner, "fake-owner");
    BOOST_CHECK_EQUAL(info.count, 3u);
    BOOST_CHECK_EQUAL(state->info_calls, 1u);
    BOOST_CHECK_EQUAL(gm.mutex_registry_size("mutex"), 7u);
    BOOST_CHECK_EQUAL(state->size_calls, 1u);
    BOOST_CHECK(gm.mutex_release("mutex", "m1", "o1") == LockStatus::kOk);
    BOOST_CHECK_EQUAL(state->release_calls, 1u);
    state->next_release = LockStatus::kNotOwner;
    BOOST_CHECK(gm.mutex_release("mutex", "m1", "o1") == LockStatus::kNotOwner);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Backend selection config
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(BackendConfigSuite)

BOOST_AUTO_TEST_CASE(ParsesLockBackendKeys) {
    auto& cfg = shield::config::global_config();
    cfg.set("global.lock_backend", "redis");
    cfg.set("global.redis.host", "127.0.0.1");
    cfg.set("global.redis.port", 7200);
    GlobalConfig parsed;
    BOOST_REQUIRE(GlobalConfig::from_global_config(&parsed, nullptr));
    BOOST_CHECK_EQUAL(parsed.lock_backend, "redis");
    BOOST_CHECK(validate_global_config(parsed, nullptr));
    // Restore the defaults so later suites parse the stock config.
    cfg.set("global.lock_backend", "");
    cfg.set("global.redis.host", "");
    cfg.set("global.redis.port", 6379);
}

BOOST_AUTO_TEST_CASE(ValidatesUnknownLockBackend) {
    GlobalConfig config;
    config.lock_backend = "etcd";
    std::string error;
    BOOST_CHECK(!validate_global_config(config, &error));
    BOOST_CHECK_NE(error.find("global.lock_backend"), std::string::npos);
    BOOST_CHECK(!validate_global_config(config, nullptr));
}

BOOST_AUTO_TEST_CASE(ValidatesRedisLockNeedsHost) {
    GlobalConfig config;
    config.lock_backend = "redis";
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

BOOST_AUTO_TEST_SUITE(RedisLockBackendSuite)

BOOST_AUTO_TEST_CASE(RedisLockLifecycle) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:gltest:" + std::to_string(::getpid()) + ":life";
    auto backend =
        shield::global::make_redis_lock_backend(redis_cfg(*srv, prefix));

    BOOST_CHECK(!backend->mutex_info("mutex", "m1").exists);
    BOOST_CHECK_EQUAL(backend->mutex_registry_size("mutex"), 0u);
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-a", 0) ==
                LockStatus::kOk);
    const auto info = backend->mutex_info("mutex", "m1");
    BOOST_CHECK(info.exists);
    BOOST_CHECK_EQUAL(info.owner, "owner-a");
    BOOST_CHECK_EQUAL(info.count, 1u);
    BOOST_CHECK(info.acquired_at_ms > 0);
    BOOST_CHECK_EQUAL(info.ttl_remaining_ms, 0u);
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-b", 0) ==
                LockStatus::kBusy);
    BOOST_CHECK(backend->mutex_release("mutex", "m1", "owner-b") ==
                LockStatus::kNotOwner);
    BOOST_CHECK(backend->mutex_release("mutex", "m1", "owner-a") ==
                LockStatus::kOk);
    // The hash is gone: a second release reports not-owner.
    BOOST_CHECK(backend->mutex_release("mutex", "m1", "owner-a") ==
                LockStatus::kNotOwner);
    BOOST_CHECK_EQUAL(backend->mutex_registry_size("mutex"), 0u);
}

BOOST_AUTO_TEST_CASE(RedisLockReentrantCountAndTtlRefresh) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:gltest:" + std::to_string(::getpid()) + ":reent";
    auto backend =
        shield::global::make_redis_lock_backend(redis_cfg(*srv, prefix));
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-a", 300) ==
                LockStatus::kOk);
    std::this_thread::sleep_for(150ms);
    // Reentrant acquire bumps the count and refreshes the ttl: the short
    // first window does not lapse while the holder keeps re-acquiring.
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-a", 3000) ==
                LockStatus::kOk);
    std::this_thread::sleep_for(300ms);
    const auto info = backend->mutex_info("mutex", "m1");
    BOOST_CHECK(info.exists);
    BOOST_CHECK_EQUAL(info.count, 2u);
    BOOST_CHECK(info.ttl_remaining_ms > 0);
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-b", 0) ==
                LockStatus::kBusy);
    BOOST_CHECK(backend->mutex_release("mutex", "m1", "owner-b") ==
                LockStatus::kNotOwner);
    BOOST_CHECK(backend->mutex_release("mutex", "m1", "owner-a") ==
                LockStatus::kOk);
    const auto alive = backend->mutex_info("mutex", "m1");
    BOOST_CHECK(alive.exists);
    BOOST_CHECK_EQUAL(alive.count, 1u);
    BOOST_CHECK_EQUAL(alive.owner, "owner-a");
    BOOST_CHECK(backend->mutex_release("mutex", "m1", "owner-a") ==
                LockStatus::kOk);
    BOOST_CHECK(!backend->mutex_info("mutex", "m1").exists);
}

BOOST_AUTO_TEST_CASE(RedisLockTtlExpiresServerSide) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:gltest:" + std::to_string(::getpid()) + ":ttl";
    auto backend =
        shield::global::make_redis_lock_backend(redis_cfg(*srv, prefix));
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-a", 300) ==
                LockStatus::kOk);
    // The expiry is server-side: a fresh backend handle sees it too.
    auto other =
        shield::global::make_redis_lock_backend(redis_cfg(*srv, prefix));
    BOOST_CHECK(wait_until(
        [&] { return !other->mutex_info("mutex", "m1").exists; }, 3000));
    BOOST_CHECK(other->mutex_acquire("mutex", "m1", "owner-b", 0) ==
                LockStatus::kOk);
    BOOST_CHECK(backend->mutex_release("mutex", "m1", "owner-a") ==
                LockStatus::kNotOwner);
}

BOOST_AUTO_TEST_CASE(RedisLockExtend) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:gltest:" + std::to_string(::getpid()) + ":ext";
    auto backend =
        shield::global::make_redis_lock_backend(redis_cfg(*srv, prefix));
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-a", 300) ==
                LockStatus::kOk);
    BOOST_CHECK(backend->mutex_extend("mutex", "m1", "owner-a", 3000));
    // The original ttl would have lapsed by now.
    std::this_thread::sleep_for(500ms);
    BOOST_CHECK(backend->mutex_info("mutex", "m1").ttl_remaining_ms > 0);
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-b", 0) ==
                LockStatus::kBusy);
    // Extend arms: a non-holder and a missing lock both fail.
    BOOST_CHECK(!backend->mutex_extend("mutex", "m1", "owner-b", 1000));
    BOOST_CHECK(!backend->mutex_extend("mutex", "ghost", "owner-a", 1000));
}

BOOST_AUTO_TEST_CASE(RedisLockExtendTtlZeroPersists) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:gltest:" + std::to_string(::getpid()) + ":persist";
    auto backend =
        shield::global::make_redis_lock_backend(redis_cfg(*srv, prefix));
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-a", 400) ==
                LockStatus::kOk);
    // Extend with ttl 0 drops the expiry (PERSIST): the lock is immortal.
    BOOST_CHECK(backend->mutex_extend("mutex", "m1", "owner-a", 0));
    std::this_thread::sleep_for(600ms);
    const auto info = backend->mutex_info("mutex", "m1");
    BOOST_CHECK(info.exists);
    BOOST_CHECK_EQUAL(info.ttl_remaining_ms, 0u);
    BOOST_CHECK(backend->mutex_acquire("mutex", "m1", "owner-b", 0) ==
                LockStatus::kBusy);
    BOOST_CHECK(backend->mutex_release("mutex", "m1", "owner-a") ==
                LockStatus::kOk);
}

BOOST_AUTO_TEST_CASE(RedisLockRegistryAndPrefixIsolation) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string base =
        "shield:gltest:" + std::to_string(::getpid()) + ":iso";
    auto a =
        shield::global::make_redis_lock_backend(redis_cfg(*srv, base + "a"));
    auto b =
        shield::global::make_redis_lock_backend(redis_cfg(*srv, base + "b"));
    // Registries are isolated under one prefix.
    BOOST_CHECK(a->mutex_acquire("mutex", "m1", "o1", 0) == LockStatus::kOk);
    BOOST_CHECK(a->mutex_acquire("spinlock", "m1", "o1", 0) == LockStatus::kOk);
    BOOST_CHECK_EQUAL(a->mutex_registry_size("mutex"), 1u);
    BOOST_CHECK_EQUAL(a->mutex_registry_size("spinlock"), 1u);
    // Prefixes are isolated keyspaces on the same server.
    BOOST_CHECK_EQUAL(b->mutex_registry_size("mutex"), 0u);
    BOOST_CHECK(b->mutex_acquire("mutex", "m1", "o2", 0) == LockStatus::kOk);
    BOOST_CHECK_EQUAL(a->mutex_info("mutex", "m1").owner, "o1");
    BOOST_CHECK_EQUAL(b->mutex_info("mutex", "m1").owner, "o2");
}

BOOST_AUTO_TEST_CASE(RedisLockClosedPortThrows) {
    const int closed_port = probe_free_port();
    BOOST_REQUIRE(closed_port > 0);
    RedisDataBackendConfig config;
    config.host = "127.0.0.1";
    config.port = closed_port;
    config.password = "pw";  // connection options still get built
    config.db = 1;
    config.prefix = "shield:gltest:closed";
    auto backend = shield::global::make_redis_lock_backend(config);
    BOOST_CHECK_THROW(backend->mutex_acquire("mutex", "m1", "o", 0),
                      std::exception);
}

BOOST_AUTO_TEST_CASE(ManagerWithRedisLockConfigEndToEnd) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    GlobalConfig config;
    config.lock_backend = "redis";
    config.redis_host = "127.0.0.1";
    config.redis_port = srv->port;
    config.redis_prefix =
        "shield:gltest:" + std::to_string(::getpid()) + ":mgr";
    GlobalManager gm(config);

    BOOST_CHECK(gm.mutex_acquire("mutex", "m1", "o1", 0) == LockStatus::kOk);
    BOOST_CHECK(gm.mutex_acquire("mutex", "m1", "o2", 0) == LockStatus::kBusy);
    const auto info = gm.mutex_info("mutex", "m1");
    BOOST_CHECK(info.exists);
    BOOST_CHECK_EQUAL(info.owner, "o1");
    BOOST_CHECK_EQUAL(gm.mutex_registry_size("mutex"), 1u);
    BOOST_CHECK(gm.mutex_extend("mutex", "m1", "o1", 1000));
    BOOST_CHECK(gm.mutex_release("mutex", "m1", "o1") == LockStatus::kOk);
    // The freed name goes to the next acquirer through the manager too.
    BOOST_CHECK(gm.mutex_acquire("mutex", "m1", "o2", 0) == LockStatus::kOk);
    BOOST_CHECK(gm.mutex_release("mutex", "m1", "o2") == LockStatus::kOk);
    BOOST_CHECK_EQUAL(gm.mutex_registry_size("mutex"), 0u);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // _WIN32
