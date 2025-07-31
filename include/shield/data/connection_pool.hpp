#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace shield::data {

// =====================================
// 连接池基础接口
// =====================================

template <typename ConnectionType>
class IConnectionPool {
public:
    virtual ~IConnectionPool() = default;

    // 连接管理
    virtual std::shared_ptr<ConnectionType> acquire_connection(
        int timeout_ms = 5000) = 0;
    virtual void release_connection(
        std::shared_ptr<ConnectionType> connection) = 0;

    // 池状态
    virtual size_t total_connections() const = 0;
    virtual size_t active_connections() const = 0;
    virtual size_t idle_connections() const = 0;

    // 健康检查
    virtual void validate_connections() = 0;
    virtual bool is_healthy() const = 0;

    // 生命周期
    virtual void start() = 0;
    virtual void stop() = 0;
};

// =====================================
// 连接包装器
// =====================================

template <typename ConnectionType>
class PooledConnection {
private:
    std::shared_ptr<ConnectionType> connection_;
    std::weak_ptr<IConnectionPool<ConnectionType>> pool_;
    std::chrono::steady_clock::time_point created_at_;
    std::chrono::steady_clock::time_point last_used_;
    std::atomic<bool> in_use_{false};
    std::atomic<bool> is_valid_{true};

public:
    PooledConnection(std::shared_ptr<ConnectionType> conn,
                     std::weak_ptr<IConnectionPool<ConnectionType>> pool)
        : connection_(conn),
          pool_(pool),
          created_at_(std::chrono::steady_clock::now()),
          last_used_(std::chrono::steady_clock::now()) {}

    ~PooledConnection() {
        if (auto pool = pool_.lock()) {
            if (connection_ && is_valid_) {
                in_use_ = false;
                pool->release_connection(shared_from_this());
            }
        }
    }

    ConnectionType* get() const { return connection_.get(); }
    ConnectionType& operator*() const { return *connection_; }
    ConnectionType* operator->() const { return connection_.get(); }

    bool is_in_use() const { return in_use_; }
    bool is_valid() const { return is_valid_; }

    void mark_in_use() {
        in_use_ = true;
        last_used_ = std::chrono::steady_clock::now();
    }

    void mark_invalid() { is_valid_ = false; }

    std::chrono::steady_clock::time_point created_time() const {
        return created_at_;
    }
    std::chrono::steady_clock::time_point last_used_time() const {
        return last_used_;
    }

    std::chrono::milliseconds age() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - created_at_);
    }

    std::chrono::milliseconds idle_time() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_used_);
    }
};

// =====================================
// 通用连接池实现
// =====================================

template <typename ConnectionType>
class GenericConnectionPool : public IConnectionPool<ConnectionType>,
                              public std::enable_shared_from_this<
                                  GenericConnectionPool<ConnectionType>> {
public:
    struct Config {
        size_t min_connections = 2;
        size_t max_connections = 10;
        std::chrono::milliseconds connection_timeout{30000};
        std::chrono::milliseconds idle_timeout{600000};        // 10分钟
        std::chrono::milliseconds max_lifetime{3600000};       // 1小时
        std::chrono::milliseconds validation_interval{30000};  // 30秒
        bool test_on_borrow = true;
        bool test_on_return = false;
        bool test_while_idle = true;
    };

    using ConnectionFactory = std::function<std::shared_ptr<ConnectionType>()>;
    using ConnectionValidator = std::function<bool(ConnectionType*)>;

private:
    Config config_;
    ConnectionFactory factory_;
    ConnectionValidator validator_;

    mutable std::mutex pool_mutex_;
    std::condition_variable pool_condition_;

    std::queue<std::shared_ptr<PooledConnection<ConnectionType>>>
        available_connections_;
    std::unordered_map<ConnectionType*,
                       std::shared_ptr<PooledConnection<ConnectionType>>>
        all_connections_;

    std::atomic<size_t> active_count_{0};
    std::atomic<bool> running_{false};
    std::thread maintenance_thread_;

    // 统计信息
    std::atomic<size_t> total_created_{0};
    std::atomic<size_t> total_destroyed_{0};
    std::atomic<size_t> total_acquired_{0};
    std::atomic<size_t> total_released_{0};
    std::atomic<size_t> validation_failures_{0};

public:
    GenericConnectionPool(const Config& config, ConnectionFactory factory,
                          ConnectionValidator validator = nullptr)
        : config_(config), factory_(factory), validator_(validator) {}

    ~GenericConnectionPool() { stop(); }

    void start() override {
        std::lock_guard<std::mutex> lock(pool_mutex_);

        if (running_) return;

        running_ = true;

        // 创建最小连接数
        for (size_t i = 0; i < config_.min_connections; ++i) {
            try_create_connection();
        }

        // 启动维护线程
        maintenance_thread_ =
            std::thread(&GenericConnectionPool::maintenance_loop, this);

        std::cout << "[ConnectionPool] Started with " << all_connections_.size()
                  << " initial connections" << std::endl;
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            running_ = false;
        }

        pool_condition_.notify_all();

        if (maintenance_thread_.joinable()) {
            maintenance_thread_.join();
        }

        // 清理所有连接
        std::lock_guard<std::mutex> lock(pool_mutex_);
        while (!available_connections_.empty()) {
            available_connections_.pop();
        }
        all_connections_.clear();
        active_count_ = 0;

        std::cout << "[ConnectionPool] Stopped. Stats - Created: "
                  << total_created_ << ", Destroyed: " << total_destroyed_
                  << ", Acquired: " << total_acquired_
                  << ", Released: " << total_released_ << std::endl;
    }

    std::shared_ptr<ConnectionType> acquire_connection(
        int timeout_ms = 5000) override {
        std::unique_lock<std::mutex> lock(pool_mutex_);

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);

        while (running_) {
            // 尝试获取可用连接
            if (!available_connections_.empty()) {
                auto pooled_conn = available_connections_.front();
                available_connections_.pop();

                // 验证连接
                if (is_connection_valid(pooled_conn)) {
                    pooled_conn->mark_in_use();
                    active_count_++;
                    total_acquired_++;
                    return pooled_conn->get();
                } else {
                    // 连接无效，销毁并继续
                    destroy_connection(pooled_conn);
                    continue;
                }
            }

            // 尝试创建新连接
            if (all_connections_.size() < config_.max_connections) {
                if (auto new_conn = try_create_connection()) {
                    new_conn->mark_in_use();
                    active_count_++;
                    total_acquired_++;
                    return new_conn->get();
                }
            }

            // 等待连接可用或超时
            if (pool_condition_.wait_until(lock, deadline) ==
                std::cv_status::timeout) {
                throw std::runtime_error("Connection pool timeout after " +
                                         std::to_string(timeout_ms) + "ms");
            }
        }

        throw std::runtime_error("Connection pool is not running");
    }

    void release_connection(
        std::shared_ptr<ConnectionType> connection) override {
        if (!connection) return;

        std::lock_guard<std::mutex> lock(pool_mutex_);

        auto it = all_connections_.find(connection.get());
        if (it != all_connections_.end()) {
            auto pooled_conn = it->second;

            // 验证连接（如果配置了）
            bool is_valid =
                !config_.test_on_return || is_connection_valid(pooled_conn);

            if (is_valid && running_) {
                // 连接有效，放回池中
                available_connections_.push(pooled_conn);
                total_released_++;
            } else {
                // 连接无效或池已停止，销毁连接
                destroy_connection(pooled_conn);
            }

            active_count_--;
            pool_condition_.notify_one();
        }
    }

    size_t total_connections() const override {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        return all_connections_.size();
    }

    size_t active_connections() const override { return active_count_; }

    size_t idle_connections() const override {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        return available_connections_.size();
    }

    void validate_connections() override {
        std::lock_guard<std::mutex> lock(pool_mutex_);

        if (!validator_) return;

        std::vector<std::shared_ptr<PooledConnection<ConnectionType>>>
            to_remove;

        // 检查空闲连接
        std::queue<std::shared_ptr<PooledConnection<ConnectionType>>>
            temp_queue;

        while (!available_connections_.empty()) {
            auto conn = available_connections_.front();
            available_connections_.pop();

            if (is_connection_valid(conn)) {
                temp_queue.push(conn);
            } else {
                to_remove.push_back(conn);
            }
        }

        available_connections_ = std::move(temp_queue);

        // 销毁无效连接
        for (auto& conn : to_remove) {
            destroy_connection(conn);
        }

        // 补充连接到最小数量
        while (all_connections_.size() < config_.min_connections) {
            if (!try_create_connection()) {
                break;
            }
        }
    }

    bool is_healthy() const override {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        return running_ && all_connections_.size() >= config_.min_connections &&
               (validation_failures_ * 100 /
                std::max(total_created_.load(), 1UL)) < 50;  // 失败率 < 50%
    }

    // 统计信息
    struct PoolStats {
        size_t total_connections;
        size_t active_connections;
        size_t idle_connections;
        size_t total_created;
        size_t total_destroyed;
        size_t total_acquired;
        size_t total_released;
        size_t validation_failures;
        double success_rate;
    };

    PoolStats get_stats() const {
        std::lock_guard<std::mutex> lock(pool_mutex_);

        size_t total_ops = total_acquired_ + total_released_;
        double success_rate =
            total_ops > 0
                ? (double)(total_ops - validation_failures_) / total_ops * 100.0
                : 100.0;

        return {all_connections_.size(),
                active_count_.load(),
                available_connections_.size(),
                total_created_.load(),
                total_destroyed_.load(),
                total_acquired_.load(),
                total_released_.load(),
                validation_failures_.load(),
                success_rate};
    }

private:
    std::shared_ptr<PooledConnection<ConnectionType>> try_create_connection() {
        try {
            auto raw_conn = factory_();
            if (!raw_conn) {
                return nullptr;
            }

            auto pooled_conn =
                std::make_shared<PooledConnection<ConnectionType>>(
                    raw_conn, this->weak_from_this());

            all_connections_[raw_conn.get()] = pooled_conn;
            available_connections_.push(pooled_conn);

            total_created_++;

            std::cout << "[ConnectionPool] Created new connection (total: "
                      << all_connections_.size() << ")" << std::endl;

            return pooled_conn;

        } catch (const std::exception& e) {
            std::cerr << "[ConnectionPool] Failed to create connection: "
                      << e.what() << std::endl;
            return nullptr;
        }
    }

    void destroy_connection(
        std::shared_ptr<PooledConnection<ConnectionType>> conn) {
        if (!conn) return;

        auto raw_ptr = conn->get();
        all_connections_.erase(raw_ptr);
        total_destroyed_++;

        std::cout << "[ConnectionPool] Destroyed connection (remaining: "
                  << all_connections_.size() << ")" << std::endl;
    }

    bool is_connection_valid(
        std::shared_ptr<PooledConnection<ConnectionType>> conn) {
        if (!conn || !conn->is_valid()) {
            return false;
        }

        // 检查连接年龄
        if (conn->age() > config_.max_lifetime) {
            conn->mark_invalid();
            return false;
        }

        // 检查空闲时间
        if (conn->idle_time() > config_.idle_timeout) {
            conn->mark_invalid();
            return false;
        }

        // 运行验证器
        if (validator_) {
            try {
                if (!validator_(conn->get())) {
                    conn->mark_invalid();
                    validation_failures_++;
                    return false;
                }
            } catch (const std::exception& e) {
                std::cerr << "[ConnectionPool] Connection validation failed: "
                          << e.what() << std::endl;
                conn->mark_invalid();
                validation_failures_++;
                return false;
            }
        }

        return true;
    }

    void maintenance_loop() {
        std::cout << "[ConnectionPool] Maintenance thread started" << std::endl;

        while (running_) {
            std::this_thread::sleep_for(config_.validation_interval);

            if (!running_) break;

            try {
                validate_connections();
            } catch (const std::exception& e) {
                std::cerr << "[ConnectionPool] Maintenance error: " << e.what()
                          << std::endl;
            }
        }

        std::cout << "[ConnectionPool] Maintenance thread stopped" << std::endl;
    }
};

// =====================================
// 连接池管理器
// =====================================

class ConnectionPoolManager {
private:
    std::unordered_map<std::string, std::shared_ptr<void>> pools_;
    mutable std::mutex pools_mutex_;

public:
    template <typename ConnectionType>
    void register_pool(const std::string& name,
                       std::shared_ptr<IConnectionPool<ConnectionType>> pool) {
        std::lock_guard<std::mutex> lock(pools_mutex_);
        pools_[name] = std::static_pointer_cast<void>(pool);
        pool->start();

        std::cout << "[PoolManager] Registered connection pool: " << name
                  << std::endl;
    }

    template <typename ConnectionType>
    std::shared_ptr<IConnectionPool<ConnectionType>> get_pool(
        const std::string& name) {
        std::lock_guard<std::mutex> lock(pools_mutex_);

        auto it = pools_.find(name);
        if (it != pools_.end()) {
            return std::static_pointer_cast<IConnectionPool<ConnectionType>>(
                it->second);
        }

        return nullptr;
    }

    void remove_pool(const std::string& name) {
        std::lock_guard<std::mutex> lock(pools_mutex_);

        auto it = pools_.find(name);
        if (it != pools_.end()) {
            pools_.erase(it);
            std::cout << "[PoolManager] Removed connection pool: " << name
                      << std::endl;
        }
    }

    std::vector<std::string> get_pool_names() const {
        std::lock_guard<std::mutex> lock(pools_mutex_);

        std::vector<std::string> names;
        for (const auto& pair : pools_) {
            names.push_back(pair.first);
        }
        return names;
    }

    void shutdown_all() {
        std::lock_guard<std::mutex> lock(pools_mutex_);

        std::cout << "[PoolManager] Shutting down " << pools_.size()
                  << " connection pools" << std::endl;

        pools_.clear();
    }

    // 健康检查所有池
    struct HealthReport {
        std::string name;
        bool healthy;
        size_t total_connections;
        size_t active_connections;
        std::string status;
    };

    std::vector<HealthReport> health_check() const {
        std::vector<HealthReport> reports;

        std::lock_guard<std::mutex> lock(pools_mutex_);

        for (const auto& pair : pools_) {
            // 这里需要类型擦除的健康检查接口
            // 简化实现，假设所有池都实现了通用健康检查
            HealthReport report;
            report.name = pair.first;
            report.healthy = true;  // 简化实现
            report.total_connections = 0;
            report.active_connections = 0;
            report.status = "OK";

            reports.push_back(report);
        }

        return reports;
    }
};

// =====================================
// 连接池工厂
// =====================================

template <typename ConnectionType>
class ConnectionPoolFactory {
public:
    using PoolConfig = typename GenericConnectionPool<ConnectionType>::Config;
    using ConnectionFactory = std::function<std::shared_ptr<ConnectionType>()>;
    using ConnectionValidator = std::function<bool(ConnectionType*)>;

    static std::shared_ptr<IConnectionPool<ConnectionType>> create_generic_pool(
        const PoolConfig& config, ConnectionFactory factory,
        ConnectionValidator validator = nullptr) {
        return std::make_shared<GenericConnectionPool<ConnectionType>>(
            config, factory, validator);
    }

    // 预配置的数据库连接池
    static std::shared_ptr<IConnectionPool<ConnectionType>>
    create_database_pool(const std::string& connection_string,
                         const PoolConfig& config) {
        ConnectionFactory factory =
            [connection_string]() -> std::shared_ptr<ConnectionType> {
            // 这里需要根据具体的ConnectionType实现连接创建
            return std::make_shared<ConnectionType>(connection_string);
        };

        ConnectionValidator validator = [](ConnectionType* conn) -> bool {
            // 简单的连接验证
            return conn != nullptr;
        };

        return create_generic_pool(config, factory, validator);
    }
};

// =====================================
// RAII连接守护者
// =====================================

template <typename ConnectionType>
class ConnectionGuard {
private:
    std::shared_ptr<ConnectionType> connection_;
    std::shared_ptr<IConnectionPool<ConnectionType>> pool_;

public:
    ConnectionGuard(std::shared_ptr<IConnectionPool<ConnectionType>> pool,
                    int timeout_ms = 5000)
        : pool_(pool) {
        if (!pool_) {
            throw std::runtime_error("Connection pool is null");
        }

        connection_ = pool_->acquire_connection(timeout_ms);
        if (!connection_) {
            throw std::runtime_error("Failed to acquire connection from pool");
        }
    }

    ~ConnectionGuard() {
        if (connection_ && pool_) {
            pool_->release_connection(connection_);
        }
    }

    // 禁止拷贝
    ConnectionGuard(const ConnectionGuard&) = delete;
    ConnectionGuard& operator=(const ConnectionGuard&) = delete;

    // 允许移动
    ConnectionGuard(ConnectionGuard&& other) noexcept
        : connection_(std::move(other.connection_)),
          pool_(std::move(other.pool_)) {
        other.connection_ = nullptr;
        other.pool_ = nullptr;
    }

    ConnectionGuard& operator=(ConnectionGuard&& other) noexcept {
        if (this != &other) {
            if (connection_ && pool_) {
                pool_->release_connection(connection_);
            }

            connection_ = std::move(other.connection_);
            pool_ = std::move(other.pool_);

            other.connection_ = nullptr;
            other.pool_ = nullptr;
        }
        return *this;
    }

    ConnectionType* get() const { return connection_.get(); }
    ConnectionType& operator*() const { return *connection_; }
    ConnectionType* operator->() const { return connection_.get(); }

    bool is_valid() const { return connection_ != nullptr; }

    // 手动释放连接
    void release() {
        if (connection_ && pool_) {
            pool_->release_connection(connection_);
            connection_ = nullptr;
        }
    }
};

// =====================================
// 连接池监控
// =====================================

class ConnectionPoolMonitor {
private:
    std::shared_ptr<ConnectionPoolManager> pool_manager_;
    std::atomic<bool> monitoring_{false};
    std::thread monitor_thread_;
    std::chrono::seconds monitor_interval_{30};

public:
    explicit ConnectionPoolMonitor(
        std::shared_ptr<ConnectionPoolManager> manager)
        : pool_manager_(manager) {}

    ~ConnectionPoolMonitor() { stop(); }

    void start(std::chrono::seconds interval = std::chrono::seconds{30}) {
        monitor_interval_ = interval;
        monitoring_ = true;

        monitor_thread_ =
            std::thread(&ConnectionPoolMonitor::monitor_loop, this);

        std::cout << "[PoolMonitor] Started monitoring with "
                  << interval.count() << "s interval" << std::endl;
    }

    void stop() {
        monitoring_ = false;

        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }

        std::cout << "[PoolMonitor] Stopped monitoring" << std::endl;
    }

    void set_interval(std::chrono::seconds interval) {
        monitor_interval_ = interval;
    }

private:
    void monitor_loop() {
        while (monitoring_) {
            try {
                auto reports = pool_manager_->health_check();

                for (const auto& report : reports) {
                    std::cout
                        << "[PoolMonitor] Pool '" << report.name
                        << "': " << (report.healthy ? "HEALTHY" : "UNHEALTHY")
                        << " (Active: " << report.active_connections << "/"
                        << report.total_connections << ")" << std::endl;
                }

            } catch (const std::exception& e) {
                std::cerr << "[PoolMonitor] Monitoring error: " << e.what()
                          << std::endl;
            }

            std::this_thread::sleep_for(monitor_interval_);
        }
    }
};

}  // namespace shield::data