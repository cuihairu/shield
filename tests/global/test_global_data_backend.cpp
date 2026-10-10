// GlobalDataBackend tests: in-process backend semantics, GlobalManager
// delegation (cache coherence stays in the manager layer), backend config
// selection, and the redis++ backend against a self-spawned redis-server.
// Constructed directly from GlobalConfig: no global config store (except
// the parsing suite), no Lua, no bootstrap.
#define BOOST_TEST_MODULE GlobalDataBackendTests
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/global/global_data_backend.hpp"
#include "shield/global/global_manager.hpp"

using namespace std::chrono_literals;
using shield::global::GlobalConfig;
using shield::global::GlobalDataBackend;
using shield::global::GlobalManager;
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
// Self-spawned redis-server (same pattern as the cluster discovery suite:
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

BOOST_AUTO_TEST_SUITE(InProcessBackendSuite)

BOOST_AUTO_TEST_CASE(BackendGetSetDelete) {
    auto backend = shield::global::make_inprocess_data_backend();
    std::string value;
    BOOST_CHECK(!backend->get("missing", &value));
    backend->set("k", "42", 0);
    BOOST_CHECK(backend->get("k", &value));
    BOOST_CHECK_EQUAL(value, "42");
    // Existence check without copying the value out.
    BOOST_CHECK(backend->get("k", nullptr));
    BOOST_CHECK(backend->del("k"));
    BOOST_CHECK(!backend->del("k"));
    BOOST_CHECK_EQUAL(backend->size(), 0u);
}

BOOST_AUTO_TEST_CASE(BackendTtlSemantics) {
    auto backend = shield::global::make_inprocess_data_backend();
    std::string value;
    backend->set("k", "v1", 30);
    BOOST_CHECK(backend->get("k", &value));
    BOOST_CHECK(wait_until([&] { return !backend->get("k", &value); }, 2000));
    // ttl 0 clears any expiry a previous write left: the key is immortal.
    backend->set("k", "v2", 0);
    std::this_thread::sleep_for(80ms);
    BOOST_CHECK(backend->get("k", &value));
    BOOST_CHECK_EQUAL(value, "v2");
    BOOST_CHECK_EQUAL(backend->size(), 1u);
}

BOOST_AUTO_TEST_CASE(BackendIncrByRejectsNonIntegers) {
    auto backend = shield::global::make_inprocess_data_backend();
    std::int64_t out = 0;
    // Missing keys start at 0.
    BOOST_CHECK(backend->incr_by("c", 5, &out, nullptr));
    BOOST_CHECK_EQUAL(out, 5);
    BOOST_CHECK(backend->incr_by("c", -2, &out, nullptr));
    BOOST_CHECK_EQUAL(out, 3);
    // A non-integer value fails without writing; null error/out arms too.
    backend->set("text", "abc", 0);
    std::string error;
    BOOST_CHECK(!backend->incr_by("text", 1, &out, &error));
    BOOST_CHECK_NE(error.find("not an integer"), std::string::npos);
    BOOST_CHECK(!backend->incr_by("text", 1, &out, nullptr));
    BOOST_CHECK(backend->incr_by("fresh", 7, nullptr, nullptr));
}

BOOST_AUTO_TEST_CASE(BackendMgetMset) {
    auto backend = shield::global::make_inprocess_data_backend();
    BOOST_CHECK(
        backend->mset({{"a", "1"}, {"b", "2"}, {"c", "3"}}, 0, nullptr));
    const auto values = backend->mget({"a", "nope", "c"});
    BOOST_REQUIRE_EQUAL(values.size(), 3u);
    BOOST_REQUIRE(values[0].has_value());
    BOOST_CHECK_EQUAL(*values[0], "1");
    BOOST_CHECK(!values[1].has_value());
    BOOST_REQUIRE(values[2].has_value());
    BOOST_CHECK_EQUAL(*values[2], "3");
    // Degenerate forms stay no-ops.
    BOOST_CHECK(backend->mget({}).empty());
    BOOST_CHECK(backend->mset({}, 0, nullptr));
}

BOOST_AUTO_TEST_CASE(BackendSizeCounts) {
    auto backend = shield::global::make_inprocess_data_backend();
    backend->set("a", "1", 0);
    backend->set("b", "2", 0);
    backend->set("c", "3", 0);
    BOOST_CHECK_EQUAL(backend->size(), 3u);
    backend->del("b");
    BOOST_CHECK_EQUAL(backend->size(), 2u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// GlobalManager delegation: cache coherence lives in the manager layer
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(ManagerDelegationSuite)

BOOST_AUTO_TEST_CASE(DeleteDropsCachedCopy) {
    GlobalManager gm(GlobalConfig{});
    std::string value;
    gm.data_set("k", "v1", 0);
    BOOST_CHECK(gm.cache_get("k", 60000, &value));
    BOOST_CHECK_EQUAL(value, "v1");
    BOOST_CHECK(gm.data_delete("k"));
    // The cached copy is gone: a refill would miss.
    BOOST_CHECK(!gm.cache_get("k", 60000, &value));
    BOOST_CHECK_EQUAL(gm.cache_size(), 0u);
}

BOOST_AUTO_TEST_CASE(IncrDropsCachedCopy) {
    GlobalManager gm(GlobalConfig{});
    std::string value;
    gm.data_set("k", "1", 0);
    BOOST_CHECK(gm.cache_get("k", 60000, &value));
    BOOST_CHECK_EQUAL(value, "1");
    std::int64_t out = 0;
    BOOST_CHECK(gm.data_incr_by("k", 4, &out, nullptr));
    BOOST_CHECK_EQUAL(out, 5);
    // The increment is a write: the stale cached copy must not resurface.
    BOOST_CHECK(gm.cache_get("k", 60000, &value));
    BOOST_CHECK_EQUAL(value, "5");
}

BOOST_AUTO_TEST_CASE(LazyExpiryDropsCachedCopy) {
    GlobalManager gm(GlobalConfig{});
    gm.data_set("k", "v", 30);
    std::string value;
    BOOST_CHECK(gm.cache_get("k", 60000, &value));
    // Reading the expired data key drops its cached copy too.
    BOOST_CHECK(wait_until([&] { return !gm.data_get("k", nullptr); }, 2000));
    BOOST_CHECK_EQUAL(gm.cache_size(), 0u);
}

BOOST_AUTO_TEST_CASE(MsetRejectsEmptyKeyWithoutPartialWrite) {
    GlobalManager gm(GlobalConfig{});
    std::string error;
    BOOST_CHECK(!gm.data_mset({{"ok", "1"}, {"", "2"}}, 0, &error));
    BOOST_CHECK_NE(error.find("must not be empty"), std::string::npos);
    // Validation runs before any write: nothing landed.
    BOOST_CHECK_EQUAL(gm.data_size(), 0u);
    std::string value;
    BOOST_CHECK(!gm.data_get("ok", &value));
    BOOST_CHECK(!gm.data_mset({{"", "v"}}, 0, nullptr));
}

BOOST_AUTO_TEST_CASE(UnknownBackendFallsBackToProcessMemory) {
    GlobalConfig config;
    config.data_backend = "bogus";
    GlobalManager gm(config);
    std::string value;
    gm.data_set("k", "v", 0);
    BOOST_CHECK(gm.data_get("k", &value));
    BOOST_CHECK_EQUAL(value, "v");
}

BOOST_AUTO_TEST_CASE(NullBackendSwapIsIgnored) {
    GlobalManager gm(GlobalConfig{});
    gm.set_data_backend(nullptr);
    std::string value;
    gm.data_set("k", "v", 0);
    BOOST_CHECK(gm.data_get("k", &value));
}

/// A plain in-map fake: the manager must route every data op into whatever
/// backend is installed. State is shared with the test so ownership of the
/// backend by the manager can never dangle the test's view of it.
struct FakeState {
    std::mutex mu;
    std::unordered_map<std::string, std::string> kv;
};

class FakeBackend final : public GlobalDataBackend {
public:
    explicit FakeBackend(std::shared_ptr<FakeState> state)
        : state_(std::move(state)) {}

    bool get(const std::string& key, std::string* out) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        auto it = state_->kv.find(key);
        if (it == state_->kv.end()) {
            return false;
        }
        if (out) *out = it->second;
        return true;
    }

    void set(const std::string& key, std::string value,
             std::uint64_t /*ttl_ms*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        state_->kv[key] = std::move(value);
    }

    bool del(const std::string& key) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        return state_->kv.erase(key) != 0;
    }

    bool incr_by(const std::string& key, std::int64_t delta, std::int64_t* out,
                 std::string* /*error*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        std::int64_t current = 0;
        auto it = state_->kv.find(key);
        if (it != state_->kv.end()) {
            current = std::stoll(it->second);
        }
        current += delta;
        state_->kv[key] = std::to_string(current);
        if (out) *out = current;
        return true;
    }

    std::vector<std::optional<std::string>> mget(
        const std::vector<std::string>& keys) override {
        std::vector<std::optional<std::string>> values;
        values.reserve(keys.size());
        for (const auto& key : keys) {
            std::string value;
            if (get(key, &value)) {
                values.emplace_back(std::move(value));
            } else {
                values.emplace_back();
            }
        }
        return values;
    }

    bool mset(const std::vector<std::pair<std::string, std::string>>& kvs,
              std::uint64_t /*ttl_ms*/, std::string* /*error*/) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        for (const auto& kv : kvs) state_->kv[kv.first] = kv.second;
        return true;
    }

    std::size_t size() override {
        std::lock_guard<std::mutex> lock(state_->mu);
        return state_->kv.size();
    }

private:
    std::shared_ptr<FakeState> state_;
};

BOOST_AUTO_TEST_CASE(SetDataBackendRoutesEveryOp) {
    GlobalManager gm(GlobalConfig{});
    auto state = std::make_shared<FakeState>();
    gm.set_data_backend(std::make_unique<FakeBackend>(state));

    std::string value;
    gm.data_set("a", "1", 0);
    BOOST_CHECK_EQUAL(state->kv["a"], "1");  // the write landed in the fake
    BOOST_CHECK(gm.data_get("a", &value));
    BOOST_CHECK_EQUAL(value, "1");
    std::int64_t out = 0;
    BOOST_CHECK(gm.data_incr_by("a", 2, &out, nullptr));
    BOOST_CHECK_EQUAL(out, 3);
    BOOST_CHECK(gm.data_mset({{"b", "2"}}, 0, nullptr));
    const auto values = gm.data_mget({"a", "nope", "b"});
    BOOST_REQUIRE_EQUAL(values.size(), 3u);
    BOOST_CHECK(!values[1].has_value());
    BOOST_CHECK_EQUAL(gm.data_size(), 2u);
    BOOST_CHECK(gm.data_delete("a"));
    BOOST_CHECK_EQUAL(gm.data_size(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Backend selection config
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(BackendConfigSuite)

BOOST_AUTO_TEST_CASE(ParsesRedisBackendKeys) {
    auto& cfg = shield::config::global_config();
    cfg.set("global.data_backend", "redis");
    cfg.set("global.redis.host", "127.0.0.1");
    cfg.set("global.redis.port", 7100);
    cfg.set("global.redis.password", "pw");
    cfg.set("global.redis.db", 2);
    cfg.set("global.redis.prefix", "shield:gx");
    GlobalConfig parsed;
    BOOST_REQUIRE(GlobalConfig::from_global_config(&parsed, nullptr));
    BOOST_CHECK_EQUAL(parsed.data_backend, "redis");
    BOOST_CHECK_EQUAL(parsed.redis_host, "127.0.0.1");
    BOOST_CHECK_EQUAL(parsed.redis_port, 7100);
    BOOST_CHECK_EQUAL(parsed.redis_password, "pw");
    BOOST_CHECK_EQUAL(parsed.redis_db, 2);
    BOOST_CHECK_EQUAL(parsed.redis_prefix, "shield:gx");
    BOOST_CHECK(validate_global_config(parsed, nullptr));
    // Restore the defaults so later suites parse the stock config.
    cfg.set("global.data_backend", "");
    cfg.set("global.redis.host", "");
    cfg.set("global.redis.port", 6379);
    cfg.set("global.redis.password", "");
    cfg.set("global.redis.db", 0);
    cfg.set("global.redis.prefix", "shield:global");
}

BOOST_AUTO_TEST_CASE(ValidatesUnknownBackend) {
    GlobalConfig config;
    config.data_backend = "etcd";
    std::string error;
    BOOST_CHECK(!validate_global_config(config, &error));
    BOOST_CHECK_NE(error.find("global.data_backend"), std::string::npos);
    BOOST_CHECK(!validate_global_config(config, nullptr));
}

BOOST_AUTO_TEST_CASE(ValidatesRedisNeedsHost) {
    GlobalConfig config;
    config.data_backend = "redis";
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

BOOST_AUTO_TEST_SUITE(RedisBackendSuite)

BOOST_AUTO_TEST_CASE(RedisBackendLifecycle) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:gdtest:" + std::to_string(::getpid()) + ":life";
    auto backend =
        shield::global::make_redis_data_backend(redis_cfg(*srv, prefix));

    std::string value;
    BOOST_CHECK(!backend->get("k", &value));
    backend->set("k", "42", 0);
    BOOST_CHECK(backend->get("k", &value));
    BOOST_CHECK_EQUAL(value, "42");
    BOOST_CHECK(backend->get("k", nullptr));
    // ttl 0 clears the TTL a previous write left.
    backend->set("k", "v1", 200);
    backend->set("k", "v2", 0);
    std::this_thread::sleep_for(400ms);
    BOOST_CHECK(backend->get("k", &value));
    BOOST_CHECK_EQUAL(value, "v2");
    BOOST_CHECK_EQUAL(backend->size(), 1u);
    BOOST_CHECK(backend->del("k"));
    BOOST_CHECK(!backend->del("k"));
    BOOST_CHECK_EQUAL(backend->size(), 0u);
}

BOOST_AUTO_TEST_CASE(RedisSetTtlExpires) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:gdtest:" + std::to_string(::getpid()) + ":ttl";
    auto backend =
        shield::global::make_redis_data_backend(redis_cfg(*srv, prefix));
    std::string value;
    backend->set("k", "v", 300);
    BOOST_CHECK(backend->get("k", &value));
    // The expiry is server-side: a fresh backend handle sees it too.
    auto other =
        shield::global::make_redis_data_backend(redis_cfg(*srv, prefix));
    BOOST_CHECK(wait_until([&] { return !other->get("k", &value); }, 2000));
    BOOST_CHECK_EQUAL(backend->size(), 0u);
}

BOOST_AUTO_TEST_CASE(RedisIncrByRejectsNonIntegers) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:gdtest:" + std::to_string(::getpid()) + ":incr";
    auto backend =
        shield::global::make_redis_data_backend(redis_cfg(*srv, prefix));
    std::int64_t out = 0;
    BOOST_CHECK(backend->incr_by("c", 5, &out, nullptr));
    BOOST_CHECK_EQUAL(out, 5);
    BOOST_CHECK(backend->incr_by("c", -2, &out, nullptr));
    BOOST_CHECK_EQUAL(out, 3);
    backend->set("text", "abc", 0);
    std::string error;
    BOOST_CHECK(!backend->incr_by("text", 1, &out, &error));
    BOOST_CHECK_NE(error.find("not an integer"), std::string::npos);
    BOOST_CHECK(!backend->incr_by("text", 1, &out, nullptr));
}

BOOST_AUTO_TEST_CASE(RedisMgetMsetAndSize) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:gdtest:" + std::to_string(::getpid()) + ":bulk";
    auto backend =
        shield::global::make_redis_data_backend(redis_cfg(*srv, prefix));
    // Degenerate forms stay no-ops.
    BOOST_CHECK(backend->mget({}).empty());
    BOOST_CHECK(backend->mset({}, 0, nullptr));
    BOOST_CHECK(
        backend->mset({{"a", "1"}, {"b", "2"}, {"c", "3"}}, 0, nullptr));
    const auto values = backend->mget({"a", "nope", "c"});
    BOOST_REQUIRE_EQUAL(values.size(), 3u);
    BOOST_REQUIRE(values[0].has_value());
    BOOST_CHECK_EQUAL(*values[0], "1");
    BOOST_CHECK(!values[1].has_value());
    BOOST_REQUIRE(values[2].has_value());
    BOOST_CHECK_EQUAL(*values[2], "3");
    // mset with a TTL stamps every key; they expire server-side.
    BOOST_CHECK(backend->mset({{"x", "1"}, {"y", "2"}}, 300, nullptr));
    BOOST_CHECK_EQUAL(backend->size(), 5u);
    BOOST_CHECK(wait_until([&] { return backend->size() == 3; }, 2000));
}

BOOST_AUTO_TEST_CASE(RedisPrefixIsolatesKeyspaces) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string base =
        "shield:gdtest:" + std::to_string(::getpid()) + ":iso";
    auto a =
        shield::global::make_redis_data_backend(redis_cfg(*srv, base + "a"));
    auto b =
        shield::global::make_redis_data_backend(redis_cfg(*srv, base + "b"));
    a->set("k", "from-a", 0);
    b->set("k", "from-b", 0);
    std::string value;
    BOOST_CHECK(a->get("k", &value));
    BOOST_CHECK_EQUAL(value, "from-a");
    BOOST_CHECK(b->get("k", &value));
    BOOST_CHECK_EQUAL(value, "from-b");
    BOOST_CHECK_EQUAL(a->size(), 1u);
    BOOST_CHECK_EQUAL(b->size(), 1u);
}

BOOST_AUTO_TEST_CASE(RedisClosedPortThrows) {
    const int closed_port = probe_free_port();
    BOOST_REQUIRE(closed_port > 0);
    RedisDataBackendConfig config;
    config.host = "127.0.0.1";
    config.port = closed_port;
    config.password = "pw";  // connection options still get built
    config.db = 1;
    config.prefix = "shield:gdtest:closed";
    auto backend = shield::global::make_redis_data_backend(config);
    BOOST_CHECK_THROW(backend->get("k", nullptr), std::exception);
}

BOOST_AUTO_TEST_CASE(ManagerWithRedisConfigEndToEnd) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    GlobalConfig config;
    config.data_backend = "redis";
    config.redis_host = "127.0.0.1";
    config.redis_port = srv->port;
    config.redis_prefix =
        "shield:gdtest:" + std::to_string(::getpid()) + ":mgr";
    GlobalManager gm(config);

    std::string value;
    gm.data_set("k", "v", 0);
    BOOST_CHECK(gm.data_get("k", &value));
    BOOST_CHECK_EQUAL(value, "v");
    BOOST_CHECK(gm.data_get("k", nullptr));
    // The local cache fills from the Redis backend and stays coherent.
    BOOST_CHECK(gm.cache_get("k", 60000, &value));
    BOOST_CHECK_EQUAL(value, "v");
    gm.data_set("k", "v2", 0);
    BOOST_CHECK(gm.cache_get("k", 60000, &value));
    BOOST_CHECK_EQUAL(value, "v2");

    std::int64_t out = 0;
    BOOST_CHECK(gm.data_incr_by("counter", 3, &out, nullptr));
    BOOST_CHECK_EQUAL(out, 3);
    BOOST_CHECK(gm.data_mset({{"m1", "1"}, {"m2", "2"}}, 0, nullptr));
    const auto values = gm.data_mget({"m1", "nope", "m2"});
    BOOST_REQUIRE_EQUAL(values.size(), 3u);
    BOOST_CHECK(!values[1].has_value());
    BOOST_CHECK_EQUAL(gm.data_size(), 4u);
    BOOST_CHECK(gm.data_delete("k"));
    BOOST_CHECK_EQUAL(gm.data_size(), 3u);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // _WIN32
