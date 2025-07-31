#pragma once

#include <any>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace shield::data {

// =====================================
// 1. 通用数据抽象层
// =====================================

// 通用数据值类型
class DataValue {
public:
    enum Type {
        NULL_TYPE,
        STRING,
        INTEGER,
        DOUBLE,
        BOOLEAN,
        BINARY,
        ARRAY,
        OBJECT,
        DATETIME
    };

private:
    Type type_;
    std::any value_;

public:
    DataValue() : type_(NULL_TYPE) {}

    template <typename T>
    DataValue(const T& value) : value_(value) {
        if constexpr (std::is_same_v<T, std::string>) {
            type_ = STRING;
        } else if constexpr (std::is_integral_v<T>) {
            type_ = INTEGER;
        } else if constexpr (std::is_floating_point_v<T>) {
            type_ = DOUBLE;
        } else if constexpr (std::is_same_v<T, bool>) {
            type_ = BOOLEAN;
        } else {
            type_ = OBJECT;
        }
    }

    Type get_type() const { return type_; }

    template <typename T>
    T as() const {
        return std::any_cast<T>(value_);
    }

    std::string to_string() const;
    bool is_null() const { return type_ == NULL_TYPE; }

    // JSON序列化支持
    std::string to_json() const;
    static DataValue from_json(const std::string& json);
};

// 数据行（记录）
using DataRow = std::unordered_map<std::string, DataValue>;

// 查询结果集
struct QueryResult {
    bool success = false;
    std::string error;
    std::vector<DataRow> rows;
    size_t affected_rows = 0;
    std::optional<DataValue> last_insert_id;
    std::unordered_map<std::string, std::string> metadata;
};

// =====================================
// 2. 查询构建器抽象
// =====================================

// 查询条件
class Criteria {
public:
    enum Operator {
        EQ,
        NE,
        GT,
        GE,
        LT,
        LE,
        LIKE,
        IN,
        NOT_IN,
        IS_NULL,
        IS_NOT_NULL,
        BETWEEN,
        AND,
        OR
    };

private:
    std::string field_;
    Operator operator_;
    std::vector<DataValue> values_;
    std::vector<std::shared_ptr<Criteria>> sub_criteria_;

public:
    Criteria(const std::string& field, Operator op, const DataValue& value);
    Criteria(const std::string& field, Operator op,
             const std::vector<DataValue>& values);

    // 链式构建
    static std::shared_ptr<Criteria> where(const std::string& field);
    std::shared_ptr<Criteria> equals(const DataValue& value);
    std::shared_ptr<Criteria> not_equals(const DataValue& value);
    std::shared_ptr<Criteria> greater_than(const DataValue& value);
    std::shared_ptr<Criteria> less_than(const DataValue& value);
    std::shared_ptr<Criteria> like(const std::string& pattern);
    std::shared_ptr<Criteria> in(const std::vector<DataValue>& values);
    std::shared_ptr<Criteria> is_null();
    std::shared_ptr<Criteria> between(const DataValue& start,
                                      const DataValue& end);

    // 逻辑组合
    std::shared_ptr<Criteria> and_also(std::shared_ptr<Criteria> other);
    std::shared_ptr<Criteria> or_also(std::shared_ptr<Criteria> other);

    // 访问器
    const std::string& get_field() const { return field_; }
    Operator get_operator() const { return operator_; }
    const std::vector<DataValue>& get_values() const { return values_; }
    const std::vector<std::shared_ptr<Criteria>>& get_sub_criteria() const {
        return sub_criteria_;
    }
};

// 排序条件
struct Sort {
    enum Direction { ASC, DESC };
    std::string field;
    Direction direction = ASC;

    Sort(const std::string& f, Direction d = ASC) : field(f), direction(d) {}

    static Sort asc(const std::string& field) { return Sort{field, ASC}; }
    static Sort desc(const std::string& field) { return Sort{field, DESC}; }
};

// 分页信息
struct Pageable {
    size_t page = 0;
    size_t size = 20;
    std::vector<Sort> sorts;

    Pageable(size_t p = 0, size_t s = 20) : page(p), size(s) {}

    size_t get_offset() const { return page * size; }
    size_t get_limit() const { return size; }
};

// 查询构建器
class QueryBuilder {
private:
    std::string collection_;  // 表名/集合名
    std::shared_ptr<Criteria> criteria_;
    std::vector<std::string> select_fields_;
    std::vector<Sort> sorts_;
    std::optional<size_t> limit_;
    std::optional<size_t> offset_;
    std::unordered_map<std::string, DataValue> updates_;

public:
    explicit QueryBuilder(const std::string& collection)
        : collection_(collection) {}

    // SELECT操作
    QueryBuilder& select(const std::vector<std::string>& fields);
    QueryBuilder& where(std::shared_ptr<Criteria> criteria);
    QueryBuilder& order_by(const std::vector<Sort>& sorts);
    QueryBuilder& limit(size_t count);
    QueryBuilder& offset(size_t count);
    QueryBuilder& page(const Pageable& pageable);

    // UPDATE操作
    QueryBuilder& set(const std::string& field, const DataValue& value);
    QueryBuilder& set(
        const std::unordered_map<std::string, DataValue>& updates);

    // 构建方法
    std::string build_select_query(const std::string& dialect) const;
    std::string build_update_query(const std::string& dialect) const;
    std::string build_delete_query(const std::string& dialect) const;

    // 访问器
    const std::string& get_collection() const { return collection_; }
    const std::shared_ptr<Criteria>& get_criteria() const { return criteria_; }
    const std::vector<std::string>& get_select_fields() const {
        return select_fields_;
    }
    const std::vector<Sort>& get_sorts() const { return sorts_; }
    const std::unordered_map<std::string, DataValue>& get_updates() const {
        return updates_;
    }
};

// =====================================
// 3. 数据源抽象接口
// =====================================

// 数据库配置
struct DataSourceConfig {
    std::string type;  // mysql, postgresql, mongodb, redis, elasticsearch, etc.
    std::string host = "localhost";
    int port = 0;  // 0表示使用默认端口
    std::string database;
    std::string username;
    std::string password;
    std::unordered_map<std::string, std::string> properties;

    // 连接池配置
    int max_connections = 10;
    int min_connections = 1;
    int connection_timeout = 30;
    bool auto_reconnect = true;
};

// 事务接口
class ITransaction {
public:
    virtual ~ITransaction() = default;
    virtual bool commit() = 0;
    virtual bool rollback() = 0;
    virtual bool is_active() const = 0;
};

// 数据源接口
class IDataSource {
public:
    virtual ~IDataSource() = default;

    // 基本CRUD操作
    virtual std::future<QueryResult> find(const QueryBuilder& query) = 0;
    virtual std::future<QueryResult> find_one(const QueryBuilder& query) = 0;
    virtual std::future<QueryResult> insert(const std::string& collection,
                                            const DataRow& data) = 0;
    virtual std::future<QueryResult> insert_many(
        const std::string& collection, const std::vector<DataRow>& data) = 0;
    virtual std::future<QueryResult> update(const QueryBuilder& query) = 0;
    virtual std::future<QueryResult> remove(const QueryBuilder& query) = 0;

    // 统计操作
    virtual std::future<size_t> count(const QueryBuilder& query) = 0;
    virtual std::future<bool> exists(const QueryBuilder& query) = 0;

    // 事务支持
    virtual std::unique_ptr<ITransaction> begin_transaction() = 0;

    // 原生查询支持
    virtual std::future<QueryResult> execute_native(
        const std::string& query,
        const std::vector<DataValue>& params = {}) = 0;

    // 连接管理
    virtual bool is_connected() const = 0;
    virtual bool test_connection() = 0;
    virtual void close() = 0;

    // 元数据
    virtual std::string get_database_type() const = 0;
    virtual std::vector<std::string> get_collections() const = 0;
};

// =====================================
// 4. SQL数据源实现
// =====================================

// MySQL数据源
class MySQLDataSource : public IDataSource {
private:
    DataSourceConfig config_;
    void* connection_pool_;  // 连接池实现

public:
    explicit MySQLDataSource(const DataSourceConfig& config);
    ~MySQLDataSource() override;

    std::future<QueryResult> find(const QueryBuilder& query) override;
    std::future<QueryResult> find_one(const QueryBuilder& query) override;
    std::future<QueryResult> insert(const std::string& collection,
                                    const DataRow& data) override;
    std::future<QueryResult> insert_many(
        const std::string& collection,
        const std::vector<DataRow>& data) override;
    std::future<QueryResult> update(const QueryBuilder& query) override;
    std::future<QueryResult> remove(const QueryBuilder& query) override;

    std::future<size_t> count(const QueryBuilder& query) override;
    std::future<bool> exists(const QueryBuilder& query) override;

    std::unique_ptr<ITransaction> begin_transaction() override;
    std::future<QueryResult> execute_native(
        const std::string& query,
        const std::vector<DataValue>& params = {}) override;

    bool is_connected() const override;
    bool test_connection() override;
    void close() override;

    std::string get_database_type() const override { return "mysql"; }
    std::vector<std::string> get_collections() const override;

private:
    std::string build_sql_from_query(const QueryBuilder& query) const;
    QueryResult execute_sql(const std::string& sql,
                            const std::vector<DataValue>& params = {}) const;
};

// PostgreSQL数据源
class PostgreSQLDataSource : public IDataSource {
    // 类似MySQL实现，但使用PostgreSQL特定的API
public:
    explicit PostgreSQLDataSource(const DataSourceConfig& config);
    std::string get_database_type() const override { return "postgresql"; }
    // ... 其他方法实现
};

// =====================================
// 5. NoSQL数据源实现
// =====================================

// MongoDB数据源
class MongoDataSource : public IDataSource {
private:
    DataSourceConfig config_;
    void* mongo_client_;  // MongoDB C++ driver客户端

public:
    explicit MongoDataSource(const DataSourceConfig& config);
    ~MongoDataSource() override;

    std::future<QueryResult> find(const QueryBuilder& query) override;
    std::future<QueryResult> find_one(const QueryBuilder& query) override;
    std::future<QueryResult> insert(const std::string& collection,
                                    const DataRow& data) override;
    std::future<QueryResult> insert_many(
        const std::string& collection,
        const std::vector<DataRow>& data) override;
    std::future<QueryResult> update(const QueryBuilder& query) override;
    std::future<QueryResult> remove(const QueryBuilder& query) override;

    std::future<size_t> count(const QueryBuilder& query) override;
    std::future<bool> exists(const QueryBuilder& query) override;

    std::unique_ptr<ITransaction> begin_transaction() override;
    std::future<QueryResult> execute_native(
        const std::string& query,
        const std::vector<DataValue>& params = {}) override;

    bool is_connected() const override;
    bool test_connection() override;
    void close() override;

    std::string get_database_type() const override { return "mongodb"; }
    std::vector<std::string> get_collections() const override;

private:
    std::string build_mongo_query_from_criteria(
        const std::shared_ptr<Criteria>& criteria) const;
    std::string build_mongo_aggregation(const QueryBuilder& query) const;
};

// Redis数据源（键值存储）
class RedisDataSource : public IDataSource {
private:
    DataSourceConfig config_;
    void* redis_client_;

public:
    explicit RedisDataSource(const DataSourceConfig& config);
    ~RedisDataSource() override;

    // Redis特有的操作
    std::future<DataValue> get(const std::string& key);
    std::future<bool> set(const std::string& key, const DataValue& value,
                          int ttl = 0);
    std::future<bool> delete_key(const std::string& key);
    std::future<std::vector<std::string>> keys(const std::string& pattern);

    // 实现基础接口（适配到Redis的键值模型）
    std::future<QueryResult> find(const QueryBuilder& query) override;
    std::future<QueryResult> find_one(const QueryBuilder& query) override;
    std::future<QueryResult> insert(const std::string& collection,
                                    const DataRow& data) override;
    std::future<QueryResult> insert_many(
        const std::string& collection,
        const std::vector<DataRow>& data) override;
    std::future<QueryResult> update(const QueryBuilder& query) override;
    std::future<QueryResult> remove(const QueryBuilder& query) override;

    std::future<size_t> count(const QueryBuilder& query) override;
    std::future<bool> exists(const QueryBuilder& query) override;

    std::unique_ptr<ITransaction> begin_transaction() override;
    std::future<QueryResult> execute_native(
        const std::string& query,
        const std::vector<DataValue>& params = {}) override;

    bool is_connected() const override;
    bool test_connection() override;
    void close() override;

    std::string get_database_type() const override { return "redis"; }
    std::vector<std::string> get_collections() const override;
};

// Elasticsearch数据源
class ElasticsearchDataSource : public IDataSource {
private:
    DataSourceConfig config_;
    void* es_client_;

public:
    explicit ElasticsearchDataSource(const DataSourceConfig& config);
    ~ElasticsearchDataSource() override;

    // Elasticsearch特有的搜索功能
    std::future<QueryResult> search(const std::string& index,
                                    const std::string& query);
    std::future<QueryResult> full_text_search(
        const std::string& index, const std::string& text,
        const std::vector<std::string>& fields);

    std::string get_database_type() const override { return "elasticsearch"; }
    // ... 其他方法实现
};

// =====================================
// 6. 数据源工厂
// =====================================

class DataSourceFactory {
public:
    using CreateFunction =
        std::function<std::unique_ptr<IDataSource>(const DataSourceConfig&)>;

private:
    static std::unordered_map<std::string, CreateFunction> creators_;

public:
    // 注册数据源创建器
    static void register_creator(const std::string& type,
                                 CreateFunction creator);

    // 创建数据源
    stati std::unique_ptr<IDataSource> create(const DataSourceConfig& config);

    // 获取支持的数据源类型
    static std::vector<std::string> get_supported_types();

    // 内置数据源注册
    static void register_built_in_creators();
};

// =====================================
// 7. Repository抽象层
// =====================================

// 实体基类
class Entity {
public:
    virtual ~Entity() = default;
    virtual DataRow to_data_row() const = 0;
    virtual void from_data_row(const DataRow& row) = 0;
    virtual std::string get_id_field() const = 0;
    virtual DataValue get_id() const = 0;
};

// Repository接口
template <typename T>
class IRepository {
    static_assert(std::is_base_of_v<Entity, T>, "T must inherit from Entity");

public:
    virtual ~IRepository() = default;

    // 基本CRUD
    virtual std::future<std::optional<T>> find_by_id(const DataValue& id) = 0;
    virtual std::future<std::vector<T>> find_all() = 0;
    virtual std::future<std::vector<T>> find_by(
        const std::shared_ptr<Criteria>& criteria) = 0;
    virtual std::future<std::vector<T>> find_by(
        const std::shared_ptr<Criteria>& criteria,
        const Pageable& pageable) = 0;

    virtual std::future<T> save(const T& entity) = 0;
    virtual std::future<std::vector<T>> save_all(
        const std::vector<T>& entities) = 0;

    virtual std::future<bool> delete_by_id(const DataValue& id) = 0;
    virtual std::future<size_t> delete_by(
        const std::shared_ptr<Criteria>& criteria) = 0;

    // 统计方法
    virtual std::future<size_t> count() = 0;
    virtual std::future<size_t> count_by(
        const std::shared_ptr<Criteria>& criteria) = 0;
    virtual std::future<bool> exists_by_id(const DataValue& id) = 0;
    virtual std::future<bool> exists_by(
        const std::shared_ptr<Criteria>& criteria) = 0;
};

// Repository实现基类
template <typename T>
class BaseRepository : public IRepository<T> {
protected:
    std::shared_ptr<IDataSource> data_source_;
    std::string collection_name_;

public:
    BaseRepository(std::shared_ptr<IDataSource> ds,
                   const std::string& collection)
        : data_source_(ds), collection_name_(collection) {}

    std::future<std::optional<T>> find_by_id(const DataValue& id) override;
    std::future<std::vector<T>> find_all() override;
    std::future<std::vector<T>> find_by(
        const std::shared_ptr<Criteria>& criteria) override;
    std::future<std::vector<T>> find_by(
        const std::shared_ptr<Criteria>& criteria,
        const Pageable& pageable) override;

    std::future<T> save(const T& entity) override;
    std::future<std::vector<T>> save_all(
        const std::vector<T>& entities) override;

    std::future<bool> delete_by_id(const DataValue& id) override;
    std::future<size_t> delete_by(
        const std::shared_ptr<Criteria>& criteria) override;

    std::future<size_t> count() override;
    std::future<size_t> count_by(
        const std::shared_ptr<Criteria>& criteria) override;
    std::future<bool> exists_by_id(const DataValue& id) override;
    std::future<bool> exists_by(
        const std::shared_ptr<Criteria>& criteria) override;

protected:
    T entity_from_row(const DataRow& row);
    std::vector<T> entities_from_result(const QueryResult& result);
};

}  // namespace shield::data