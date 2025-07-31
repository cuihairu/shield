#include "shield/database/database_actor_service.hpp"

#include <chrono>
#include <iostream>
#include <thread>

// 这里需要包含MySQL C API
#ifdef SHIELD_USE_MYSQL
#include <mysql/mysql.h>
#endif

namespace shield::database {

// =====================================
// MySQL连接实现
// =====================================

MySQLConnection::MySQLConnection(const DatabaseConfig& config)
    : config_(config) {
#ifdef SHIELD_USE_MYSQL
    mysql_conn_ = mysql_init(nullptr);
    if (!mysql_conn_) {
        throw std::runtime_error("Failed to initialize MySQL connection");
    }
#endif
}

MySQLConnection::~MySQLConnection() { disconnect(); }

bool MySQLConnection::connect() {
#ifdef SHIELD_USE_MYSQL
    if (!mysql_conn_) return false;

    // 设置连接选项
    mysql_options(static_cast<MYSQL*>(mysql_conn_), MYSQL_OPT_CONNECT_TIMEOUT,
                  &config_.connection_timeout);
    mysql_options(static_cast<MYSQL*>(mysql_conn_), MYSQL_OPT_RECONNECT,
                  &config_.auto_reconnect);
    mysql_options(static_cast<MYSQL*>(mysql_conn_), MYSQL_SET_CHARSET_NAME,
                  config_.charset.c_str());

    // 连接数据库
    if (mysql_real_connect(static_cast<MYSQL*>(mysql_conn_),
                           config_.host.c_str(), config_.username.c_str(),
                           config_.password.c_str(), config_.database.c_str(),
                           config_.port, nullptr, CLIENT_MULTI_STATEMENTS)) {
        connected_ = true;
        std::cout << "[MySQL] Connected to " << config_.host << ":"
                  << config_.port << "/" << config_.database << std::endl;
        return true;
    } else {
        connected_ = false;
        std::cerr << "[MySQL] Connection failed: "
                  << mysql_error(static_cast<MYSQL*>(mysql_conn_)) << std::endl;
        return false;
    }
#else
    std::cout << "[MySQL] Mock connection to " << config_.host << ":"
              << config_.port << "/" << config_.database << std::endl;
    connected_ = true;
    return true;
#endif
}

void MySQLConnection::disconnect() {
#ifdef SHIELD_USE_MYSQL
    if (mysql_conn_) {
        mysql_close(static_cast<MYSQL*>(mysql_conn_));
        mysql_conn_ = nullptr;
    }
#endif
    connected_ = false;
}

QueryResult MySQLConnection::execute_query(
    const std::string& sql, const std::vector<std::string>& params) {
#ifdef SHIELD_USE_MYSQL
    if (!connected_) {
        return {false, "Not connected to database", {}, 0, 0};
    }

    check_connection();

    // 构建参数化查询 (简单实现，生产环境需要prepared statements)
    std::string final_sql = sql;
    for (size_t i = 0; i < params.size(); ++i) {
        size_t pos = final_sql.find('?');
        if (pos != std::string::npos) {
            std::string escaped_param = escape_string(params[i]);
            final_sql.replace(pos, 1, "'" + escaped_param + "'");
        }
    }

    // 执行查询
    if (mysql_real_query(static_cast<MYSQL*>(mysql_conn_), final_sql.c_str(),
                         final_sql.length()) != 0) {
        return handle_mysql_error();
    }

    QueryResult result;
    result.success = true;
    result.affected_rows =
        mysql_affected_rows(static_cast<MYSQL*>(mysql_conn_));
    result.last_insert_id = mysql_insert_id(static_cast<MYSQL*>(mysql_conn_));

    // 获取结果集
    MYSQL_RES* mysql_result =
        mysql_store_result(static_cast<MYSQL*>(mysql_conn_));
    if (mysql_result) {
        int num_fields = mysql_num_fields(mysql_result);
        MYSQL_FIELD* fields = mysql_fetch_fields(mysql_result);

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(mysql_result))) {
            std::unordered_map<std::string, std::string> row_data;
            for (int i = 0; i < num_fields; ++i) {
                std::string field_name = fields[i].name;
                std::string field_value = row[i] ? row[i] : "NULL";
                row_data[field_name] = field_value;
            }
            result.rows.push_back(std::move(row_data));
        }

        mysql_free_result(mysql_result);
    }

    return result;
#else
    // Mock implementation for development
    QueryResult result;
    result.success = true;

    if (sql.find("SELECT") == 0) {
        // Mock SELECT result
        result.rows = {{{"id", "1"}, {"name", "Player1"}, {"level", "10"}},
                       {{"id", "2"}, {"name", "Player2"}, {"level", "15"}}};
    } else if (sql.find("INSERT") == 0) {
        result.affected_rows = 1;
        result.last_insert_id = 123;
    } else if (sql.find("UPDATE") == 0 || sql.find("DELETE") == 0) {
        result.affected_rows = 1;
    }

    // 模拟数据库延迟
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    return result;
#endif
}

bool MySQLConnection::begin_transaction() {
#ifdef SHIELD_USE_MYSQL
    return mysql_real_query(static_cast<MYSQL*>(mysql_conn_),
                            "START TRANSACTION", 17) == 0;
#else
    return true;
#endif
}

bool MySQLConnection::commit_transaction() {
#ifdef SHIELD_USE_MYSQL
    return mysql_real_query(static_cast<MYSQL*>(mysql_conn_), "COMMIT", 6) == 0;
#else
    return true;
#endif
}

bool MySQLConnection::rollback_transaction() {
#ifdef SHIELD_USE_MYSQL
    return mysql_real_query(static_cast<MYSQL*>(mysql_conn_), "ROLLBACK", 8) ==
           0;
#else
    return true;
#endif
}

std::string MySQLConnection::escape_string(const std::string& str) {
#ifdef SHIELD_USE_MYSQL
    if (!connected_) return str;

    std::string escaped;
    escaped.resize(str.length() * 2 + 1);

    size_t escaped_length =
        mysql_real_escape_string(static_cast<MYSQL*>(mysql_conn_), &escaped[0],
                                 str.c_str(), str.length());

    escaped.resize(escaped_length);
    return escaped;
#else
    // Simple escape for mock
    std::string escaped = str;
    size_t pos = 0;
    while ((pos = escaped.find('\'', pos)) != std::string::npos) {
        escaped.replace(pos, 1, "\\'");
        pos += 2;
    }
    return escaped;
#endif
}

void MySQLConnection::check_connection() {
#ifdef SHIELD_USE_MYSQL
    if (mysql_ping(static_cast<MYSQL*>(mysql_conn_)) != 0) {
        std::cerr << "[MySQL] Connection lost, attempting to reconnect..."
                  << std::endl;
        connect();
    }
#endif
}

QueryResult MySQLConnection::handle_mysql_error() {
#ifdef SHIELD_USE_MYSQL
    std::string error_msg = mysql_error(static_cast<MYSQL*>(mysql_conn_));
    int error_no = mysql_errno(static_cast<MYSQL*>(mysql_conn_));

    std::cerr << "[MySQL] Query failed (" << error_no << "): " << error_msg
              << std::endl;

    return {false, error_msg, {}, 0, 0};
#else
    return {false, "Mock error", {}, 0, 0};
#endif
}

// =====================================
// 连接池实现
// =====================================

DatabaseConnectionPool::DatabaseConnectionPool(const DatabaseConfig& config)
    : config_(config), max_connections_(config.max_connections) {
    initialize_pool();
}

DatabaseConnectionPool::~DatabaseConnectionPool() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    while (!available_connections_.empty()) {
        available_connections_.pop();
    }
    all_connections_.clear();
}

void DatabaseConnectionPool::initialize_pool() {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    for (size_t i = 0; i < max_connections_; ++i) {
        auto conn = create_connection();
        if (conn && conn->connect()) {
            all_connections_.push_back(std::move(conn));
        }
    }

    // 将连接添加到可用队列
    for (auto& conn : all_connections_) {
        available_connections_.push(std::make_unique<MySQLConnection>(
            *dynamic_cast<MySQLConnection*>(conn.get())));
    }

    std::cout << "[ConnectionPool] Initialized pool with "
              << available_connections_.size() << " connections for "
              << config_.database << std::endl;
}

std::unique_ptr<IDatabaseConnection>
DatabaseConnectionPool::create_connection() {
    return std::make_unique<MySQLConnection>(config_);
}

DatabaseConnectionPool::ConnectionGuard DatabaseConnectionPool::get_connection(
    int timeout_ms) {
    std::unique_lock<std::mutex> lock(pool_mutex_);

    // 等待可用连接
    auto timeout = std::chrono::milliseconds(timeout_ms);
    if (!cv_.wait_for(lock, timeout,
                      [this] { return !available_connections_.empty(); })) {
        throw std::runtime_error("Connection pool timeout");
    }

    auto conn = std::move(available_connections_.front());
    available_connections_.pop();
    active_connections_++;

    return ConnectionGuard(this, std::move(conn));
}

void DatabaseConnectionPool::return_connection(
    std::unique_ptr<IDatabaseConnection> conn) {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    if (conn && conn->is_connected()) {
        available_connections_.push(std::move(conn));
    } else {
        // 连接已断开，创建新连接
        auto new_conn = create_connection();
        if (new_conn && new_conn->connect()) {
            available_connections_.push(std::move(new_conn));
        }
    }

    active_connections_--;
    cv_.notify_one();
}

size_t DatabaseConnectionPool::get_pool_size() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    return max_connections_;
}

size_t DatabaseConnectionPool::get_active_connections() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    return active_connections_;
}

// =====================================
// 数据库Actor服务实现
// =====================================

bool DatabaseActorService::register_database(const std::string& name,
                                             const DatabaseConfig& config) {
    std::lock_guard<std::mutex> lock(service_mutex_);

    if (connection_pools_.find(name) != connection_pools_.end()) {
        std::cerr << "[DatabaseService] Database '" << name
                  << "' already registered" << std::endl;
        return false;
    }

    try {
        auto pool = std::make_unique<DatabaseConnectionPool>(config);
        connection_pools_[name] = std::move(pool);

        std::cout << "[DatabaseService] Registered database: " << name
                  << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[DatabaseService] Failed to register database '" << name
                  << "': " << e.what() << std::endl;
        return false;
    }
}

void DatabaseActorService::unregister_database(const std::string& name) {
    std::lock_guard<std::mutex> lock(service_mutex_);

    auto it = connection_pools_.find(name);
    if (it != connection_pools_.end()) {
        connection_pools_.erase(it);
        std::cout << "[DatabaseService] Unregistered database: " << name
                  << std::endl;
    }
}

std::future<QueryResult> DatabaseActorService::execute_query_async(
    const std::string& database_name, const std::string& sql,
    const std::vector<std::string>& params) {
    return std::async(std::launch::async, [this, database_name, sql, params]() {
        return execute_query_sync(database_name, sql, params);
    });
}

QueryResult DatabaseActorService::execute_query_sync(
    const std::string& database_name, const std::string& sql,
    const std::vector<std::string>& params) {
    std::lock_guard<std::mutex> lock(service_mutex_);

    auto it = connection_pools_.find(database_name);
    if (it == connection_pools_.end()) {
        return {
            false, "Database '" + database_name + "' not registered", {}, 0, 0};
    }

    try {
        auto conn_guard = it->second->get_connection();
        return conn_guard->execute_query(sql, params);
    } catch (const std::exception& e) {
        return {false, "Connection error: " + std::string(e.what()), {}, 0, 0};
    }
}

DatabaseActorService::TransactionGuard DatabaseActorService::begin_transaction(
    const std::string& database_name) {
    std::lock_guard<std::mutex> lock(service_mutex_);

    auto it = connection_pools_.find(database_name);
    if (it == connection_pools_.end()) {
        throw std::runtime_error("Database '" + database_name +
                                 "' not registered");
    }

    auto conn_guard = it->second->get_connection();
    if (!conn_guard->begin_transaction()) {
        throw std::runtime_error("Failed to begin transaction");
    }

    return TransactionGuard(std::move(conn_guard));
}

DatabaseActorService::PoolStatus DatabaseActorService::get_pool_status(
    const std::string& database_name) const {
    std::lock_guard<std::mutex> lock(service_mutex_);

    auto it = connection_pools_.find(database_name);
    if (it == connection_pools_.end()) {
        return {0, 0, 0};
    }

    auto total = it->second->get_pool_size();
    auto active = it->second->get_active_connections();

    return {total, active, total - active};
}

std::vector<std::string> DatabaseActorService::get_registered_databases()
    const {
    std::lock_guard<std::mutex> lock(service_mutex_);

    std::vector<std::string> names;
    for (const auto& pair : connection_pools_) {
        names.push_back(pair.first);
    }

    return names;
}

// =====================================
// 事务守护者实现
// =====================================

DatabaseActorService::TransactionGuard::TransactionGuard(
    DatabaseConnectionPool::ConnectionGuard conn_guard)
    : connection_guard_(std::move(conn_guard)) {}

DatabaseActorService::TransactionGuard::~TransactionGuard() {
    if (!committed_) {
        rollback();
    }
}

QueryResult DatabaseActorService::TransactionGuard::execute(
    const std::string& sql, const std::vector<std::string>& params) {
    return connection_guard_->execute_query(sql, params);
}

bool DatabaseActorService::TransactionGuard::commit() {
    committed_ = connection_guard_->commit_transaction();
    return committed_;
}

void DatabaseActorService::TransactionGuard::rollback() {
    connection_guard_->rollback_transaction();
}

}  // namespace shield::database