#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "shield/data/connection_pool.hpp"
#include "shield/data/data_access_framework.hpp"
#include "shield/data/orm.hpp"
#include "shield/data/query_cache.hpp"

using namespace shield::data;
using namespace shield::data::cache;
using namespace shield::data::orm;
using namespace testing;

// =====================================
// 测试实体定义
// =====================================

class TestUser : public BaseEntity {
private:
    int64_t id_ = 0;
    std::string name_;
    std::string email_;
    int age_ = 0;

public:
    TestUser() = default;
    TestUser(const std::string& name, const std::string& email, int age)
        : name_(name), email_(email), age_(age) {}

    // Getters and setters
    int64_t get_id() const { return id_; }
    void set_id(int64_t id) {
        id_ = id;
        mark_field_dirty("id");
    }

    const std::string& get_name() const { return name_; }
    void set_name(const std::string& name) {
        name_ = name;
        mark_field_dirty("name");
    }

    const std::string& get_email() const { return email_; }
    void set_email(const std::string& email) {
        email_ = email;
        mark_field_dirty("email");
    }

    int get_age() const { return age_; }
    void set_age(int age) {
        age_ = age;
        mark_field_dirty("age");
    }

    // BaseEntity interface
    std::string get_table_name() const override { return "test_users"; }

    DataRow to_data_row() const override {
        DataRow row;
        if (id_ != 0) row["id"] = DataValue(id_);
        row["name"] = DataValue(name_);
        row["email"] = DataValue(email_);
        row["age"] = DataValue(age_);
        return row;
    }

    void from_data_row(const DataRow& row) override {
        auto it = row.find("id");
        if (it != row.end() && !it->second.is_null()) {
            id_ = it->second.as<int64_t>();
        }

        it = row.find("name");
        if (it != row.end()) {
            name_ = it->second.as<std::string>();
        }

        it = row.find("email");
        if (it != row.end()) {
            email_ = it->second.as<std::string>();
        }

        it = row.find("age");
        if (it != row.end()) {
            age_ = it->second.as<int>();
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
// Mock数据源
// =====================================

class MockDataSource : public IDataSource {
private:
    std::unordered_map<std::string, std::vector<DataRow>> tables_;
    std::atomic<int64_t> next_id_{1};

public:
    MockDataSource() {
        // 初始化测试数据
        tables_["test_users"] = {{{"id", DataValue(1L)},
                                  {"name", DataValue("Alice")},
                                  {"email", DataValue("alice@test.com")},
                                  {"age", DataValue(25)}},
                                 {{"id", DataValue(2L)},
                                  {"name", DataValue("Bob")},
                                  {"email", DataValue("bob@test.com")},
                                  {"age", DataValue(30)}},
                                 {{"id", DataValue(3L)},
                                  {"name", DataValue("Charlie")},
                                  {"email", DataValue("charlie@test.com")},
                                  {"age", DataValue(35)}}};
        next_id_ = 4;
    }

    std::future<QueryResult> find(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(10));  // 模拟数据库延迟

            QueryResult result;
            result.success = true;

            auto it = tables_.find(query.get_collection());
            if (it != tables_.end()) {
                result.rows = it->second;

                // 简单的WHERE条件过滤
                if (query.get_criteria()) {
                    result.rows =
                        filter_rows(result.rows, query.get_criteria());
                }

                // 简单的LIMIT处理
                if (query.limit_.has_value() &&
                    result.rows.size() > *query.limit_) {
                    result.rows.resize(*query.limit_);
                }
            }

            return result;
        });
    }

    std::future<QueryResult> find_one(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            auto modified_query = query;
            const_cast<QueryBuilder&>(modified_query).limit(1);
            auto result_future = find(modified_query);
            auto result = result_future.get();

            if (result.success && result.rows.size() > 1) {
                result.rows.resize(1);
            }

            return result;
        });
    }

    std::future<QueryResult> insert(const std::string& collection,
                                    const DataRow& data) override {
        return std::async(std::launch::async, [this, collection, data]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

            QueryResult result;
            result.success = true;
            result.affected_rows = 1;

            auto new_data = data;
            int64_t new_id = next_id_++;
            new_data["id"] = DataValue(new_id);
            result.last_insert_id = DataValue(new_id);

            tables_[collection].push_back(new_data);
            result.rows = {new_data};

            return result;
        });
    }

    std::future<QueryResult> insert_many(
        const std::string& collection,
        const std::vector<DataRow>& data) override {
        return std::async(std::launch::async, [this, collection, data]() {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(data.size() * 2));

            QueryResult result;
            result.success = true;
            result.affected_rows = data.size();

            for (auto row : data) {
                int64_t new_id = next_id_++;
                row["id"] = DataValue(new_id);
                tables_[collection].push_back(row);
                result.rows.push_back(row);
            }

            return result;
        });
    }

    std::future<QueryResult> update(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));

            QueryResult result;
            result.success = true;

            auto& rows = tables_[query.get_collection()];
            size_t updated_count = 0;

            for (auto& row : rows) {
                if (!query.get_criteria() ||
                    matches_criteria(row, query.get_criteria())) {
                    // 更新字段
                    for (const auto& update : query.get_updates()) {
                        row[update.first] = update.second;
                    }
                    updated_count++;
                }
            }

            result.affected_rows = updated_count;
            return result;
        });
    }

    std::future<QueryResult> remove(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(6));

            QueryResult result;
            result.success = true;

            auto& rows = tables_[query.get_collection()];
            size_t original_size = rows.size();

            if (query.get_criteria()) {
                rows.erase(std::remove_if(rows.begin(), rows.end(),
                                          [this, &query](const DataRow& row) {
                                              return matches_criteria(
                                                  row, query.get_criteria());
                                          }),
                           rows.end());
            } else {
                rows.clear();
            }

            result.affected_rows = original_size - rows.size();
            return result;
        });
    }

    std::future<size_t> count(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() -> size_t {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));

            auto it = tables_.find(query.get_collection());
            if (it == tables_.end()) {
                return 0;
            }

            if (!query.get_criteria()) {
                return it->second.size();
            }

            return std::count_if(it->second.begin(), it->second.end(),
                                 [this, &query](const DataRow& row) {
                                     return matches_criteria(
                                         row, query.get_criteria());
                                 });
        });
    }

    std::future<bool> exists(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            auto count_future = count(query);
            return count_future.get() > 0;
        });
    }

    std::unique_ptr<ITransaction> begin_transaction() override {
        return nullptr;  // Mock不实现事务
    }

    std::future<QueryResult> execute_native(
        const std::string& query,
        const std::vector<DataValue>& params) override {
        return std::async(std::launch::async, []() {
            QueryResult result;
            result.success = true;
            return result;
        });
    }

    bool is_connected() const override { return true; }
    bool test_connection() override { return true; }
    void close() override {}
    std::string get_database_type() const override { return "mock"; }
    std::vector<std::string> get_collections() const override {
        std::vector<std::string> collections;
        for (const auto& pair : tables_) {
            collections.push_back(pair.first);
        }
        return collections;
    }

private:
    std::vector<DataRow> filter_rows(
        const std::vector<DataRow>& rows,
        const std::shared_ptr<Criteria>& criteria) {
        std::vector<DataRow> filtered;
        for (const auto& row : rows) {
            if (matches_criteria(row, criteria)) {
                filtered.push_back(row);
            }
        }
        return filtered;
    }

    bool matches_criteria(const DataRow& row,
                          const std::shared_ptr<Criteria>& criteria) {
        if (!criteria) return true;

        if (criteria->get_operator() == Criteria::EQ) {
            auto it = row.find(criteria->get_field());
            if (it != row.end() && !criteria->get_values().empty()) {
                return it->second.to_string() ==
                       criteria->get_values()[0].to_string();
            }
        } else if (criteria->get_operator() == Criteria::GT) {
            auto it = row.find(criteria->get_field());
            if (it != row.end() && !criteria->get_values().empty()) {
                if (it->second.get_type() == DataValue::INTEGER) {
                    return it->second.as<int>() >
                           criteria->get_values()[0].as<int>();
                }
            }
        }

        return false;
    }
};

// =====================================
// 数据访问框架集成测试
// =====================================

class DataAccessFrameworkTest : public Test {
protected:
    void SetUp() override {
        mock_datasource_ = std::make_shared<MockDataSource>();

        CacheConfig cache_config;
        cache_config.max_entries = 100;
        cache_config.default_ttl = std::chrono::seconds{60};

        cache_manager_ = std::make_shared<QueryCacheManager>(cache_config);
        cache_manager_->start();

        cached_datasource_ = std::make_shared<CachedDataSource>(
            mock_datasource_, cache_manager_, cache_config);

        entity_manager_ =
            std::make_shared<EntityManager<TestUser>>(cached_datasource_);
    }

    void TearDown() override { cache_manager_->stop(); }

    std::shared_ptr<MockDataSource> mock_datasource_;
    std::shared_ptr<QueryCacheManager> cache_manager_;
    std::shared_ptr<CachedDataSource> cached_datasource_;
    std::shared_ptr<EntityManager<TestUser>> entity_manager_;
};

// =====================================
// 基本CRUD测试
// =====================================

TEST_F(DataAccessFrameworkTest, BasicCRUDOperations) {
    // 测试查询所有用户
    auto find_all_future = entity_manager_->find_all();
    auto users = find_all_future.get();

    EXPECT_EQ(users.size(), 3);
    EXPECT_EQ(users[0]->get_name(), "Alice");
    EXPECT_EQ(users[1]->get_name(), "Bob");
    EXPECT_EQ(users[2]->get_name(), "Charlie");
}

TEST_F(DataAccessFrameworkTest, FindById) {
    // 测试根据ID查询
    auto find_future = entity_manager_->find(DataValue(1L));
    auto user = find_future.get();

    ASSERT_NE(user, nullptr);
    EXPECT_EQ(user->get_id(), 1);
    EXPECT_EQ(user->get_name(), "Alice");
    EXPECT_EQ(user->get_email(), "alice@test.com");
    EXPECT_EQ(user->get_age(), 25);
}

TEST_F(DataAccessFrameworkTest, InsertUser) {
    // 测试插入新用户
    auto new_user = std::make_shared<TestUser>("David", "david@test.com", 28);

    auto save_future = entity_manager_->save(new_user);
    auto saved_user = save_future.get();

    ASSERT_NE(saved_user, nullptr);
    EXPECT_GT(saved_user->get_id(), 0);
    EXPECT_EQ(saved_user->get_name(), "David");
    EXPECT_EQ(saved_user->get_email(), "david@test.com");
    EXPECT_EQ(saved_user->get_age(), 28);
    EXPECT_TRUE(saved_user->is_managed());
}

TEST_F(DataAccessFrameworkTest, UpdateUser) {
    // 首先查询一个用户
    auto find_future = entity_manager_->find(DataValue(1L));
    auto user = find_future.get();

    ASSERT_NE(user, nullptr);

    // 修改用户信息
    user->set_age(26);
    EXPECT_TRUE(user->has_dirty_fields());

    // 保存更新
    auto save_future = entity_manager_->save(user);
    auto updated_user = save_future.get();

    ASSERT_NE(updated_user, nullptr);
    EXPECT_EQ(updated_user->get_age(), 26);
    EXPECT_FALSE(updated_user->has_dirty_fields());
}

TEST_F(DataAccessFrameworkTest, DeleteUser) {
    // 首先插入一个用户
    auto new_user =
        std::make_shared<TestUser>("ToDelete", "delete@test.com", 40);
    auto save_future = entity_manager_->save(new_user);
    auto saved_user = save_future.get();

    ASSERT_NE(saved_user, nullptr);
    int64_t user_id = saved_user->get_id();

    // 删除用户
    auto remove_future = entity_manager_->remove(saved_user);
    remove_future.get();  // 等待删除完成

    EXPECT_TRUE(saved_user->is_removed());

    // 验证用户已被删除
    auto find_future = entity_manager_->find(DataValue(user_id));
    auto found_user = find_future.get();
    EXPECT_EQ(found_user, nullptr);
}

// =====================================
// 查询构建器测试
// =====================================

TEST_F(DataAccessFrameworkTest, CriteriaQueries) {
    // 测试条件查询
    auto criteria = Criteria::where("age")->greater_than(DataValue(30));
    auto find_future = entity_manager_->find_by_criteria(criteria);
    auto users = find_future.get();

    EXPECT_EQ(users.size(), 1);
    EXPECT_EQ(users[0]->get_name(), "Charlie");
    EXPECT_EQ(users[0]->get_age(), 35);
}

TEST_F(DataAccessFrameworkTest, TypedQueryBuilder) {
    // 测试类型安全的查询构建器
    auto query_future = entity_manager_->query()
                            .where_field_equals("name", std::string("Bob"))
                            .execute();

    auto users = query_future.get();

    EXPECT_EQ(users.size(), 1);
    EXPECT_EQ(users[0]->get_name(), "Bob");
    EXPECT_EQ(users[0]->get_age(), 30);
}

TEST_F(DataAccessFrameworkTest, LimitAndOffset) {
    // 测试分页查询
    auto query_future = entity_manager_->query()
                            .order_by({Sort::asc("name")})
                            .limit(2)
                            .offset(1)
                            .execute();

    auto users = query_future.get();

    EXPECT_EQ(users.size(), 2);
    EXPECT_EQ(users[0]->get_name(), "Bob");      // 按名称排序后的第二个
    EXPECT_EQ(users[1]->get_name(), "Charlie");  // 按名称排序后的第三个
}

// =====================================
// 缓存功能测试
// =====================================

TEST_F(DataAccessFrameworkTest, CachePerformance) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // 第一次查询（缓存未命中）
    auto find_future1 = entity_manager_->find(DataValue(1L));
    auto user1 = find_future1.get();

    auto mid_time = std::chrono::high_resolution_clock::now();

    // 第二次查询（缓存命中）
    auto find_future2 = entity_manager_->find(DataValue(1L));
    auto user2 = find_future2.get();

    auto end_time = std::chrono::high_resolution_clock::now();

    // 验证结果一致
    ASSERT_NE(user1, nullptr);
    ASSERT_NE(user2, nullptr);
    EXPECT_EQ(user1->get_id(), user2->get_id());
    EXPECT_EQ(user1->get_name(), user2->get_name());

    // 验证缓存性能提升
    auto first_duration = std::chrono::duration_cast<std::chrono::microseconds>(
        mid_time - start_time);
    auto second_duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time -
                                                              mid_time);

    // 第二次查询应该明显更快（缓存命中）
    EXPECT_LT(second_duration.count(), first_duration.count());

    // 检查缓存统计
    auto cache_stats = cache_manager_->get_statistics();
    EXPECT_GT(cache_stats.total_requests.load(), 0);
    EXPECT_GT(cache_stats.cache_hits.load(), 0);
}

TEST_F(DataAccessFrameworkTest, CacheInvalidation) {
    // 先查询一次，建立缓存
    auto find_future1 = entity_manager_->find(DataValue(1L));
    auto user1 = find_future1.get();
    ASSERT_NE(user1, nullptr);

    // 修改用户并保存（应该触发缓存失效）
    user1->set_age(99);
    auto save_future = entity_manager_->save(user1);
    save_future.get();

    // 再次查询，应该获取到更新后的数据
    auto find_future2 = entity_manager_->find(DataValue(1L));
    auto user2 = find_future2.get();

    ASSERT_NE(user2, nullptr);
    EXPECT_EQ(user2->get_age(), 99);
}

// =====================================
// 并发测试
// =====================================

TEST_F(DataAccessFrameworkTest, ConcurrentOperations) {
    const int thread_count = 10;
    const int operations_per_thread = 5;

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([this, t, operations_per_thread,
                              &success_count]() {
            for (int i = 0; i < operations_per_thread; ++i) {
                try {
                    // 创建用户
                    auto user = std::make_shared<TestUser>(
                        "User" + std::to_string(t * operations_per_thread + i),
                        "user" + std::to_string(t * operations_per_thread + i) +
                            "@test.com",
                        20 + (t * operations_per_thread + i) % 40);

                    auto save_future = entity_manager_->save(user);
                    auto saved_user = save_future.get();

                    if (saved_user && saved_user->get_id() > 0) {
                        success_count++;
                    }
                } catch (const std::exception& e) {
                    // 记录错误但不中断测试
                    std::cerr << "Concurrent operation failed: " << e.what()
                              << std::endl;
                }
            }
        });
    }

    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }

    // 验证大部分操作成功
    int expected_success = thread_count * operations_per_thread;
    EXPECT_GE(success_count.load(), expected_success * 0.8);  // 至少80%成功率
}

// =====================================
// 实体状态管理测试
// =====================================

TEST_F(DataAccessFrameworkTest, EntityStateManagement) {
    // 新建实体
    auto user = std::make_shared<TestUser>("StateTest", "state@test.com", 30);
    EXPECT_TRUE(user->is_new());
    EXPECT_FALSE(user->is_managed());

    // 保存后应该变为managed状态
    auto save_future = entity_manager_->save(user);
    auto saved_user = save_future.get();

    ASSERT_NE(saved_user, nullptr);
    EXPECT_FALSE(saved_user->is_new());
    EXPECT_TRUE(saved_user->is_managed());

    // 修改实体
    saved_user->set_age(31);
    EXPECT_TRUE(saved_user->has_dirty_fields());

    // 保存修改后dirty fields应该被清除
    auto update_future = entity_manager_->save(saved_user);
    auto updated_user = update_future.get();

    EXPECT_FALSE(updated_user->has_dirty_fields());
    EXPECT_TRUE(updated_user->is_managed());

    // 删除后应该变为removed状态
    auto remove_future = entity_manager_->remove(updated_user);
    remove_future.get();

    EXPECT_TRUE(updated_user->is_removed());
    EXPECT_FALSE(updated_user->is_managed());
}

// =====================================
// 批量操作测试
// =====================================

TEST_F(DataAccessFrameworkTest, BatchOperations) {
    // 创建多个用户
    std::vector<std::shared_ptr<TestUser>> users;
    for (int i = 0; i < 5; ++i) {
        users.push_back(std::make_shared<TestUser>(
            "BatchUser" + std::to_string(i),
            "batch" + std::to_string(i) + "@test.com", 25 + i));
    }

    // 批量保存
    auto save_all_future = entity_manager_->save_all(users);
    auto saved_users = save_all_future.get();

    EXPECT_EQ(saved_users.size(), 5);

    for (size_t i = 0; i < saved_users.size(); ++i) {
        EXPECT_GT(saved_users[i]->get_id(), 0);
        EXPECT_EQ(saved_users[i]->get_name(), "BatchUser" + std::to_string(i));
        EXPECT_TRUE(saved_users[i]->is_managed());
    }
}

// =====================================
// 性能基准测试
// =====================================

TEST_F(DataAccessFrameworkTest, PerformanceBenchmark) {
    const int num_operations = 100;

    auto start_time = std::chrono::high_resolution_clock::now();

    // 执行多次查询操作
    for (int i = 0; i < num_operations; ++i) {
        auto find_future = entity_manager_->find(DataValue(1L + (i % 3)));
        auto user = find_future.get();
        EXPECT_NE(user, nullptr);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);

    std::cout << "Executed " << num_operations << " find operations in "
              << duration.count() << "ms" << std::endl;
    std::cout << "Average time per operation: "
              << (static_cast<double>(duration.count()) / num_operations)
              << "ms" << std::endl;

    // 验证缓存效果
    auto cache_stats = cache_manager_->get_statistics();
    double hit_ratio = cache_stats.get_hit_ratio();
    std::cout << "Cache hit ratio: " << (hit_ratio * 100) << "%" << std::endl;

    // 由于重复查询相同的数据，缓存命中率应该很高
    EXPECT_GT(hit_ratio, 0.5);  // 至少50%的命中率
}

// =====================================
// 主测试运行器
// =====================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    std::cout << "=== Shield数据访问框架集成测试 ===" << std::endl;
    std::cout << "测试包括:" << std::endl;
    std::cout << "- 基本CRUD操作" << std::endl;
    std::cout << "- 查询构建器功能" << std::endl;
    std::cout << "- 缓存性能优化" << std::endl;
    std::cout << "- 并发操作安全性" << std::endl;
    std::cout << "- 实体状态管理" << std::endl;
    std::cout << "- 批量操作" << std::endl;
    std::cout << "- 性能基准测试" << std::endl;
    std::cout << "=================================" << std::endl;

    return RUN_ALL_TESTS();
}