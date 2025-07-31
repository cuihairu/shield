#include <chrono>
#include <iostream>
#include <thread>

#include "shield/data/connection_pool.hpp"
#include "shield/data/data_access_framework.hpp"
#include "shield/data/orm.hpp"
#include "shield/data/query_cache.hpp"

using namespace shield::data;
using namespace shield::data::cache;
using namespace shield::data::orm;

// =====================================
// 示例实体定义
// =====================================

// 用户实体
class User : public BaseEntity {
private:
    int64_t id_ = 0;
    std::string username_;
    std::string email_;
    int level_ = 1;
    std::chrono::system_clock::time_point created_at_;

public:
    SHIELD_ENTITY("users")
    SHIELD_AUTO_INCREMENT(id, "id")
    SHIELD_FIELD(username, "username", "VARCHAR(50)", false, false, false, 50)
    SHIELD_FIELD(email, "email", "VARCHAR(100)", false, false, false, 100, "",
                 true, true)
    SHIELD_FIELD(level, "level", "INT", false, false, true, 0, "1")

    // 构造函数
    User() = default;
    User(const std::string& username, const std::string& email, int level = 1)
        : username_(username),
          email_(email),
          level_(level),
          created_at_(std::chrono::system_clock::now()) {}

    // 访问器
    int64_t get_id() const { return id_; }
    void set_id(int64_t id) {
        id_ = id;
        mark_field_dirty("id");
    }

    const std::string& get_username() const { return username_; }
    void set_username(const std::string& username) {
        username_ = username;
        mark_field_dirty("username");
    }

    const std::string& get_email() const { return email_; }
    void set_email(const std::string& email) {
        email_ = email;
        mark_field_dirty("email");
    }

    int get_level() const { return level_; }
    void set_level(int level) {
        level_ = level;
        mark_field_dirty("level");
    }

    // BaseEntity接口实现
    std::string get_table_name() const override { return "users"; }

    DataRow to_data_row() const override {
        DataRow row;
        if (id_ != 0) row["id"] = DataValue(id_);
        row["username"] = DataValue(username_);
        row["email"] = DataValue(email_);
        row["level"] = DataValue(level_);
        return row;
    }

    void from_data_row(const DataRow& row) override {
        auto it = row.find("id");
        if (it != row.end() && !it->second.is_null()) {
            id_ = it->second.as<int64_t>();
        }

        it = row.find("username");
        if (it != row.end()) {
            username_ = it->second.as<std::string>();
        }

        it = row.find("email");
        if (it != row.end()) {
            email_ = it->second.as<std::string>();
        }

        it = row.find("level");
        if (it != row.end()) {
            level_ = it->second.as<int>();
        }

        clear_dirty_fields();
    }

    std::string get_primary_key_field() const override { return "id"; }

    DataValue get_primary_key_value() const override { return DataValue(id_); }

    void set_primary_key_value(const DataValue& value) override {
        if (!value.is_null()) {
            id_ = value.as<int64_t>();
        }
    }
};

// 订单实体
class Order : public BaseEntity {
private:
    int64_t id_ = 0;
    int64_t user_id_ = 0;
    std::string product_name_;
    double amount_ = 0.0;
    std::string status_ = "pending";

public:
    SHIELD_ENTITY("orders")
    SHIELD_AUTO_INCREMENT(id, "id")
    SHIELD_FIELD(user_id, "user_id", "BIGINT", false, false, false)
    SHIELD_FIELD(product_name, "product_name", "VARCHAR(200)")
    SHIELD_FIELD(amount, "amount", "DECIMAL(10,2)")
    SHIELD_FIELD(status, "status", "VARCHAR(20)")

    Order() = default;
    Order(int64_t user_id, const std::string& product, double amount)
        : user_id_(user_id), product_name_(product), amount_(amount) {}

    // 访问器
    int64_t get_id() const { return id_; }
    int64_t get_user_id() const { return user_id_; }
    const std::string& get_product_name() const { return product_name_; }
    double get_amount() const { return amount_; }
    const std::string& get_status() const { return status_; }

    void set_status(const std::string& status) {
        status_ = status;
        mark_field_dirty("status");
    }

    // BaseEntity接口实现
    std::string get_table_name() const override { return "orders"; }

    DataRow to_data_row() const override {
        DataRow row;
        if (id_ != 0) row["id"] = DataValue(id_);
        row["user_id"] = DataValue(user_id_);
        row["product_name"] = DataValue(product_name_);
        row["amount"] = DataValue(amount_);
        row["status"] = DataValue(status_);
        return row;
    }

    void from_data_row(const DataRow& row) override {
        auto it = row.find("id");
        if (it != row.end() && !it->second.is_null()) {
            id_ = it->second.as<int64_t>();
        }

        it = row.find("user_id");
        if (it != row.end()) {
            user_id_ = it->second.as<int64_t>();
        }

        it = row.find("product_name");
        if (it != row.end()) {
            product_name_ = it->second.as<std::string>();
        }

        it = row.find("amount");
        if (it != row.end()) {
            amount_ = it->second.as<double>();
        }

        it = row.find("status");
        if (it != row.end()) {
            status_ = it->second.as<std::string>();
        }

        clear_dirty_fields();
    }

    std::string get_primary_key_field() const override { return "id"; }

    DataValue get_primary_key_value() const override { return DataValue(id_); }

    void set_primary_key_value(const DataValue& value) override {
        if (!value.is_null()) {
            id_ = value.as<int64_t>();
        }
    }
};

// =====================================
// Repository实现
// =====================================

class UserRepository {
private:
    std::shared_ptr<EntityManager<User>> entity_manager_;

public:
    explicit UserRepository(std::shared_ptr<IDataSource> data_source) {
        entity_manager_ = std::make_shared<EntityManager<User>>(data_source);
    }

    // 基本CRUD操作
    std::future<std::shared_ptr<User>> find_by_id(int64_t id) {
        return entity_manager_->find(DataValue(id));
    }

    std::future<std::vector<std::shared_ptr<User>>> find_all() {
        return entity_manager_->find_all();
    }

    std::future<std::shared_ptr<User>> save(std::shared_ptr<User> user) {
        return entity_manager_->save(user);
    }

    std::future<void> remove(std::shared_ptr<User> user) {
        return entity_manager_->remove(user);
    }

    // 自定义查询方法
    std::future<std::vector<std::shared_ptr<User>>> find_by_level(int level) {
        auto criteria = Criteria::where("level")->equals(DataValue(level));
        return entity_manager_->find_by_criteria(criteria);
    }

    std::future<std::vector<std::shared_ptr<User>>> find_by_username_like(
        const std::string& pattern) {
        auto criteria = Criteria::where("username")->like(pattern);
        return entity_manager_->find_by_criteria(criteria);
    }

    std::future<std::vector<std::shared_ptr<User>>> find_high_level_users(
        int min_level, size_t limit) {
        auto criteria =
            Criteria::where("level")->greater_than(DataValue(min_level));
        return entity_manager_->query()
            .where(criteria)
            .order_by({Sort::desc("level"), Sort::asc("username")})
            .limit(limit)
            .execute();
    }

    // 事务操作示例
    std::future<bool> batch_update_levels(const std::vector<int64_t>& user_ids,
                                          int new_level) {
        return std::async(std::launch::async, [this, user_ids, new_level]() {
            auto transaction = entity_manager_->begin_transaction();

            try {
                for (int64_t user_id : user_ids) {
                    auto user_future = find_by_id(user_id);
                    auto user = user_future.get();

                    if (user) {
                        user->set_level(new_level);
                        auto save_future = entity_manager_->save(user);
                        save_future.get();
                        transaction->add_entity(user);
                    }
                }

                return transaction->commit();
            } catch (const std::exception& e) {
                std::cerr << "Batch update failed: " << e.what() << std::endl;
                transaction->rollback();
                return false;
            }
        });
    }
};

class OrderRepository {
private:
    std::shared_ptr<EntityManager<Order>> entity_manager_;

public:
    explicit OrderRepository(std::shared_ptr<IDataSource> data_source) {
        entity_manager_ = std::make_shared<EntityManager<Order>>(data_source);
    }

    std::future<std::vector<std::shared_ptr<Order>>> find_by_user_id(
        int64_t user_id) {
        auto criteria = Criteria::where("user_id")->equals(DataValue(user_id));
        return entity_manager_->find_by_criteria(criteria);
    }

    std::future<std::vector<std::shared_ptr<Order>>> find_by_status(
        const std::string& status) {
        auto criteria = Criteria::where("status")->equals(DataValue(status));
        return entity_manager_->find_by_criteria(criteria);
    }

    std::future<std::vector<std::shared_ptr<Order>>> find_high_value_orders(
        double min_amount) {
        auto criteria =
            Criteria::where("amount")->greater_than(DataValue(min_amount));
        return entity_manager_->query()
            .where(criteria)
            .order_by({Sort::desc("amount")})
            .execute();
    }

    std::future<std::shared_ptr<Order>> save(std::shared_ptr<Order> order) {
        return entity_manager_->save(order);
    }
};

// =====================================
// 性能监控和统计
// =====================================

void print_performance_statistics(
    const std::shared_ptr<QueryPerformanceMonitor>& monitor,
    const std::shared_ptr<QueryCacheManager>& cache_manager) {
    std::cout << "\n=== 性能统计报告 ===" << std::endl;

    // 缓存统计
    auto cache_stats = cache_manager->get_statistics();
    std::cout << "缓存统计:" << std::endl;
    std::cout << "  总请求数: " << cache_stats.total_requests.load()
              << std::endl;
    std::cout << "  缓存命中: " << cache_stats.cache_hits.load() << std::endl;
    std::cout << "  缓存未命中: " << cache_stats.cache_misses.load()
              << std::endl;
    std::cout << "  命中率: " << std::fixed << std::setprecision(2)
              << (cache_stats.get_hit_ratio() * 100) << "%" << std::endl;
    std::cout << "  当前缓存大小: " << cache_stats.cache_size.load()
              << std::endl;

    // 查询性能统计
    auto slow_queries = monitor->get_top_slow_queries(5);
    if (!slow_queries.empty()) {
        std::cout << "\n最慢查询TOP5:" << std::endl;
        for (const auto& metrics : slow_queries) {
            std::cout << "  查询: " << metrics.query_signature << std::endl;
            std::cout << "    平均执行时间: "
                      << metrics.avg_execution_time.count() << "ms"
                      << std::endl;
            std::cout << "    执行次数: " << metrics.execution_count
                      << std::endl;
            std::cout << "    缓存命中率: " << std::fixed
                      << std::setprecision(2) << (metrics.cache_hit_ratio * 100)
                      << "%" << std::endl;
        }
    }

    auto frequent_queries = monitor->get_most_frequent_queries(5);
    if (!frequent_queries.empty()) {
        std::cout << "\n最频繁查询TOP5:" << std::endl;
        for (const auto& metrics : frequent_queries) {
            std::cout << "  查询: " << metrics.query_signature << std::endl;
            std::cout << "    执行次数: " << metrics.execution_count
                      << std::endl;
            std::cout << "    平均执行时间: "
                      << metrics.avg_execution_time.count() << "ms"
                      << std::endl;
        }
    }
}

// =====================================
// 主演示函数
// =====================================

int main() {
    std::cout << "=== Shield数据访问框架完整演示 ===" << std::endl;

    try {
        // 1. 注册内置数据源
        DataSourceFactory::register_built_in_creators();

        // 2. 配置数据源
        DataSourceConfig mysql_config;
        mysql_config.type = "mysql";
        mysql_config.host = "localhost";
        mysql_config.port = 3306;
        mysql_config.database = "shield_demo";
        mysql_config.username = "demo_user";
        mysql_config.password = "demo_pass";
        mysql_config.max_connections = 20;
        mysql_config.min_connections = 5;

        DataSourceConfig postgres_config;
        postgres_config.type = "postgresql";
        postgres_config.host = "localhost";
        postgres_config.port = 5432;
        postgres_config.database = "shield_demo";
        postgres_config.username = "demo_user";
        postgres_config.password = "demo_pass";

        // 3. 创建数据源（带连接池）
        std::cout << "\n--- 创建数据源和连接池 ---" << std::endl;
        auto mysql_datasource = DataSourceFactory::create(mysql_config);
        auto postgres_datasource = DataSourceFactory::create(postgres_config);

        // 4. 设置查询缓存
        std::cout << "\n--- 配置查询缓存系统 ---" << std::endl;
        CacheConfig cache_config;
        cache_config.max_entries = 1000;
        cache_config.default_ttl = std::chrono::seconds{300};  // 5分钟
        cache_config.enable_statistics = true;

        auto cache_manager = std::make_shared<QueryCacheManager>(cache_config);
        cache_manager->start();

        // 5. 创建带缓存的数据源
        auto cached_mysql_ds = std::make_shared<CachedDataSource>(
            mysql_datasource, cache_manager, cache_config);
        auto cached_postgres_ds = std::make_shared<CachedDataSource>(
            postgres_datasource, cache_manager, cache_config);

        // 6. 创建性能监控
        auto performance_monitor = std::make_shared<QueryPerformanceMonitor>();

        // 7. 创建Repository
        std::cout << "\n--- 创建Repository层 ---" << std::endl;
        UserRepository user_repo(cached_mysql_ds);
        OrderRepository order_repo(cached_postgres_ds);

        // 8. 基本CRUD操作演示
        std::cout << "\n--- 基本CRUD操作演示 ---" << std::endl;

        // 创建用户
        auto user1 = std::make_shared<User>("alice", "alice@example.com", 10);
        auto user2 = std::make_shared<User>("bob", "bob@example.com", 15);
        auto user3 =
            std::make_shared<User>("charlie", "charlie@example.com", 20);

        std::cout << "创建用户..." << std::endl;
        auto save_future1 = user_repo.save(user1);
        auto save_future2 = user_repo.save(user2);
        auto save_future3 = user_repo.save(user3);

        auto saved_user1 = save_future1.get();
        auto saved_user2 = save_future2.get();
        auto saved_user3 = save_future3.get();

        std::cout << "用户创建完成: " << saved_user1->get_username()
                  << "(ID:" << saved_user1->get_id() << "), "
                  << saved_user2->get_username()
                  << "(ID:" << saved_user2->get_id() << "), "
                  << saved_user3->get_username()
                  << "(ID:" << saved_user3->get_id() << ")" << std::endl;

        // 创建订单
        auto order1 =
            std::make_shared<Order>(saved_user1->get_id(), "iPhone 15", 999.99);
        auto order2 = std::make_shared<Order>(saved_user2->get_id(),
                                              "MacBook Pro", 2499.99);
        auto order3 =
            std::make_shared<Order>(saved_user1->get_id(), "AirPods", 179.99);

        std::cout << "创建订单..." << std::endl;
        auto order_save1 = order_repo.save(order1);
        auto order_save2 = order_repo.save(order2);
        auto order_save3 = order_repo.save(order3);

        order_save1.get();
        order_save2.get();
        order_save3.get();

        std::cout << "订单创建完成" << std::endl;

        // 9. 复杂查询演示
        std::cout << "\n--- 复杂查询演示 ---" << std::endl;

        // 查询高级用户
        std::cout << "查询level > 12的用户:" << std::endl;
        auto high_level_users_future = user_repo.find_high_level_users(12, 10);
        auto high_level_users = high_level_users_future.get();

        for (const auto& user : high_level_users) {
            std::cout << "  " << user->get_username()
                      << " (Level: " << user->get_level() << ")" << std::endl;
        }

        // 查询用户名包含特定字符的用户
        std::cout << "\n查询用户名包含'a'的用户:" << std::endl;
        auto users_with_a_future = user_repo.find_by_username_like("%a%");
        auto users_with_a = users_with_a_future.get();

        for (const auto& user : users_with_a) {
            std::cout << "  " << user->get_username() << " ("
                      << user->get_email() << ")" << std::endl;
        }

        // 查询高价值订单
        std::cout << "\n查询金额 > 500的订单:" << std::endl;
        auto high_value_orders_future =
            order_repo.find_high_value_orders(500.0);
        auto high_value_orders = high_value_orders_future.get();

        for (const auto& order : high_value_orders) {
            std::cout << "  " << order->get_product_name() << " - $"
                      << order->get_amount()
                      << " (用户ID: " << order->get_user_id() << ")"
                      << std::endl;
        }

        // 10. 缓存性能测试
        std::cout << "\n--- 缓存性能测试 ---" << std::endl;

        auto start_time = std::chrono::high_resolution_clock::now();

        // 第一次查询（缓存未命中）
        std::cout << "第一次查询（无缓存）:" << std::endl;
        for (int i = 0; i < 5; ++i) {
            auto users_future = user_repo.find_by_level(10);
            auto users = users_future.get();
            std::cout << "  查询 " << (i + 1) << ": 找到 " << users.size()
                      << " 个用户" << std::endl;
        }

        auto mid_time = std::chrono::high_resolution_clock::now();

        // 第二次查询（缓存命中）
        std::cout << "\n第二次查询（有缓存）:" << std::endl;
        for (int i = 0; i < 5; ++i) {
            auto users_future = user_repo.find_by_level(10);
            auto users = users_future.get();
            std::cout << "  查询 " << (i + 1) << ": 找到 " << users.size()
                      << " 个用户" << std::endl;
        }

        auto end_time = std::chrono::high_resolution_clock::now();

        auto first_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(mid_time -
                                                                  start_time);
        auto second_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time -
                                                                  mid_time);

        std::cout << "无缓存查询耗时: " << first_duration.count() << "ms"
                  << std::endl;
        std::cout << "有缓存查询耗时: " << second_duration.count() << "ms"
                  << std::endl;
        std::cout << "性能提升: " << std::fixed << std::setprecision(2)
                  << (static_cast<double>(first_duration.count()) /
                      second_duration.count())
                  << "x" << std::endl;

        // 11. 事务演示
        std::cout << "\n--- 事务操作演示 ---" << std::endl;
        std::vector<int64_t> user_ids = {saved_user1->get_id(),
                                         saved_user2->get_id()};
        auto batch_update_future = user_repo.batch_update_levels(user_ids, 25);
        bool batch_success = batch_update_future.get();

        std::cout << "批量更新用户level到25: "
                  << (batch_success ? "成功" : "失败") << std::endl;

        // 验证更新结果
        if (batch_success) {
            auto updated_user1_future =
                user_repo.find_by_id(saved_user1->get_id());
            auto updated_user1 = updated_user1_future.get();
            if (updated_user1) {
                std::cout << "用户 " << updated_user1->get_username()
                          << " 的新level: " << updated_user1->get_level()
                          << std::endl;
            }
        }

        // 12. 并发测试
        std::cout << "\n--- 并发操作测试 ---" << std::endl;
        std::vector<std::thread> threads;
        std::atomic<int> success_count{0};

        for (int i = 0; i < 10; ++i) {
            threads.emplace_back([&user_repo, &success_count, i]() {
                try {
                    auto user = std::make_shared<User>(
                        "user" + std::to_string(i),
                        "user" + std::to_string(i) + "@test.com", i % 5 + 1);
                    auto save_future = user_repo.save(user);
                    auto saved_user = save_future.get();

                    if (saved_user && saved_user->get_id() > 0) {
                        success_count++;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "并发操作失败: " << e.what() << std::endl;
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        std::cout << "并发创建用户: " << success_count.load() << "/10 成功"
                  << std::endl;

        // 13. 性能统计报告
        print_performance_statistics(performance_monitor, cache_manager);

        // 14. 导出性能指标
        performance_monitor->export_metrics_to_json(
            "query_performance_report.json");
        std::cout << "\n性能报告已导出到 query_performance_report.json"
                  << std::endl;

        // 15. 清理资源
        std::cout << "\n--- 清理资源 ---" << std::endl;
        cache_manager->stop();
        cached_mysql_ds->close();
        cached_postgres_ds->close();

        std::cout << "\n=== 演示完成 ===" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "演示过程中发生错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}