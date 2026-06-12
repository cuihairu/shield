// [SHIELD_DATA] Data access module
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace shield::data {

/// @brief Row data (column name -> value)
using Row = std::unordered_map<std::string, std::string>;

/// @brief Query result
struct QueryResult {
    bool success = false;
    std::string error_message;
    std::vector<Row> rows;
    int64_t affected_rows = 0;
    int64_t last_insert_id = 0;

    static QueryResult ok(std::vector<Row> rows = {}) {
        return {true, "", std::move(rows), 0, 0};
    }

    static QueryResult error(std::string msg) {
        return {false, std::move(msg), {}, 0, 0};
    }
};

/// @brief Database connection interface
class DatabaseConnection {
public:
    virtual ~DatabaseConnection() = default;

    /// @brief Execute a query and return all rows
    virtual QueryResult query(std::string_view sql,
                             const std::vector<std::string>& params) = 0;

    /// @brief Execute a query and return at most one row
    virtual QueryResult query_one(std::string_view sql,
                                 const std::vector<std::string>& params) = 0;

    /// @brief Execute a statement (INSERT, UPDATE, DELETE)
    virtual QueryResult execute(std::string_view sql,
                               const std::vector<std::string>& params) = 0;

    /// @brief Check if connection is alive
    virtual bool ping() = 0;

    /// @brief Close the connection
    virtual void close() = 0;
};

/// @brief Database pool manager
class DatabasePool {
public:
    DatabasePool();
    ~DatabasePool();

    // Non-copyable
    DatabasePool(const DatabasePool&) = delete;
    DatabasePool& operator=(const DatabasePool&) = delete;

    /// @brief Initialize pool from config
    bool initialize(std::string_view config_key = "database");

    /// @brief Get a connection from the pool
    std::shared_ptr<DatabaseConnection> acquire();

    /// @brief Execute a query (convenience - acquires and releases connection)
    QueryResult query(std::string_view sql,
                     const std::vector<std::string>& params);

    QueryResult query_one(std::string_view sql,
                         const std::vector<std::string>& params);

    QueryResult execute(std::string_view sql,
                       const std::vector<std::string>& params);

    /// @brief Check if pool is initialized
    bool is_initialized() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief Redis connection interface
class RedisConnection {
public:
    virtual ~RedisConnection() = default;

    /// @brief Get a value
    virtual std::pair<bool, std::string> get(std::string_view key) = 0;

    /// @brief Set a value (with optional TTL in seconds)
    virtual bool set(std::string_view key, std::string_view value,
                    int ttl_seconds = 0) = 0;

    /// @brief Delete keys
    virtual int del(std::string_view key) = 0;

    /// @brief Publish to a channel
    virtual int publish(std::string_view channel, std::string_view message) = 0;

    /// @brief Subscribe to a channel
    using SubscribeCallback = std::function<void(std::string_view channel,
                                                  std::string_view message)>;
    virtual bool subscribe(std::string_view channel,
                           SubscribeCallback callback) = 0;

    /// @brief Unsubscribe from a channel
    virtual bool unsubscribe(std::string_view channel) = 0;

    /// @brief Check if connection is alive
    virtual bool ping() = 0;

    /// @brief Close the connection
    virtual void close() = 0;
};

/// @brief Redis pool manager
class RedisPool {
public:
    RedisPool();
    ~RedisPool();

    // Non-copyable
    RedisPool(const RedisPool&) = delete;
    RedisPool& operator=(const RedisPool&) = delete;

    /// @brief Initialize pool from config
    bool initialize(std::string_view config_key = "redis");

    /// @brief Get a connection from the pool
    std::shared_ptr<RedisConnection> acquire();

    /// @brief Execute a Redis command (convenience methods)
    std::pair<bool, std::string> get(std::string_view key);
    bool set(std::string_view key, std::string_view value, int ttl_seconds = 0);
    int del(std::string_view key);
    int publish(std::string_view channel, std::string_view message);
    bool subscribe(std::string_view channel,
                  RedisConnection::SubscribeCallback callback);
    bool unsubscribe(std::string_view channel);

    /// @brief Check if pool is initialized
    bool is_initialized() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief Get global database pool
DatabasePool& database();

/// @brief Get global Redis pool
RedisPool& redis();

}  // namespace shield::data
