#pragma once

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace shield::database {

// 数据库查询结果
struct QueryResult {
    bool success = false;
    std::string error;
    std::vector<std::unordered_map<std::string, std::string>> rows;
    size_t affected_rows = 0;
    size_t last_insert_id = 0;
};

// 数据库连接配置
struct DatabaseConfig {
    std::string driver = "mysql";  // mysql, postgresql, sqlite
    std::string host = "localhost";
    int port = 3306;
    std::string database;
    std::string username;
    std::string password;
    int max_connections = 10;
    int connection_timeout = 30;
    bool auto_reconnect = true;
    std::string charset = "utf8mb4";
};

// 数据库连接接口
class IDatabaseConnection {
public:
    virtual ~IDatabaseConnection() = default;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    virtual QueryResult execute_query(
        const std::string& sql,
        const std::vector<std::string>& params = {}) = 0;
    virtual bool begin_transaction() = 0;
    virtual bool commit_transaction() = 0;
    virtual bool rollback_transaction() = 0;
    virtual std::string escape_string(const std::string& str) = 0;
};

// MySQL连接实现
class MySQLConnection : public IDatabaseConnection {
private:
    DatabaseConfig config_;
    void* mysql_conn_ = nullptr;  // MYSQL* 连接句柄
    bool connected_ = false;

public:
    explicit MySQLConnection(const DatabaseConfig& config);
    ~MySQLConnection() override;

    bool connect() override;
    void disconnect() override;
    bool is_connected() const override { return connected_; }
    QueryResult execute_query(
        const std::string& sql,
        const std::vector<std::string>& params = {}) override;
    bool begin_transaction() override;
    bool commit_transaction() override;
    bool rollback_transaction() override;
    std::string escape_string(const std::string& str) override;

private:
    void check_connection();
    QueryResult handle_mysql_error();
};

// 连接池
class DatabaseConnectionPool {
private:
    DatabaseConfig config_;
    std::queue<std::unique_ptr<IDatabaseConnection>> available_connections_;
    std::vector<std::unique_ptr<IDatabaseConnection>> all_connections_;
    mutable std::mutex pool_mutex_;
    std::condition_variable cv_;
    size_t max_connections_;
    size_t active_connections_ = 0;

public:
    explicit DatabaseConnectionPool(const DatabaseConfig& config);
    ~DatabaseConnectionPool();

    // 获取连接 (RAII管理)
    class ConnectionGuard {
    private:
        DatabaseConnectionPool* pool_;
        std::unique_ptr<IDatabaseConnection> connection_;

    public:
        ConnectionGuard(DatabaseConnectionPool* pool,
                        std::unique_ptr<IDatabaseConnection> conn)
            : pool_(pool), connection_(std::move(conn)) {}

        ~ConnectionGuard() {
            if (connection_ && pool_) {
                pool_->return_connection(std::move(connection_));
            }
        }

        IDatabaseConnection* get() const { return connection_.get(); }
        IDatabaseConnection* operator->() const { return connection_.get(); }
        IDatabaseConnection& operator*() const { return *connection_.get(); }
    };

    ConnectionGuard get_connection(int timeout_ms = 5000);
    void return_connection(std::unique_ptr<IDatabaseConnection> conn);
    size_t get_pool_size() const;
    size_t get_active_connections() const;

private:
    std::unique_ptr<IDatabaseConnection> create_connection();
    void initialize_pool();
};

// Actor模型数据库服务
class DatabaseActorService {
private:
    std::unordered_map<std::string, std::unique_ptr<DatabaseConnectionPool>>
        connection_pools_;
    mutable std::mutex service_mutex_;

public:
    DatabaseActorService() = default;
    ~DatabaseActorService() = default;

    // 注册数据库连接池
    bool register_database(const std::string& name,
                           const DatabaseConfig& config);

    // 移除数据库连接池
    void unregister_database(const std::string& name);

    // 异步执行查询 (返回future)
    std::future<QueryResult> execute_query_async(
        const std::string& database_name, const std::string& sql,
        const std::vector<std::string>& params = {});

    // 同步执行查询
    QueryResult execute_query_sync(const std::string& database_name,
                                   const std::string& sql,
                                   const std::vector<std::string>& params = {});

    // 事务支持
    class TransactionGuard {
    private:
        DatabaseConnectionPool::ConnectionGuard connection_guard_;
        bool committed_ = false;

    public:
        explicit TransactionGuard(
            DatabaseConnectionPool::ConnectionGuard conn_guard);
        ~TransactionGuard();

        QueryResult execute(const std::string& sql,
                            const std::vector<std::string>& params = {});
        bool commit();
        void rollback();
    };

    TransactionGuard begin_transaction(const std::string& database_name);

    // 获取连接池状态
    struct PoolStatus {
        size_t total_connections;
        size_t active_connections;
        size_t available_connections;
    };

    PoolStatus get_pool_status(const std::string& database_name) const;
    std::vector<std::string> get_registered_databases() const;
};

// Lua绑定辅助函数
namespace lua_bindings {

// 注册数据库服务到Lua
void register_database_service(lua_State* L, DatabaseActorService& service);

// Lua调用的查询函数
int lua_execute_query(lua_State* L);
int lua_begin_transaction(lua_State* L);
int lua_get_pool_status(lua_State* L);

// 结果转换函数
void push_query_result(lua_State* L, const QueryResult& result);
QueryResult create_query_result_from_lua(lua_State* L, int index);
}  // namespace lua_bindings

}  // namespace shield::database