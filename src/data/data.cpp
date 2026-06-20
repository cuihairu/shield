// [SHIELD_DATA] Data access implementation
#include "shield/data/data.hpp"

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"

#include <sw/redis++/redis++.h>

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

// Forward declarations for hiredis/redis++
namespace redispp {
template<typename... T>
class Connection;
}

namespace shield::data {

// =============================================================================
// Database Implementation
// =============================================================================

// Mock DatabaseConnection implementation.
// When `force_error` is set, all operations return the configured error.
class MockDatabaseConnection : public DatabaseConnection {
public:
    // Test hook: set to non-empty to make all queries return an error.
    static inline std::string force_error;

    QueryResult query(std::string_view sql,
                     const std::vector<std::string>& params) override {
        if (!force_error.empty()) {
            return QueryResult::error(force_error);
        }
        return QueryResult::ok();
    }

    QueryResult query_one(std::string_view sql,
                         const std::vector<std::string>& params) override {
        if (!force_error.empty()) {
            return QueryResult::error(force_error);
        }
        return QueryResult::ok();
    }

    QueryResult execute(std::string_view sql,
                       const std::vector<std::string>& params) override {
        if (!force_error.empty()) {
            return QueryResult::error(force_error);
        }
        auto result = QueryResult::ok();
        result.affected_rows = 1;
        result.last_insert_id = 1;
        return result;
    }

    bool ping() override {
        return true;
    }

    void close() override {}
};

struct DatabasePool::Impl {
    std::vector<std::shared_ptr<DatabaseConnection>> connections;
    std::queue<std::shared_ptr<DatabaseConnection>> available;
    std::mutex mutex;
    std::condition_variable cv;
    size_t max_size = 10;
    size_t current_size = 0;
    bool initialized = false;
    std::string config_key;
};

DatabasePool::DatabasePool() : impl_(std::make_unique<Impl>()) {}

DatabasePool::~DatabasePool() = default;

bool DatabasePool::initialize(std::string_view config_key) {
    std::lock_guard lock(impl_->mutex);
    impl_->config_key = std::string(config_key);

    // Read from config
    auto& cfg = shield::config::global_config();

    std::string prefix(config_key);
    std::string host = cfg.get_string(prefix + ".host",
                                     "localhost");
    int port = static_cast<int>(cfg.get_int(prefix + ".port", 3306));
    std::string database = cfg.get_string(prefix + ".database",
                                           "shield");
    std::string user = cfg.get_string(prefix + ".user",
                                       "root");
    std::string password = cfg.get_string(prefix + ".password",
                                           "");

    auto& log = shield::log::get_logger("database");
    SHIELD_LOG_INFO(log, "Database config: " + host + ":" + std::to_string(port) +
                    "/" + database);

    // Create mock connections for now
    for (size_t i = 0; i < impl_->max_size; ++i) {
        auto conn = std::make_shared<MockDatabaseConnection>();
        impl_->connections.push_back(conn);
        impl_->available.push(conn);
    }

    impl_->current_size = impl_->max_size;
    impl_->initialized = true;
    return true;
}

std::shared_ptr<DatabaseConnection> DatabasePool::acquire() {
    std::unique_lock lock(impl_->mutex);

    // Wait for available connection
    impl_->cv.wait(lock, [this]() {
        return !impl_->available.empty() || !impl_->initialized;
    });

    if (!impl_->initialized || impl_->available.empty()) {
        return nullptr;
    }

    auto conn = impl_->available.front();
    impl_->available.pop();
    return conn;
}

void DatabasePool::release(std::shared_ptr<DatabaseConnection> conn) {
    if (!conn) {
        return;
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->initialized) {
            impl_->available.push(std::move(conn));
        }
    }
    impl_->cv.notify_one();
}

QueryResult DatabasePool::query(std::string_view sql,
                                const std::vector<std::string>& params) {
    auto conn = acquire();
    if (!conn) {
        return QueryResult::error("No database connection available");
    }
    auto result = conn->query(sql, params);
    release(std::move(conn));
    return result;
}

QueryResult DatabasePool::query_one(std::string_view sql,
                                    const std::vector<std::string>& params) {
    auto conn = acquire();
    if (!conn) {
        return QueryResult::error("No database connection available");
    }
    auto result = conn->query_one(sql, params);
    release(std::move(conn));
    return result;
}

QueryResult DatabasePool::execute(std::string_view sql,
                                  const std::vector<std::string>& params) {
    auto conn = acquire();
    if (!conn) {
        return QueryResult::error("No database connection available");
    }
    auto result = conn->execute(sql, params);
    release(std::move(conn));
    return result;
}

bool DatabasePool::is_initialized() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->initialized;
}

// =============================================================================
// Redis Implementation
// =============================================================================

// Real RedisConnection implementation using redis++.
class RealRedisConnection : public RedisConnection {
public:
    explicit RealRedisConnection(const sw::redis::ConnectionOptions& opts)
        : redis_(opts) {}

    std::pair<bool, std::string> get(std::string_view key) override {
        try {
            auto val = redis_.get(std::string(key));
            if (val) {
                return {true, *val};
            }
            return {false, ""};
        } catch (const std::exception& e) {
            return {false, ""};
        }
    }

    bool set(std::string_view key, std::string_view value,
             int ttl_seconds) override {
        try {
            if (ttl_seconds > 0) {
                return redis_.set(std::string(key), std::string(value),
                                  std::chrono::milliseconds(ttl_seconds * 1000));
            }
            return redis_.set(std::string(key), std::string(value));
        } catch (const std::exception& e) {
            return false;
        }
    }

    int del(std::string_view key) override {
        try {
            return static_cast<int>(redis_.del(std::string(key)));
        } catch (const std::exception& e) {
            return 0;
        }
    }

    bool exists(std::string_view key) override {
        try {
            return redis_.exists(std::string(key)) > 0;
        } catch (const std::exception& e) {
            return false;
        }
    }

    int publish(std::string_view channel, std::string_view message) override {
        try {
            return static_cast<int>(
                redis_.publish(std::string(channel), std::string(message)));
        } catch (const std::exception& e) {
            return 0;
        }
    }

    bool subscribe(std::string_view channel,
                   SubscribeCallback callback) override {
        try {
            auto sub = redis_.subscriber();
            sub.on_message([callback](std::string ch, std::string msg) {
                callback(ch, msg);
            });
            sub.subscribe(std::string(channel));
            // Run subscriber in a background thread.
            subscriber_thread_ = std::thread([sub = std::move(sub)]() mutable {
                try {
                    while (true) {
                        sub.consume();
                    }
                } catch (...) {
                    // Subscription cancelled or connection lost.
                }
            });
            return true;
        } catch (const std::exception& e) {
            return false;
        }
    }

    bool unsubscribe(std::string_view channel) override {
        try {
            // The subscriber thread will exit on next consume() failure.
            // For a clean unsubscribe, we'd need to store the subscriber
            // object and call sub.unsubscribe(). For now, detach the thread.
            if (subscriber_thread_.joinable()) {
                subscriber_thread_.detach();
            }
            return true;
        } catch (const std::exception& e) {
            return false;
        }
    }

    bool ping() override {
        try {
            redis_.ping();  // returns "PONG" string; throws on failure
            return true;
        } catch (const std::exception& e) {
            return false;
        }
    }

    void close() override {
        if (subscriber_thread_.joinable()) {
            subscriber_thread_.detach();
        }
    }

private:
    sw::redis::Redis redis_;
    std::thread subscriber_thread_;
};

// Placeholder RedisConnection implementation
class MockRedisConnection : public RedisConnection {
public:
    std::pair<bool, std::string> get(std::string_view key) override {
        return {false, ""};
    }

    bool set(std::string_view key, std::string_view value,
            int ttl_seconds) override {
        return true;
    }

    int del(std::string_view key) override {
        return 0;
    }

    bool exists(std::string_view key) override {
        (void)key;
        return false;
    }

    int publish(std::string_view channel, std::string_view message) override {
        return 0;
    }

    bool subscribe(std::string_view channel,
                  SubscribeCallback callback) override {
        return true;
    }

    bool unsubscribe(std::string_view channel) override {
        return true;
    }

    bool ping() override {
        return true;
    }

    void close() override {}
};

struct RedisPool::Impl {
    std::vector<std::shared_ptr<RedisConnection>> connections;
    std::queue<std::shared_ptr<RedisConnection>> available;
    std::mutex mutex;
    std::condition_variable cv;
    size_t max_size = 10;
    bool initialized = false;
    std::string config_key;
};

RedisPool::RedisPool() : impl_(std::make_unique<Impl>()) {}

RedisPool::~RedisPool() = default;

bool RedisPool::initialize(std::string_view config_key) {
    std::lock_guard lock(impl_->mutex);
    impl_->config_key = std::string(config_key);

    // Read from config
    auto& cfg = shield::config::global_config();

    std::string prefix(config_key);
    std::string host = cfg.get_string(prefix + ".host",
                                     "localhost");
    int port = static_cast<int>(cfg.get_int(prefix + ".port", 6379));
    int db = static_cast<int>(cfg.get_int(prefix + ".db", 0));

    auto& log = shield::log::get_logger("redis");
    SHIELD_LOG_INFO(log, "Redis config: " + host + ":" + std::to_string(port) +
                    "/" + std::to_string(db));

    // Try to connect to real Redis.
    try {
        sw::redis::ConnectionOptions opts;
        opts.host = host;
        opts.port = port;
        opts.db = db;

        // Test connection with a ping.
        sw::redis::Redis test_redis(opts);
        test_redis.ping();

        // Connection successful; create real connections.
        for (size_t i = 0; i < impl_->max_size; ++i) {
            auto conn = std::make_shared<RealRedisConnection>(opts);
            impl_->connections.push_back(conn);
            impl_->available.push(conn);
        }
        SHIELD_LOG_INFO(log, "Redis connected to " + host + ":" +
                        std::to_string(port));
    } catch (const std::exception& e) {
        // Fall back to mock connections.
        SHIELD_LOG_WARNING(log, "Redis connection failed (" + std::string(e.what()) +
                           "), using mock pool");
        for (size_t i = 0; i < impl_->max_size; ++i) {
            auto conn = std::make_shared<MockRedisConnection>();
            impl_->connections.push_back(conn);
            impl_->available.push(conn);
        }
    }

    impl_->initialized = true;
    return true;
}

std::shared_ptr<RedisConnection> RedisPool::acquire() {
    std::unique_lock lock(impl_->mutex);

    impl_->cv.wait(lock, [this]() {
        return !impl_->available.empty() || !impl_->initialized;
    });

    if (!impl_->initialized || impl_->available.empty()) {
        return nullptr;
    }

    auto conn = impl_->available.front();
    impl_->available.pop();
    return conn;
}

std::pair<bool, std::string> RedisPool::get(std::string_view key) {
    auto conn = acquire();
    if (!conn) {
        return {false, ""};
    }
    auto result = conn->get(key);
    release(std::move(conn));
    return result;
}

bool RedisPool::set(std::string_view key, std::string_view value,
                    int ttl_seconds) {
    auto conn = acquire();
    if (!conn) {
        return false;
    }
    const bool result = conn->set(key, value, ttl_seconds);
    release(std::move(conn));
    return result;
}

int RedisPool::del(std::string_view key) {
    auto conn = acquire();
    if (!conn) {
        return 0;
    }
    const int result = conn->del(key);
    release(std::move(conn));
    return result;
}

bool RedisPool::exists(std::string_view key) {
    auto conn = acquire();
    if (!conn) {
        return false;
    }
    const bool result = conn->exists(key);
    release(std::move(conn));
    return result;
}

int RedisPool::publish(std::string_view channel, std::string_view message) {
    auto conn = acquire();
    if (!conn) {
        return 0;
    }
    const int result = conn->publish(channel, message);
    release(std::move(conn));
    return result;
}

bool RedisPool::subscribe(std::string_view channel,
                          RedisConnection::SubscribeCallback callback) {
    auto conn = acquire();
    if (!conn) {
        return false;
    }
    const bool result = conn->subscribe(channel, callback);
    release(std::move(conn));
    return result;
}

bool RedisPool::unsubscribe(std::string_view channel) {
    auto conn = acquire();
    if (!conn) {
        return false;
    }
    const bool result = conn->unsubscribe(channel);
    release(std::move(conn));
    return result;
}

void RedisPool::release(std::shared_ptr<RedisConnection> conn) {
    if (!conn) {
        return;
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->initialized) {
            impl_->available.push(std::move(conn));
        }
    }
    impl_->cv.notify_one();
}

bool RedisPool::is_initialized() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->initialized;
}

// =============================================================================
// Global instances
// =============================================================================

static DatabasePool* g_database = nullptr;
static std::unique_ptr<DatabasePool> g_database_owner;

static RedisPool* g_redis = nullptr;
static std::unique_ptr<RedisPool> g_redis_owner;

DatabasePool& database() {
    if (!g_database) {
        g_database_owner = std::make_unique<DatabasePool>();
        g_database = g_database_owner.get();
    }
    return *g_database;
}

RedisPool& redis() {
    if (!g_redis) {
        g_redis_owner = std::make_unique<RedisPool>();
        g_redis = g_redis_owner.get();
    }
    return *g_redis;
}

void set_mock_db_error(std::string error) {
    MockDatabaseConnection::force_error = std::move(error);
}

}  // namespace shield::data
