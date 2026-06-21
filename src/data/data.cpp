// [SHIELD_DATA] Data access implementation
#include "shield/data/data.hpp"

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/plugin/database.h"
#include "shield/plugin/plugin_host.hpp"

#include "dynamic_library.hpp"

#include <sw/redis++/redis++.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
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
    static inline MockDbOperation last_operation;
    static inline std::mutex operation_mutex;

    QueryResult query(std::string_view sql,
                     const std::vector<std::string>& params) override {
        record_operation("query", sql, params);
        if (!force_error.empty()) {
            return QueryResult::error(force_error);
        }
        return QueryResult::ok();
    }

    QueryResult query_one(std::string_view sql,
                         const std::vector<std::string>& params) override {
        record_operation("query_one", sql, params);
        if (!force_error.empty()) {
            return QueryResult::error(force_error);
        }
        return QueryResult::ok();
    }

    QueryResult execute(std::string_view sql,
                       const std::vector<std::string>& params) override {
        record_operation("execute", sql, params);
        if (!force_error.empty()) {
            return QueryResult::error(force_error);
        }
        auto result = QueryResult::ok();
        result.affected_rows = 1;
        result.last_insert_id = 1;
        return result;
    }

    QueryResult begin_transaction() override {
        if (!force_error.empty()) {
            return QueryResult::error(force_error);
        }
        in_transaction_ = true;
        return QueryResult::ok();
    }

    QueryResult commit_transaction() override {
        if (!force_error.empty()) {
            return QueryResult::error(force_error);
        }
        if (!in_transaction_) {
            return QueryResult::error("no active transaction",
                                      "transaction_aborted");
        }
        in_transaction_ = false;
        return QueryResult::ok();
    }

    QueryResult rollback_transaction() override {
        if (!force_error.empty()) {
            return QueryResult::error(force_error);
        }
        in_transaction_ = false;
        return QueryResult::ok();
    }

    bool ping() override {
        return true;
    }

    void close() override {}

private:
    static void record_operation(std::string method,
                                 std::string_view sql,
                                 const std::vector<std::string>& params) {
        std::lock_guard lock(operation_mutex);
        last_operation.method = std::move(method);
        last_operation.sql = std::string(sql);
        last_operation.params = params;
    }

    bool in_transaction_ = false;
};

// =============================================================================
// Plugin-backed DatabaseConnection
// =============================================================================
//
// Wraps a shield_database_v1 handle (resolved by PluginHost) and adapts
// the C ABI to the C++ DatabaseConnection interface. All plugin memory
// ownership is handled here: result structs are freed via the plugin's
// free_result callback, and the underlying DynamicLibrary is kept alive
// via shared_ptr so the plugin code stays mapped until every connection
// is destroyed.

namespace {

// Translate a plugin result struct into the C++ QueryResult used by the
// rest of shield_data. Does NOT call free_result - caller must.
QueryResult translate_plugin_result(const shield_db_result& r) {
    QueryResult qr;
    qr.success = (r.success == 1);
    qr.error_message = r.error_msg ? r.error_msg : "";
    qr.error_code = r.error_code ? r.error_code : (qr.success ? "" : "db_query_failed");
    qr.affected_rows = r.affected_rows;
    qr.last_insert_id = r.last_insert_id;
    if (r.cells && r.row_count > 0 && r.col_count > 0) {
        qr.rows.reserve(static_cast<size_t>(r.row_count));
        for (int i = 0; i < r.row_count; ++i) {
            Row row;
            for (int j = 0; j < r.col_count; ++j) {
                const char* cell = r.cells[static_cast<size_t>(i) * r.col_count + j];
                std::string col = "col_" + std::to_string(j);
                row[col] = cell ? cell : "";
            }
            qr.rows.push_back(std::move(row));
        }
    }
    return qr;
}

// Map a hard plugin return code (non-zero) into a stable error string.
// Plugins are expected to fill out_result->error_code for SQL-level
// failures; this is only for transport/protocol level disasters.
const char* plugin_rc_to_code(int rc) {
    return rc == 0 ? "" : "connection_lost";
}

}  // namespace

class PluginDatabaseConnection : public DatabaseConnection {
public:
    PluginDatabaseConnection(const shield_database_v1* plugin,
                             shield_db_conn* handle)
        : plugin_(plugin)
        , handle_(handle) {}

    ~PluginDatabaseConnection() override {
        if (handle_ && plugin_ && plugin_->disconnect) {
            plugin_->disconnect(handle_);
        }
    }

    PluginDatabaseConnection(const PluginDatabaseConnection&) = delete;
    PluginDatabaseConnection& operator=(const PluginDatabaseConnection&) = delete;

    QueryResult query(std::string_view sql,
                      const std::vector<std::string>& params) override {
        std::vector<const char*> p;
        p.reserve(params.size());
        for (const auto& s : params) p.push_back(s.c_str());

        shield_db_result r{};
        std::string sql_str(sql);
        int rc = plugin_->query(handle_, sql_str.c_str(),
                                p.empty() ? nullptr : p.data(),
                                static_cast<int>(p.size()), &r);
        QueryResult qr;
        if (rc != 0) {
            qr = QueryResult::error("plugin transport error",
                                    plugin_rc_to_code(rc));
            last_error_code_ = qr.error_code;
        } else {
            qr = translate_plugin_result(r);
            last_error_code_ = qr.success ? std::string{}
                                          : (r.error_code ? r.error_code
                                                          : "db_query_failed");
        }
        if (plugin_->free_result) plugin_->free_result(&r);
        return qr;
    }

    QueryResult query_one(std::string_view sql,
                          const std::vector<std::string>& params) override {
        auto result = query(sql, params);
        if (result.success && result.rows.size() > 1) {
            result.rows.resize(1);
        }
        return result;
    }

    QueryResult execute(std::string_view sql,
                        const std::vector<std::string>& params) override {
        std::vector<const char*> p;
        p.reserve(params.size());
        for (const auto& s : params) p.push_back(s.c_str());

        shield_db_result r{};
        std::string sql_str(sql);
        int rc = plugin_->execute(handle_, sql_str.c_str(),
                                  p.empty() ? nullptr : p.data(),
                                  static_cast<int>(p.size()), &r);
        QueryResult qr;
        if (rc != 0) {
            qr = QueryResult::error("plugin transport error",
                                    plugin_rc_to_code(rc));
            last_error_code_ = qr.error_code;
        } else {
            qr = translate_plugin_result(r);
            last_error_code_ = qr.success ? std::string{}
                                          : (r.error_code ? r.error_code
                                                          : "db_query_failed");
        }
        if (plugin_->free_result) plugin_->free_result(&r);
        return qr;
    }

    QueryResult begin_transaction() override {
        return run_txn("begin", plugin_->begin);
    }
    QueryResult commit_transaction() override {
        return run_txn("commit", plugin_->commit);
    }
    QueryResult rollback_transaction() override {
        return run_txn("rollback", plugin_->rollback);
    }

    bool ping() override {
        if (!plugin_->ping) return true;
        int rc = plugin_->ping(handle_);
        if (rc != 1) last_error_code_ = "connection_lost";
        return rc == 1;
    }

    void close() override {
        if (handle_ && plugin_->disconnect) {
            plugin_->disconnect(handle_);
            handle_ = nullptr;
        }
    }

    std::string last_error_code() const { return last_error_code_; }

private:
    using txn_fn = int (*)(shield_db_conn*, shield_db_result*);
    QueryResult run_txn(const char* /*op*/, txn_fn fn) {
        shield_db_result r{};
        int rc = fn(handle_, &r);
        QueryResult qr;
        if (rc != 0) {
            qr = QueryResult::error("plugin transport error",
                                    plugin_rc_to_code(rc));
            last_error_code_ = qr.error_code;
        } else {
            qr = translate_plugin_result(r);
            last_error_code_ = qr.success ? std::string{}
                                          : (r.error_code ? r.error_code
                                                          : "transaction_aborted");
        }
        if (plugin_->free_result) plugin_->free_result(&r);
        return qr;
    }

    const shield_database_v1* plugin_;  // vtable; PluginHost owns the library
    shield_db_conn* handle_;
    std::string last_error_code_;
};

struct DatabasePool::Impl {
    std::vector<std::shared_ptr<DatabaseConnection>> connections;
    std::queue<std::shared_ptr<DatabaseConnection>> available;
    std::mutex mutex;
    std::condition_variable cv;
    std::function<std::shared_ptr<DatabaseConnection>()> factory;
    std::chrono::milliseconds acquire_timeout{5000};
    size_t max_size = 10;
    size_t current_size = 0;
    bool initialized = false;
    bool mock = false;
    std::string config_key;
    std::string last_error_code;

    // Database provider vtable, resolved from PluginHost binding
    // "database.default". The host owns the underlying shared library, so
    // it stays mapped as long as PluginHost is alive (whole process).
    const shield_database_v1* plugin = nullptr;
    std::string driver_name;
};

DatabasePool::DatabasePool() : impl_(std::make_unique<Impl>()) {}

DatabasePool::~DatabasePool() {
    std::vector<std::shared_ptr<DatabaseConnection>> connections;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->initialized = false;
        std::swap(connections, impl_->connections);
        while (!impl_->available.empty()) {
            impl_->available.pop();
        }
        impl_->current_size = 0;
    }
    impl_->cv.notify_all();
    for (auto& conn : connections) {
        if (conn) {
            conn->close();
        }
    }
}

bool DatabasePool::initialize(std::string_view config_key) {
    std::lock_guard lock(impl_->mutex);
    impl_->config_key = std::string(config_key);
    impl_->initialized = false;
    impl_->last_error_code.clear();
    for (auto& conn : impl_->connections) {
        if (conn) {
            conn->close();
        }
    }
    impl_->connections.clear();
    while (!impl_->available.empty()) {
        impl_->available.pop();
    }
    impl_->current_size = 0;

    // Read from config
    auto& cfg = shield::config::global_config();

    std::string prefix(config_key);
    std::string host = cfg.get_string(prefix + ".host", "localhost");
    int port = static_cast<int>(cfg.get_int(prefix + ".port", 3306));
    std::string database = cfg.get_string(prefix + ".database", "shield");
    std::string user = cfg.get_string(prefix + ".username",
                                      cfg.get_string(prefix + ".user", "root"));
    std::string password = cfg.get_string(prefix + ".password", "");
    // Driver is now determined by which package the "database.default"
    // binding points at; this config value is only used for log labels.
    std::string driver = cfg.get_string(prefix + ".driver", "plugin");
    const bool has_enabled = cfg.has(prefix + ".enabled");
    const bool mock = cfg.get_bool(prefix + ".mock", !has_enabled);
    const bool allow_mock_fallback =
        cfg.get_bool(prefix + ".allow_mock_fallback", false);
    const size_t pool_size = static_cast<size_t>(
        std::max<int64_t>(1, cfg.get_int(prefix + ".pool_size", 1)));
    const size_t max_pool_size = static_cast<size_t>(
        std::max<int64_t>(static_cast<int64_t>(pool_size),
                          cfg.get_int(prefix + ".max_pool_size",
                                      static_cast<int64_t>(pool_size))));
    const auto acquire_timeout_ms = std::max<int64_t>(
        1, cfg.get_int(prefix + ".acquire_timeout", 5000));
    const auto connect_timeout_ms = std::max<int64_t>(
        1, cfg.get_int(prefix + ".connect_timeout", 5000));
    const auto query_timeout_ms = std::max<int64_t>(
        1, cfg.get_int(prefix + ".query_timeout", 30000));

    auto& log = shield::log::get_logger("database");
    SHIELD_LOG_INFO(log, "Database config: driver=" + driver + " " +
                             host + ":" + std::to_string(port) + "/" + database +
                             " pool=" + std::to_string(pool_size) + "/" +
                             std::to_string(max_pool_size) +
                             (mock ? " mock=true" : ""));

    impl_->max_size = max_pool_size;
    impl_->acquire_timeout = std::chrono::milliseconds(acquire_timeout_ms);
    impl_->mock = mock;
    impl_->driver_name = driver;

    if (mock) {
        impl_->factory = []() {
            return std::make_shared<MockDatabaseConnection>();
        };
        for (size_t i = 0; i < pool_size; ++i) {
            auto conn = impl_->factory();
            impl_->connections.push_back(conn);
            impl_->available.push(conn);
        }
        impl_->current_size = pool_size;
        impl_->initialized = true;
        impl_->cv.notify_all();
        SHIELD_LOG_INFO(log, "Database mock pool initialized");
        return true;
    }

    // -------------------------------------------------------------------------
    // Plugin source: the shield.database.v1 provider resolved by PluginHost
    // under the "database.default" binding (configured in the app.yaml
    // `plugins` section). The host owns the shared library lifecycle; the
    // pool only holds the vtable pointer. If no binding is configured,
    // impl_->plugin stays null and the fallback below (mock, when allowed)
    // applies — so configs without a database plugin still boot.
    // -------------------------------------------------------------------------
    const std::string plugin_name = "database.default";
    impl_->plugin = shield::plugin::global_host()
                        .get_by_binding<shield_database_v1>("database.default");
    if (impl_->plugin) {
        impl_->driver_name =
            impl_->plugin->name ? impl_->plugin->name : "plugin";
        SHIELD_LOG_INFO(log, "Database provider resolved via binding '" +
                                 plugin_name + "' v" +
                                 (impl_->plugin->version ? impl_->plugin->version : "?"));
    }

    if (!impl_->plugin) {
        // Either no DLL found, or ABI mismatch.
        if (impl_->last_error_code.empty()) {
            impl_->last_error_code = "module_unavailable";
        }
        if (!allow_mock_fallback) {
            SHIELD_LOG_ERROR(log, "No DB plugin available for driver '" +
                                      driver + "' (looked for " + plugin_name +
                                      ".dll/.so/.dylib)");
            return false;
        }
        SHIELD_LOG_WARNING(log, "DB plugin '" + plugin_name +
                                    "' not found; falling back to mock because " +
                                    prefix + ".allow_mock_fallback=true");
        impl_->mock = true;
        impl_->factory = []() {
            return std::make_shared<MockDatabaseConnection>();
        };
        for (size_t i = 0; i < pool_size; ++i) {
            auto conn = impl_->factory();
            impl_->connections.push_back(conn);
            impl_->available.push(conn);
        }
        impl_->current_size = pool_size;
        impl_->initialized = true;
        impl_->cv.notify_all();
        return true;
    }

    // Build the connect args once and reuse for every pooled connection.
    auto connect_args = std::make_shared<shield_db_connect_args>();
    connect_args->host = host.c_str();
    connect_args->port = port;
    connect_args->user = user.c_str();
    connect_args->password = password.c_str();
    connect_args->database = database.c_str();
    connect_args->extra_json = nullptr;
    connect_args->connect_timeout_ms = static_cast<int>(connect_timeout_ms);
    connect_args->query_timeout_ms = static_cast<int>(query_timeout_ms);

    auto* plugin_ptr = impl_->plugin;

    auto real_factory = [plugin_ptr, connect_args,
                         host, port, database]() {
        char err_buf[512] = {};
        shield_db_conn* h = plugin_ptr->connect(connect_args.get(),
                                                err_buf, sizeof(err_buf));
        if (!h) {
            throw std::runtime_error(err_buf[0] ? err_buf
                                                : "plugin connect returned NULL");
        }
        return std::make_shared<PluginDatabaseConnection>(plugin_ptr, h);
    };

    try {
        auto test_conn = real_factory();
        // Probe with a SELECT 1 (most engines support this; SQLite, MySQL,
        // PostgreSQL all do). If it fails we treat the plugin as broken.
        auto test_result = test_conn->query("SELECT 1", {});
        if (!test_result.success) {
            throw std::runtime_error(test_result.error_message);
        }

        impl_->factory = std::move(real_factory);
        impl_->connections.push_back(test_conn);
        impl_->available.push(test_conn);
        for (size_t i = 1; i < pool_size; ++i) {
            auto conn = impl_->factory();
            impl_->connections.push_back(conn);
            impl_->available.push(conn);
        }
        impl_->current_size = pool_size;
        impl_->initialized = true;
        impl_->cv.notify_all();
        SHIELD_LOG_INFO(log, "DB pool connected via plugin '" +
                                 plugin_name + "' to " + host + ":" +
                                 std::to_string(port) + "/" + database);
        return true;
    } catch (const std::exception& e) {
        impl_->last_error_code = "connection_lost";
        if (!allow_mock_fallback) {
            SHIELD_LOG_ERROR(log, "DB connect via plugin '" + plugin_name +
                                      "' failed: " + std::string(e.what()));
            return false;
        }

        SHIELD_LOG_WARNING(log, "DB connect via plugin '" + plugin_name +
                                    "' failed (" + std::string(e.what()) +
                                    "), using mock pool because " + prefix +
                                    ".allow_mock_fallback=true");
        impl_->mock = true;
        impl_->factory = []() {
            return std::make_shared<MockDatabaseConnection>();
        };
        for (size_t i = 0; i < pool_size; ++i) {
            auto conn = impl_->factory();
            impl_->connections.push_back(conn);
            impl_->available.push(conn);
        }
        impl_->current_size = pool_size;
        impl_->initialized = true;
        impl_->cv.notify_all();
        return true;
    }
}

std::shared_ptr<DatabaseConnection> DatabasePool::acquire() {
    std::unique_lock lock(impl_->mutex);

    if (!impl_->initialized) {
        impl_->last_error_code = "module_unavailable";
        return nullptr;
    }

    if (impl_->available.empty() && impl_->current_size < impl_->max_size) {
        auto factory = impl_->factory;
        ++impl_->current_size;
        lock.unlock();
        try {
            auto conn = factory();
            lock.lock();
            impl_->connections.push_back(conn);
            lock.unlock();
            auto* raw = conn.get();
            return std::shared_ptr<DatabaseConnection>(
                raw,
                [this, conn = std::move(conn)](DatabaseConnection*) mutable {
                    release(std::move(conn));
                });
        } catch (const std::exception& e) {
            lock.lock();
            --impl_->current_size;
            impl_->last_error_code = "connection_lost";
            lock.unlock();
            impl_->cv.notify_one();
            auto& log = shield::log::get_logger("database");
            SHIELD_LOG_ERROR(log, "Failed to create database connection: " +
                                      std::string(e.what()));
            return nullptr;
        }
    }

    const bool ready = impl_->cv.wait_for(lock, impl_->acquire_timeout, [this]() {
        return !impl_->available.empty() || !impl_->initialized;
    });

    if (!ready || !impl_->initialized || impl_->available.empty()) {
        impl_->last_error_code = ready ? "module_unavailable" : "pool_exhausted";
        return nullptr;
    }

    auto conn = impl_->available.front();
    impl_->available.pop();
    auto* raw = conn.get();
    return std::shared_ptr<DatabaseConnection>(
        raw,
        [this, conn = std::move(conn)](DatabaseConnection*) mutable {
            release(std::move(conn));
        });
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
        return QueryResult::error("No database connection available",
                                  last_error_code().empty() ? "pool_exhausted"
                                                            : last_error_code());
    }
    auto result = conn->query(sql, params);
    return result;
}

QueryResult DatabasePool::query_one(std::string_view sql,
                                    const std::vector<std::string>& params) {
    auto conn = acquire();
    if (!conn) {
        return QueryResult::error("No database connection available",
                                  last_error_code().empty() ? "pool_exhausted"
                                                            : last_error_code());
    }
    auto result = conn->query_one(sql, params);
    return result;
}

QueryResult DatabasePool::execute(std::string_view sql,
                                  const std::vector<std::string>& params) {
    auto conn = acquire();
    if (!conn) {
        return QueryResult::error("No database connection available",
                                  last_error_code().empty() ? "pool_exhausted"
                                                            : last_error_code());
    }
    auto result = conn->execute(sql, params);
    return result;
}

bool DatabasePool::is_initialized() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->initialized;
}

std::string DatabasePool::last_error_code() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->last_error_code;
}

// =============================================================================
// Redis Implementation
// =============================================================================

// Real RedisConnection implementation using redis++.
class RealRedisConnection : public RedisConnection {
public:
    explicit RealRedisConnection(const sw::redis::ConnectionOptions& opts)
        : redis_(opts) {}

    std::string last_error_code() const override { return last_error_code_; }

    std::pair<bool, std::string> get(std::string_view key) override {
        try {
            last_error_code_.clear();
            auto val = redis_.get(std::string(key));
            if (val) {
                return {true, *val};
            }
            return {false, ""};
        } catch (const std::exception& e) {
            last_error_code_ = map_error_code(e);
            return {false, ""};
        }
    }

    bool set(std::string_view key, std::string_view value,
             int ttl_seconds) override {
        try {
            last_error_code_.clear();
            if (ttl_seconds > 0) {
                return redis_.set(std::string(key), std::string(value),
                                  std::chrono::milliseconds(ttl_seconds * 1000));
            }
            return redis_.set(std::string(key), std::string(value));
        } catch (const std::exception& e) {
            last_error_code_ = map_error_code(e);
            return false;
        }
    }

    int del(std::string_view key) override {
        try {
            last_error_code_.clear();
            return static_cast<int>(redis_.del(std::string(key)));
        } catch (const std::exception& e) {
            last_error_code_ = map_error_code(e);
            return 0;
        }
    }

    bool exists(std::string_view key) override {
        try {
            last_error_code_.clear();
            return redis_.exists(std::string(key)) > 0;
        } catch (const std::exception& e) {
            last_error_code_ = map_error_code(e);
            return false;
        }
    }

    int publish(std::string_view channel, std::string_view message) override {
        try {
            last_error_code_.clear();
            return static_cast<int>(
                redis_.publish(std::string(channel), std::string(message)));
        } catch (const std::exception& e) {
            last_error_code_ = map_error_code(e);
            return 0;
        }
    }

    bool subscribe(std::string_view channel,
                   SubscribeCallback callback) override {
        try {
            last_error_code_.clear();
            auto sub = redis_.subscriber();
            sub.on_message([callback](std::string ch, std::string msg) {
                callback(ch, msg);
            });
            sub.subscribe(std::string(channel));
            subscriber_thread_ = std::thread([sub = std::move(sub)]() mutable {
                try {
                    while (true) {
                        sub.consume();
                    }
                } catch (...) {}
            });
            return true;
        } catch (const std::exception& e) {
            last_error_code_ = map_error_code(e);
            return false;
        }
    }

    bool unsubscribe(std::string_view channel) override {
        try {
            if (subscriber_thread_.joinable()) {
                subscriber_thread_.detach();
            }
            return true;
        } catch (const std::exception& e) {
            last_error_code_ = map_error_code(e);
            return false;
        }
    }

    bool ping() override {
        try {
            last_error_code_.clear();
            redis_.ping();
            return true;
        } catch (const std::exception& e) {
            last_error_code_ = map_error_code(e);
            return false;
        }
    }

    void close() override {
        if (subscriber_thread_.joinable()) {
            subscriber_thread_.detach();
        }
    }

private:
    // Map redis++ exception to stable error code.
    static std::string map_error_code(const std::exception& e) {
        if (dynamic_cast<const sw::redis::TimeoutError*>(&e)) return "command_timeout";
        if (dynamic_cast<const sw::redis::ClosedError*>(&e)) return "connection_lost";
        if (dynamic_cast<const sw::redis::IoError*>(&e)) return "connection_lost";
        if (dynamic_cast<const sw::redis::OomError*>(&e)) return "pool_exhausted";
        if (dynamic_cast<const sw::redis::ReplyError*>(&e)) {
            // Check for WRONGTYPE errors.
            std::string msg = e.what();
            if (msg.find("WRONGTYPE") != std::string::npos) return "wrong_type";
            return "redis_command_failed";
        }
        return "redis_command_failed";
    }

    sw::redis::Redis redis_;
    std::thread subscriber_thread_;
    std::string last_error_code_ = "redis_command_failed";
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
    std::function<std::shared_ptr<RedisConnection>()> factory;
    std::chrono::milliseconds acquire_timeout{5000};
    size_t max_size = 10;
    size_t current_size = 0;
    bool initialized = false;
    bool mock = false;
    std::string config_key;
    std::string last_error_code;  // track last operation's error code
};

RedisPool::RedisPool() : impl_(std::make_unique<Impl>()) {}

RedisPool::~RedisPool() {
    std::vector<std::shared_ptr<RedisConnection>> connections;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->initialized = false;
        std::swap(connections, impl_->connections);
        while (!impl_->available.empty()) {
            impl_->available.pop();
        }
        impl_->current_size = 0;
    }
    impl_->cv.notify_all();
    for (auto& conn : connections) {
        if (conn) {
            conn->close();
        }
    }
}

bool RedisPool::initialize(std::string_view config_key) {
    std::lock_guard lock(impl_->mutex);
    impl_->config_key = std::string(config_key);
    impl_->initialized = false;
    impl_->last_error_code.clear();
    for (auto& conn : impl_->connections) {
        if (conn) {
            conn->close();
        }
    }
    impl_->connections.clear();
    while (!impl_->available.empty()) {
        impl_->available.pop();
    }
    impl_->current_size = 0;

    // Read from config
    auto& cfg = shield::config::global_config();

    std::string prefix(config_key);
    std::string host = cfg.get_string(prefix + ".host",
                                     "localhost");
    int port = static_cast<int>(cfg.get_int(prefix + ".port", 6379));
    int db = static_cast<int>(cfg.get_int(prefix + ".db", 0));
    std::string password = cfg.get_string(prefix + ".password", "");
    const bool has_enabled = cfg.has(prefix + ".enabled");
    const bool mock = cfg.get_bool(prefix + ".mock", !has_enabled);
    const bool allow_mock_fallback =
        cfg.get_bool(prefix + ".allow_mock_fallback", false);
    const size_t pool_size = static_cast<size_t>(
        std::max<int64_t>(1, cfg.get_int(prefix + ".pool_size", 1)));
    const size_t max_pool_size = static_cast<size_t>(
        std::max<int64_t>(static_cast<int64_t>(pool_size),
                          cfg.get_int(prefix + ".max_pool_size",
                                      static_cast<int64_t>(pool_size))));
    const auto acquire_timeout_ms = std::max<int64_t>(
        1, cfg.get_int(prefix + ".acquire_timeout", 5000));
    const auto command_timeout_ms = std::max<int64_t>(
        1, cfg.get_int(prefix + ".command_timeout", 5000));
    const auto connect_timeout_ms = std::max<int64_t>(
        1, cfg.get_int(prefix + ".connect_timeout", 5000));

    auto& log = shield::log::get_logger("redis");
    SHIELD_LOG_INFO(log, "Redis config: " + host + ":" + std::to_string(port) +
                             "/" + std::to_string(db) +
                             " pool=" + std::to_string(pool_size) + "/" +
                             std::to_string(max_pool_size) +
                             (mock ? " mock=true" : ""));

    impl_->max_size = max_pool_size;
    impl_->acquire_timeout = std::chrono::milliseconds(acquire_timeout_ms);
    impl_->mock = mock;

    if (mock) {
        impl_->factory = []() {
            return std::make_shared<MockRedisConnection>();
        };
        for (size_t i = 0; i < pool_size; ++i) {
            auto conn = impl_->factory();
            impl_->connections.push_back(conn);
            impl_->available.push(conn);
        }
        impl_->current_size = pool_size;
        impl_->initialized = true;
        impl_->cv.notify_all();
        SHIELD_LOG_INFO(log, "Redis mock pool initialized");
        return true;
    }

    sw::redis::ConnectionOptions opts;
    opts.host = host;
    opts.port = port;
    opts.db = db;
    opts.connect_timeout = std::chrono::milliseconds(connect_timeout_ms);
    opts.socket_timeout = std::chrono::milliseconds(command_timeout_ms);
    if (!password.empty()) {
        opts.password = password;
    }
    auto real_factory = [opts]() {
        return std::make_shared<RealRedisConnection>(opts);
    };

    try {
        // Test connection with a ping.
        sw::redis::Redis test_redis(opts);
        test_redis.ping();

        impl_->factory = std::move(real_factory);
        for (size_t i = 0; i < pool_size; ++i) {
            auto conn = impl_->factory();
            impl_->connections.push_back(conn);
            impl_->available.push(conn);
        }
        impl_->current_size = pool_size;
        impl_->initialized = true;
        impl_->cv.notify_all();
        SHIELD_LOG_INFO(log, "Redis connected to " + host + ":" +
                        std::to_string(port));
        return true;
    } catch (const std::exception& e) {
        impl_->last_error_code = "connection_lost";
        if (!allow_mock_fallback) {
            SHIELD_LOG_ERROR(log, "Redis connection failed: " +
                                      std::string(e.what()));
            return false;
        }

        SHIELD_LOG_WARNING(log, "Redis connection failed (" +
                                    std::string(e.what()) +
                                    "), using mock pool because " + prefix +
                                    ".allow_mock_fallback=true");
        impl_->mock = true;
        impl_->factory = []() {
            return std::make_shared<MockRedisConnection>();
        };
        for (size_t i = 0; i < pool_size; ++i) {
            auto conn = impl_->factory();
            impl_->connections.push_back(conn);
            impl_->available.push(conn);
        }
        impl_->current_size = pool_size;
        impl_->initialized = true;
        impl_->cv.notify_all();
        return true;
    }
}

std::shared_ptr<RedisConnection> RedisPool::acquire() {
    std::unique_lock lock(impl_->mutex);

    if (!impl_->initialized) {
        impl_->last_error_code = "module_unavailable";
        return nullptr;
    }

    if (impl_->available.empty() && impl_->current_size < impl_->max_size) {
        auto factory = impl_->factory;
        ++impl_->current_size;
        lock.unlock();
        try {
            auto conn = factory();
            lock.lock();
            impl_->connections.push_back(conn);
            lock.unlock();
            auto* raw = conn.get();
            return std::shared_ptr<RedisConnection>(
                raw,
                [this, conn = std::move(conn)](RedisConnection*) mutable {
                    release(std::move(conn));
                });
        } catch (const std::exception& e) {
            lock.lock();
            --impl_->current_size;
            impl_->last_error_code = "connection_lost";
            lock.unlock();
            impl_->cv.notify_one();
            auto& log = shield::log::get_logger("redis");
            SHIELD_LOG_ERROR(log, "Failed to create Redis connection: " +
                                      std::string(e.what()));
            return nullptr;
        }
    }

    const bool ready = impl_->cv.wait_for(lock, impl_->acquire_timeout, [this]() {
        return !impl_->available.empty() || !impl_->initialized;
    });

    if (!ready || !impl_->initialized || impl_->available.empty()) {
        impl_->last_error_code = ready ? "module_unavailable" : "pool_exhausted";
        return nullptr;
    }

    auto conn = impl_->available.front();
    impl_->available.pop();
    auto* raw = conn.get();
    return std::shared_ptr<RedisConnection>(
        raw,
        [this, conn = std::move(conn)](RedisConnection*) mutable {
            release(std::move(conn));
        });
}

std::pair<bool, std::string> RedisPool::get(std::string_view key) {
    auto conn = acquire();
    if (!conn) {
        std::lock_guard lock(impl_->mutex);
        if (impl_->last_error_code.empty()) {
            impl_->last_error_code = "pool_exhausted";
        }
        return {false, ""};
    }
    auto result = conn->get(key);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->last_error_code = conn->last_error_code();
    }
    return result;
}

bool RedisPool::set(std::string_view key, std::string_view value,
                    int ttl_seconds) {
    auto conn = acquire();
    if (!conn) {
        std::lock_guard lock(impl_->mutex);
        if (impl_->last_error_code.empty()) {
            impl_->last_error_code = "pool_exhausted";
        }
        return false;
    }
    const bool result = conn->set(key, value, ttl_seconds);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->last_error_code = conn->last_error_code();
    }
    return result;
}

int RedisPool::del(std::string_view key) {
    auto conn = acquire();
    if (!conn) {
        std::lock_guard lock(impl_->mutex);
        if (impl_->last_error_code.empty()) {
            impl_->last_error_code = "pool_exhausted";
        }
        return 0;
    }
    const int result = conn->del(key);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->last_error_code = conn->last_error_code();
    }
    return result;
}

bool RedisPool::exists(std::string_view key) {
    auto conn = acquire();
    if (!conn) {
        std::lock_guard lock(impl_->mutex);
        if (impl_->last_error_code.empty()) {
            impl_->last_error_code = "pool_exhausted";
        }
        return false;
    }
    const bool result = conn->exists(key);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->last_error_code = conn->last_error_code();
    }
    return result;
}

int RedisPool::publish(std::string_view channel, std::string_view message) {
    auto conn = acquire();
    if (!conn) {
        std::lock_guard lock(impl_->mutex);
        if (impl_->last_error_code.empty()) {
            impl_->last_error_code = "pool_exhausted";
        }
        return 0;
    }
    const int result = conn->publish(channel, message);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->last_error_code = conn->last_error_code();
    }
    return result;
}

bool RedisPool::subscribe(std::string_view channel,
                          RedisConnection::SubscribeCallback callback) {
    auto conn = acquire();
    if (!conn) {
        std::lock_guard lock(impl_->mutex);
        if (impl_->last_error_code.empty()) {
            impl_->last_error_code = "pool_exhausted";
        }
        return false;
    }
    const bool result = conn->subscribe(channel, callback);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->last_error_code = conn->last_error_code();
    }
    return result;
}

bool RedisPool::unsubscribe(std::string_view channel) {
    auto conn = acquire();
    if (!conn) {
        return false;
    }
    const bool result = conn->unsubscribe(channel);
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

std::string RedisPool::last_error_code() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->last_error_code;
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

MockDbOperation mock_db_last_operation() {
    std::lock_guard lock(MockDatabaseConnection::operation_mutex);
    return MockDatabaseConnection::last_operation;
}

void clear_mock_db_last_operation() {
    std::lock_guard lock(MockDatabaseConnection::operation_mutex);
    MockDatabaseConnection::last_operation = {};
}

}  // namespace shield::data
