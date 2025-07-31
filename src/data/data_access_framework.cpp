#include "shield/data/data_access_framework.hpp"

#include <chrono>
#include <regex>
#include <sstream>
#include <thread>

// 包含各种数据库驱动
#ifdef SHIELD_USE_MYSQL
#include <mysql/mysql.h>
#endif

#ifdef SHIELD_USE_POSTGRESQL
#include <libpq-fe.h>
#endif

#ifdef SHIELD_USE_MONGODB
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#endif

#ifdef SHIELD_USE_REDIS
#include <hiredis/hiredis.h>
#endif

namespace shield::data {

// =====================================
// DataValue实现
// =====================================

std::string DataValue::to_string() const {
    switch (type_) {
        case NULL_TYPE:
            return "NULL";
        case STRING:
            return as<std::string>();
        case INTEGER:
            return std::to_string(as<int64_t>());
        case DOUBLE:
            return std::to_string(as<double>());
        case BOOLEAN:
            return as<bool>() ? "true" : "false";
        case DATETIME:
            return as<std::string>();  // 假设存储为ISO 8601字符串
        default:
            return "UNKNOWN";
    }
}

std::string DataValue::to_json() const {
    switch (type_) {
        case NULL_TYPE:
            return "null";
        case STRING:
        case DATETIME:
            return "\"" + as<std::string>() + "\"";
        case INTEGER:
            return std::to_string(as<int64_t>());
        case DOUBLE:
            return std::to_string(as<double>());
        case BOOLEAN:
            return as<bool>() ? "true" : "false";
        case OBJECT:
            // 需要JSON序列化库支持
            return "{}";
        case ARRAY:
            return "[]";
        default:
            return "null";
    }
}

// =====================================
// Criteria实现
// =====================================

Criteria::Criteria(const std::string& field, Operator op,
                   const DataValue& value)
    : field_(field), operator_(op), values_({value}) {}

Criteria::Criteria(const std::string& field, Operator op,
                   const std::vector<DataValue>& values)
    : field_(field), operator_(op), values_(values) {}

std::shared_ptr<Criteria> Criteria::where(const std::string& field) {
    return std::make_shared<Criteria>(field, EQ, DataValue());
}

std::shared_ptr<Criteria> Criteria::equals(const DataValue& value) {
    operator_ = EQ;
    values_ = {value};
    return shared_from_this();
}

std::shared_ptr<Criteria> Criteria::not_equals(const DataValue& value) {
    operator_ = NE;
    values_ = {value};
    return shared_from_this();
}

std::shared_ptr<Criteria> Criteria::greater_than(const DataValue& value) {
    operator_ = GT;
    values_ = {value};
    return shared_from_this();
}

std::shared_ptr<Criteria> Criteria::less_than(const DataValue& value) {
    operator_ = LT;
    values_ = {value};
    return shared_from_this();
}

std::shared_ptr<Criteria> Criteria::like(const std::string& pattern) {
    operator_ = LIKE;
    values_ = {DataValue(pattern)};
    return shared_from_this();
}

std::shared_ptr<Criteria> Criteria::in(const std::vector<DataValue>& values) {
    operator_ = IN;
    values_ = values;
    return shared_from_this();
}

std::shared_ptr<Criteria> Criteria::is_null() {
    operator_ = IS_NULL;
    values_.clear();
    return shared_from_this();
}

std::shared_ptr<Criteria> Criteria::between(const DataValue& start,
                                            const DataValue& end) {
    operator_ = BETWEEN;
    values_ = {start, end};
    return shared_from_this();
}

std::shared_ptr<Criteria> Criteria::and_also(std::shared_ptr<Criteria> other) {
    auto combined =
        std::make_shared<Criteria>("", AND, std::vector<DataValue>{});
    combined->sub_criteria_ = {shared_from_this(), other};
    return combined;
}

std::shared_ptr<Criteria> Criteria::or_also(std::shared_ptr<Criteria> other) {
    auto combined =
        std::make_shared<Criteria>("", OR, std::vector<DataValue>{});
    combined->sub_criteria_ = {shared_from_this(), other};
    return combined;
}

// =====================================
// QueryBuilder实现
// =====================================

QueryBuilder& QueryBuilder::select(const std::vector<std::string>& fields) {
    select_fields_ = fields;
    return *this;
}

QueryBuilder& QueryBuilder::where(std::shared_ptr<Criteria> criteria) {
    criteria_ = criteria;
    return *this;
}

QueryBuilder& QueryBuilder::order_by(const std::vector<Sort>& sorts) {
    sorts_ = sorts;
    return *this;
}

QueryBuilder& QueryBuilder::limit(size_t count) {
    limit_ = count;
    return *this;
}

QueryBuilder& QueryBuilder::offset(size_t count) {
    offset_ = count;
    return *this;
}

QueryBuilder& QueryBuilder::page(const Pageable& pageable) {
    limit_ = pageable.size;
    offset_ = pageable.get_offset();
    sorts_ = pageable.sorts;
    return *this;
}

QueryBuilder& QueryBuilder::set(const std::string& field,
                                const DataValue& value) {
    updates_[field] = value;
    return *this;
}

QueryBuilder& QueryBuilder::set(
    const std::unordered_map<std::string, DataValue>& updates) {
    for (const auto& pair : updates) {
        updates_[pair.first] = pair.second;
    }
    return *this;
}

// =====================================
// PostgreSQL数据源实现
// =====================================

class PostgreSQLDataSource : public IDataSource {
private:
    DataSourceConfig config_;
    void* connection_pool_;

public:
    explicit PostgreSQLDataSource(const DataSourceConfig& config)
        : config_(config), connection_pool_(nullptr) {
        initialize_connection_pool();
    }

    ~PostgreSQLDataSource() override { close(); }

    std::future<QueryResult> find(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            std::string sql = build_postgresql_select(query);
            auto params = extract_parameters(query);
            return execute_postgresql_query(sql, params);
        });
    }

    std::future<QueryResult> find_one(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            auto modified_query = query;
            const_cast<QueryBuilder&>(modified_query).limit(1);
            std::string sql = build_postgresql_select(modified_query);
            auto params = extract_parameters(modified_query);
            auto result = execute_postgresql_query(sql, params);

            // 只返回第一行
            if (result.success && result.rows.size() > 1) {
                result.rows.resize(1);
            }

            return result;
        });
    }

    std::future<QueryResult> insert(const std::string& collection,
                                    const DataRow& data) override {
        return std::async(std::launch::async, [this, collection, data]() {
            std::ostringstream sql;
            sql << "INSERT INTO " << collection << " (";

            std::vector<std::string> fields;
            std::vector<DataValue> values;

            for (const auto& pair : data) {
                fields.push_back(pair.first);
                values.push_back(pair.second);
            }

            sql << std::accumulate(
                fields.begin(), fields.end(), std::string(),
                [](const std::string& a, const std::string& b) {
                    return a.empty() ? b : a + ", " + b;
                });

            sql << ") VALUES (";
            for (size_t i = 0; i < values.size(); ++i) {
                sql << "$" << (i + 1);
                if (i < values.size() - 1) sql << ", ";
            }
            sql << ") RETURNING *";

            return execute_postgresql_query(sql.str(), values);
        });
    }

    std::future<QueryResult> insert_many(
        const std::string& collection,
        const std::vector<DataRow>& data) override {
        return std::async(std::launch::async, [this, collection, data]() {
            if (data.empty()) {
                return QueryResult{true, "", {}, 0};
            }

            // 收集所有字段
            std::set<std::string> all_fields;
            for (const auto& row : data) {
                for (const auto& pair : row) {
                    all_fields.insert(pair.first);
                }
            }

            std::vector<std::string> field_list(all_fields.begin(),
                                                all_fields.end());

            std::ostringstream sql;
            sql << "INSERT INTO " << collection << " (";
            sql << std::accumulate(
                field_list.begin(), field_list.end(), std::string(),
                [](const std::string& a, const std::string& b) {
                    return a.empty() ? b : a + ", " + b;
                });
            sql << ") VALUES ";

            std::vector<DataValue> all_values;
            for (size_t i = 0; i < data.size(); ++i) {
                sql << "(";
                for (size_t j = 0; j < field_list.size(); ++j) {
                    sql << "$" << (all_values.size() + 1);

                    auto it = data[i].find(field_list[j]);
                    if (it != data[i].end()) {
                        all_values.push_back(it->second);
                    } else {
                        all_values.push_back(DataValue());  // NULL值
                    }

                    if (j < field_list.size() - 1) sql << ", ";
                }
                sql << ")";
                if (i < data.size() - 1) sql << ", ";
            }

            return execute_postgresql_query(sql.str(), all_values);
        });
    }

    std::future<QueryResult> update(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            std::string sql = build_postgresql_update(query);
            auto params = extract_parameters(query);
            return execute_postgresql_query(sql, params);
        });
    }

    std::future<QueryResult> remove(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            std::string sql = build_postgresql_delete(query);
            auto params = extract_parameters(query);
            return execute_postgresql_query(sql, params);
        });
    }

    std::future<size_t> count(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() -> size_t {
            auto modified_query = query;
            const_cast<QueryBuilder&>(modified_query)
                .select({"COUNT(*) as count"});

            std::string sql = build_postgresql_select(modified_query);
            auto params = extract_parameters(modified_query);
            auto result = execute_postgresql_query(sql, params);

            if (result.success && !result.rows.empty()) {
                auto it = result.rows[0].find("count");
                if (it != result.rows[0].end()) {
                    return std::stoull(it->second.to_string());
                }
            }
            return 0;
        });
    }

    std::future<bool> exists(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            auto count_future = count(query);
            return count_future.get() > 0;
        });
    }

    std::unique_ptr<ITransaction> begin_transaction() override {
        // PostgreSQL事务实现
        return std::make_unique<PostgreSQLTransaction>(this);
    }

    std::future<QueryResult> execute_native(
        const std::string& query,
        const std::vector<DataValue>& params) override {
        return std::async(std::launch::async, [this, query, params]() {
            return execute_postgresql_query(query, params);
        });
    }

    bool is_connected() const override {
        // 检查连接状态
        return connection_pool_ != nullptr;
    }

    bool test_connection() override {
        auto result = execute_postgresql_query("SELECT 1", {});
        return result.success;
    }

    void close() override {
        if (connection_pool_) {
            // 清理连接池
            connection_pool_ = nullptr;
        }
    }

    std::string get_database_type() const override { return "postgresql"; }

    std::vector<std::string> get_collections() const override {
        auto result = execute_postgresql_query(
            "SELECT table_name FROM information_schema.tables WHERE "
            "table_schema = 'public'",
            {});

        std::vector<std::string> tables;
        for (const auto& row : result.rows) {
            auto it = row.find("table_name");
            if (it != row.end()) {
                tables.push_back(it->second.to_string());
            }
        }
        return tables;
    }

private:
    void initialize_connection_pool() {
#ifdef SHIELD_USE_POSTGRESQL
        // 初始化PostgreSQL连接池
        std::ostringstream conn_str;
        conn_str << "host=" << config_.host << " port=" << config_.port
                 << " dbname=" << config_.database
                 << " user=" << config_.username
                 << " password=" << config_.password;

        // 这里应该创建连接池，简化实现
        connection_pool_ = reinterpret_cast<void*>(1);  // Mock

        std::cout << "[PostgreSQL] Connection pool initialized for "
                  << config_.database << std::endl;
#else
        std::cout << "[PostgreSQL] Mock connection pool initialized"
                  << std::endl;
        connection_pool_ = reinterpret_cast<void*>(1);
#endif
    }

    QueryResult execute_postgresql_query(const std::string& sql,
                                         const std::vector<DataValue>& params) {
#ifdef SHIELD_USE_POSTGRESQL
        // 实际PostgreSQL查询执行
        PGconn* conn = PQconnectdb(build_connection_string().c_str());

        if (PQstatus(conn) != CONNECTION_OK) {
            QueryResult result;
            result.success = false;
            result.error = PQerrorMessage(conn);
            PQfinish(conn);
            return result;
        }

        // 准备参数
        std::vector<const char*> param_values;
        std::vector<std::string> param_strings;

        for (const auto& param : params) {
            param_strings.push_back(param.to_string());
            param_values.push_back(param_strings.back().c_str());
        }

        // 执行查询
        PGresult* res = PQexecParams(conn, sql.c_str(), params.size(), nullptr,
                                     param_values.data(), nullptr, nullptr, 0);

        QueryResult result;
        ExecStatusType status = PQresultStatus(res);

        if (status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK) {
            result.success = true;
            result.affected_rows = std::stoull(PQcmdTuples(res));

            // 处理结果集
            int rows = PQntuples(res);
            int cols = PQnfields(res);

            for (int i = 0; i < rows; ++i) {
                DataRow row;
                for (int j = 0; j < cols; ++j) {
                    std::string field_name = PQfname(res, j);
                    std::string field_value = PQgetvalue(res, i, j);
                    row[field_name] = DataValue(field_value);
                }
                result.rows.push_back(std::move(row));
            }
        } else {
            result.success = false;
            result.error = PQerrorMessage(conn);
        }

        PQclear(res);
        PQfinish(conn);
        return result;
#else
        // Mock实现
        QueryResult result;
        result.success = true;

        // 模拟不同类型的查询结果
        if (sql.find("SELECT") == 0) {
            result.rows = {
                {{"id", DataValue(1)}, {"name", DataValue("Test1")}},
                {{"id", DataValue(2)}, {"name", DataValue("Test2")}}};
        } else if (sql.find("INSERT") == 0) {
            result.affected_rows = 1;
            result.last_insert_id = DataValue(123);
        } else if (sql.find("UPDATE") == 0 || sql.find("DELETE") == 0) {
            result.affected_rows = 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return result;
#endif
    }

    std::string build_connection_string() const {
        std::ostringstream conn_str;
        conn_str << "host=" << config_.host << " port=" << config_.port
                 << " dbname=" << config_.database
                 << " user=" << config_.username
                 << " password=" << config_.password;
        return conn_str.str();
    }

    std::string build_postgresql_select(const QueryBuilder& query) {
        std::ostringstream sql;
        sql << "SELECT ";

        if (query.get_select_fields().empty()) {
            sql << "*";
        } else {
            for (size_t i = 0; i < query.get_select_fields().size(); ++i) {
                sql << query.get_select_fields()[i];
                if (i < query.get_select_fields().size() - 1) sql << ", ";
            }
        }

        sql << " FROM " << query.get_collection();

        if (query.get_criteria()) {
            sql << " WHERE " << build_where_clause(query.get_criteria());
        }

        if (!query.get_sorts().empty()) {
            sql << " ORDER BY ";
            for (size_t i = 0; i < query.get_sorts().size(); ++i) {
                const auto& sort = query.get_sorts()[i];
                sql << sort.field << " "
                    << (sort.direction == Sort::ASC ? "ASC" : "DESC");
                if (i < query.get_sorts().size() - 1) sql << ", ";
            }
        }

        if (query.limit_.has_value()) {
            sql << " LIMIT " << *query.limit_;
        }

        if (query.offset_.has_value()) {
            sql << " OFFSET " << *query.offset_;
        }

        return sql.str();
    }

    std::string build_postgresql_update(const QueryBuilder& query) {
        std::ostringstream sql;
        sql << "UPDATE " << query.get_collection() << " SET ";

        const auto& updates = query.get_updates();
        size_t i = 0;
        for (const auto& pair : updates) {
            sql << pair.first << " = $" << (i + 1);
            if (++i < updates.size()) sql << ", ";
        }

        if (query.get_criteria()) {
            sql << " WHERE "
                << build_where_clause(query.get_criteria(), updates.size());
        }

        return sql.str();
    }

    std::string build_postgresql_delete(const QueryBuilder& query) {
        std::ostringstream sql;
        sql << "DELETE FROM " << query.get_collection();

        if (query.get_criteria()) {
            sql << " WHERE " << build_where_clause(query.get_criteria());
        }

        return sql.str();
    }

    std::string build_where_clause(const std::shared_ptr<Criteria>& criteria,
                                   size_t param_offset = 0) {
        // PostgreSQL WHERE子句构建（使用$1, $2...占位符）
        return build_criteria_clause(criteria, param_offset);
    }

    std::string build_criteria_clause(const std::shared_ptr<Criteria>& criteria,
                                      size_t& param_index) {
        if (criteria->get_operator() == Criteria::AND) {
            std::vector<std::string> sub_clauses;
            for (const auto& sub : criteria->get_sub_criteria()) {
                sub_clauses.push_back(build_criteria_clause(sub, param_index));
            }
            return "(" +
                   std::accumulate(
                       sub_clauses.begin(), sub_clauses.end(), std::string(),
                       [](const std::string& a, const std::string& b) {
                           return a.empty() ? b : a + " AND " + b;
                       }) +
                   ")";
        } else if (criteria->get_operator() == Criteria::OR) {
            std::vector<std::string> sub_clauses;
            for (const auto& sub : criteria->get_sub_criteria()) {
                sub_clauses.push_back(build_criteria_clause(sub, param_index));
            }
            return "(" +
                   std::accumulate(
                       sub_clauses.begin(), sub_clauses.end(), std::string(),
                       [](const std::string& a, const std::string& b) {
                           return a.empty() ? b : a + " OR " + b;
                       }) +
                   ")";
        } else {
            return build_single_condition(criteria, param_index);
        }
    }

    std::string build_single_condition(
        const std::shared_ptr<Criteria>& criteria, size_t& param_index) {
        const std::string& field = criteria->get_field();
        Criteria::Operator op = criteria->get_operator();

        switch (op) {
            case Criteria::EQ:
                return field + " = $" + std::to_string(++param_index);
            case Criteria::NE:
                return field + " != $" + std::to_string(++param_index);
            case Criteria::GT:
                return field + " > $" + std::to_string(++param_index);
            case Criteria::LT:
                return field + " < $" + std::to_string(++param_index);
            case Criteria::LIKE:
                return field + " LIKE $" + std::to_string(++param_index);
            case Criteria::IN: {
                std::ostringstream oss;
                oss << field << " IN (";
                for (size_t i = 0; i < criteria->get_values().size(); ++i) {
                    oss << "$" << (++param_index);
                    if (i < criteria->get_values().size() - 1) oss << ", ";
                }
                oss << ")";
                return oss.str();
            }
            case Criteria::IS_NULL:
                return field + " IS NULL";
            case Criteria::BETWEEN:
                return field + " BETWEEN $" + std::to_string(++param_index) +
                       " AND $" + std::to_string(++param_index);
            default:
                return field + " = $" + std::to_string(++param_index);
        }
    }

    std::vector<DataValue> extract_parameters(const QueryBuilder& query) {
        std::vector<DataValue> params;

        // 添加UPDATE参数
        for (const auto& pair : query.get_updates()) {
            params.push_back(pair.second);
        }

        // 添加WHERE参数
        if (query.get_criteria()) {
            extract_criteria_params(query.get_criteria(), params);
        }

        return params;
    }

    void extract_criteria_params(const std::shared_ptr<Criteria>& criteria,
                                 std::vector<DataValue>& params) {
        if (criteria->get_operator() == Criteria::AND ||
            criteria->get_operator() == Criteria::OR) {
            for (const auto& sub : criteria->get_sub_criteria()) {
                extract_criteria_params(sub, params);
            }
        } else if (criteria->get_operator() != Criteria::IS_NULL) {
            for (const auto& value : criteria->get_values()) {
                params.push_back(value);
            }
        }
    }

    class PostgreSQLTransaction : public ITransaction {
    private:
        PostgreSQLDataSource* datasource_;
        bool active_;

    public:
        explicit PostgreSQLTransaction(PostgreSQLDataSource* ds)
            : datasource_(ds), active_(true) {
            // 开始事务
            datasource_->execute_postgresql_query("BEGIN", {});
        }

        ~PostgreSQLTransaction() {
            if (active_) {
                rollback();
            }
        }

        bool commit() override {
            if (!active_) return false;

            auto result = datasource_->execute_postgresql_query("COMMIT", {});
            active_ = false;
            return result.success;
        }

        bool rollback() override {
            if (!active_) return false;

            auto result = datasource_->execute_postgresql_query("ROLLBACK", {});
            active_ = false;
            return result.success;
        }

        bool is_active() const override { return active_; }
    };
};

// =====================================
// Elasticsearch数据源实现
// =====================================

class ElasticsearchDataSource : public IDataSource {
private:
    DataSourceConfig config_;
    void* es_client_;

public:
    explicit ElasticsearchDataSource(const DataSourceConfig& config)
        : config_(config), es_client_(nullptr) {
        initialize_client();
    }

    ~ElasticsearchDataSource() override { close(); }

    // 实现基本接口
    std::future<QueryResult> find(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            std::string es_query = build_elasticsearch_query(query);
            return execute_search(query.get_collection(), es_query);
        });
    }

    std::future<QueryResult> find_one(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            auto modified_query = query;
            const_cast<QueryBuilder&>(modified_query).limit(1);
            std::string es_query = build_elasticsearch_query(modified_query);
            auto result =
                execute_search(modified_query.get_collection(), es_query);

            if (result.success && result.rows.size() > 1) {
                result.rows.resize(1);
            }

            return result;
        });
    }

    std::future<QueryResult> insert(const std::string& collection,
                                    const DataRow& data) override {
        return std::async(std::launch::async, [this, collection, data]() {
            std::string doc_id = generate_document_id(data);
            std::string json_doc = data_row_to_json(data);
            return execute_index(collection, doc_id, json_doc);
        });
    }

    std::future<QueryResult> insert_many(
        const std::string& collection,
        const std::vector<DataRow>& data) override {
        return std::async(std::launch::async, [this, collection, data]() {
            std::string bulk_body = build_bulk_index_body(collection, data);
            return execute_bulk(bulk_body);
        });
    }

    std::future<QueryResult> update(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            std::string update_script = build_update_script(query);
            std::string search_query = build_elasticsearch_query(query);
            return execute_update_by_query(query.get_collection(), search_query,
                                           update_script);
        });
    }

    std::future<QueryResult> remove(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            std::string search_query = build_elasticsearch_query(query);
            return execute_delete_by_query(query.get_collection(),
                                           search_query);
        });
    }

    std::future<size_t> count(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() -> size_t {
            std::string search_query = build_elasticsearch_query(query);
            auto result = execute_count(query.get_collection(), search_query);

            if (result.success && !result.rows.empty()) {
                auto it = result.rows[0].find("count");
                if (it != result.rows[0].end()) {
                    return std::stoull(it->second.to_string());
                }
            }
            return 0;
        });
    }

    std::future<bool> exists(const QueryBuilder& query) override {
        return std::async(std::launch::async, [this, query]() {
            auto count_future = count(query);
            return count_future.get() > 0;
        });
    }

    std::unique_ptr<ITransaction> begin_transaction() override {
        // Elasticsearch不支持传统事务，返回null或NoOp事务
        return nullptr;
    }

    std::future<QueryResult> execute_native(
        const std::string& query,
        const std::vector<DataValue>& params) override {
        return std::async(std::launch::async, [this, query, params]() {
            // 执行原生Elasticsearch查询（JSON格式）
            return execute_raw_query(query);
        });
    }

    bool is_connected() const override { return es_client_ != nullptr; }

    bool test_connection() override {
        auto result = execute_raw_query("{}");
        return result.success;
    }

    void close() override {
        if (es_client_) {
            es_client_ = nullptr;
        }
    }

    std::string get_database_type() const override { return "elasticsearch"; }

    std::vector<std::string> get_collections() const override {
        // 获取所有索引
        auto result = execute_raw_query("GET /_cat/indices?format=json");

        std::vector<std::string> indices;
        for (const auto& row : result.rows) {
            auto it = row.find("index");
            if (it != row.end()) {
                indices.push_back(it->second.to_string());
            }
        }
        return indices;
    }

    // Elasticsearch特有的搜索功能
    std::future<QueryResult> search(const std::string& index,
                                    const std::string& query) {
        return std::async(std::launch::async, [this, index, query]() {
            return execute_search(index, query);
        });
    }

    std::future<QueryResult> full_text_search(
        const std::string& index, const std::string& text,
        const std::vector<std::string>& fields) {
        return std::async(std::launch::async, [this, index, text, fields]() {
            std::ostringstream query;
            query << R"({
                "query": {
                    "multi_match": {
                        "query": ")"
                  << text << R"(",
                        "fields": [)";

            for (size_t i = 0; i < fields.size(); ++i) {
                query << "\"" << fields[i] << "\"";
                if (i < fields.size() - 1) query << ", ";
            }

            query << R"(]
                    }
                }
            })";

            return execute_search(index, query.str());
        });
    }

private:
    void initialize_client() {
        // 初始化Elasticsearch客户端
        std::ostringstream endpoint;
        endpoint << "http://" << config_.host << ":" << config_.port;

#ifdef SHIELD_USE_ELASTICSEARCH
        // 初始化实际的ES客户端
        es_client_ = reinterpret_cast<void*>(1);  // 实际实现
#else
        es_client_ = reinterpret_cast<void*>(1);  // Mock
#endif

        std::cout << "[Elasticsearch] Client initialized for " << endpoint.str()
                  << std::endl;
    }

    std::string build_elasticsearch_query(const QueryBuilder& query) {
        std::ostringstream json;
        json << R"({)";

        // 构建查询部分
        if (query.get_criteria()) {
            json << R"("query": )" << build_es_query_dsl(query.get_criteria());
        } else {
            json << R"("query": {"match_all": {}})";
        }

        // 构建_source部分（字段选择）
        if (!query.get_select_fields().empty()) {
            json << R"(, "_source": [)";
            for (size_t i = 0; i < query.get_select_fields().size(); ++i) {
                json << "\"" << query.get_select_fields()[i] << "\"";
                if (i < query.get_select_fields().size() - 1) json << ", ";
            }
            json << "]";
        }

        // 构建排序
        if (!query.get_sorts().empty()) {
            json << R"(, "sort": [)";
            for (size_t i = 0; i < query.get_sorts().size(); ++i) {
                const auto& sort = query.get_sorts()[i];
                json << R"({")" << sort.field << R"(": {"order": ")"
                     << (sort.direction == Sort::ASC ? "asc" : "desc")
                     << R"("}})";
                if (i < query.get_sorts().size() - 1) json << ", ";
            }
            json << "]";
        }

        // 构建分页
        if (query.limit_.has_value()) {
            json << R"(, "size": )" << *query.limit_;
        }
        if (query.offset_.has_value()) {
            json << R"(, "from": )" << *query.offset_;
        }

        json << "}";
        return json.str();
    }

    std::string build_es_query_dsl(const std::shared_ptr<Criteria>& criteria) {
        if (criteria->get_operator() == Criteria::AND) {
            std::ostringstream json;
            json << R"({"bool": {"must": [)";

            const auto& sub_criteria = criteria->get_sub_criteria();
            for (size_t i = 0; i < sub_criteria.size(); ++i) {
                json << build_es_query_dsl(sub_criteria[i]);
                if (i < sub_criteria.size() - 1) json << ", ";
            }

            json << "]}}";
            return json.str();
        } else if (criteria->get_operator() == Criteria::OR) {
            std::ostringstream json;
            json << R"({"bool": {"should": [)";

            const auto& sub_criteria = criteria->get_sub_criteria();
            for (size_t i = 0; i < sub_criteria.size(); ++i) {
                json << build_es_query_dsl(sub_criteria[i]);
                if (i < sub_criteria.size() - 1) json << ", ";
            }

            json << "]}}";
            return json.str();
        } else {
            return build_es_term_query(criteria);
        }
    }

    std::string build_es_term_query(const std::shared_ptr<Criteria>& criteria) {
        const std::string& field = criteria->get_field();
        Criteria::Operator op = criteria->get_operator();

        std::ostringstream json;

        switch (op) {
            case Criteria::EQ:
                json << R"({"term": {")" << field << R"(": ")"
                     << criteria->get_values()[0].to_string() << R"("}})";
                break;
            case Criteria::GT:
                json << R"({"range": {")" << field << R"(": {"gt": ")"
                     << criteria->get_values()[0].to_string() << R"("}}})";
                break;
            case Criteria::LT:
                json << R"({"range": {")" << field << R"(": {"lt": ")"
                     << criteria->get_values()[0].to_string() << R"("}}})";
                break;
            case Criteria::LIKE:
                json << R"({"wildcard": {")" << field << R"(": ")"
                     << criteria->get_values()[0].to_string() << R"("}})";
                break;
            case Criteria::IN: {
                json << R"({"terms": {")" << field << R"(": [)";
                const auto& values = criteria->get_values();
                for (size_t i = 0; i < values.size(); ++i) {
                    json << "\"" << values[i].to_string() << "\"";
                    if (i < values.size() - 1) json << ", ";
                }
                json << "]}}";
                break;
            }
            case Criteria::BETWEEN:
                json << R"({"range": {")" << field << R"(": {"gte": ")"
                     << criteria->get_values()[0].to_string()
                     << R"(", "lte": ")"
                     << criteria->get_values()[1].to_string() << R"("}}})";
                break;
            default:
                json << R"({"match_all": {}})";
        }

        return json.str();
    }

    // Mock执行方法（实际实现需要ES客户端库）
    QueryResult execute_search(const std::string& index,
                               const std::string& query) {
        QueryResult result;
        result.success = true;

        // Mock搜索结果
        result.rows = {{{"_id", DataValue("1")},
                        {"name", DataValue("Doc1")},
                        {"score", DataValue(1.0)}},
                       {{"_id", DataValue("2")},
                        {"name", DataValue("Doc2")},
                        {"score", DataValue(0.8)}}};

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return result;
    }

    QueryResult execute_index(const std::string& index, const std::string& id,
                              const std::string& document) {
        QueryResult result;
        result.success = true;
        result.affected_rows = 1;
        result.last_insert_id = DataValue(id);

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return result;
    }

    QueryResult execute_bulk(const std::string& bulk_body) {
        QueryResult result;
        result.success = true;

        // 计算批量操作数量
        size_t line_count =
            std::count(bulk_body.begin(), bulk_body.end(), '\n');
        result.affected_rows =
            line_count / 2;  // 每个文档需要2行（metadata + document）

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return result;
    }

    QueryResult execute_count(const std::string& index,
                              const std::string& query) {
        QueryResult result;
        result.success = true;
        result.rows = {{{"count", DataValue(42)}}};

        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        return result;
    }

    QueryResult execute_update_by_query(const std::string& index,
                                        const std::string& query,
                                        const std::string& script) {
        QueryResult result;
        result.success = true;
        result.affected_rows = 5;  // Mock更新数量

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return result;
    }

    QueryResult execute_delete_by_query(const std::string& index,
                                        const std::string& query) {
        QueryResult result;
        result.success = true;
        result.affected_rows = 3;  // Mock删除数量

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        return result;
    }

    QueryResult execute_raw_query(const std::string& query) {
        QueryResult result;
        result.success = true;

        // Mock原生查询结果
        result.rows = {{{"status", DataValue("ok")}}};

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return result;
    }

    std::string generate_document_id(const DataRow& data) {
        auto it = data.find("id");
        if (it != data.end()) {
            return it->second.to_string();
        }

        // 生成UUID
        return "doc_" +
               std::to_string(
                   std::chrono::system_clock::now().time_since_epoch().count());
    }

    std::string data_row_to_json(const DataRow& data) {
        std::ostringstream json;
        json << "{";

        size_t i = 0;
        for (const auto& pair : data) {
            json << "\"" << pair.first << "\": " << pair.second.to_json();
            if (++i < data.size()) json << ", ";
        }

        json << "}";
        return json.str();
    }

    std::string build_bulk_index_body(const std::string& index,
                                      const std::vector<DataRow>& data) {
        std::ostringstream bulk;

        for (const auto& row : data) {
            std::string doc_id = generate_document_id(row);

            // Index metadata line
            bulk << R"({"index": {"_index": ")" << index << R"(", "_id": ")"
                 << doc_id << R"("}})";
            bulk << "\n";

            // Document line
            bulk << data_row_to_json(row);
            bulk << "\n";
        }

        return bulk.str();
    }

    std::string build_update_script(const QueryBuilder& query) {
        std::ostringstream script;
        script << R"({"script": {"source": ")";

        const auto& updates = query.get_updates();
        for (const auto& pair : updates) {
            script << "ctx._source." << pair.first << " = '"
                   << pair.second.to_string() << "'; ";
        }

        script << R"("}})";
        return script.str();
    }
};

// =====================================
// 数据源工厂实现
// =====================================

std::unordered_map<std::string, DataSourceFactory::CreateFunction>
    DataSourceFactory::creators_;

void DataSourceFactory::register_creator(const std::string& type,
                                         CreateFunction creator) {
    creators_[type] = creator;
}

std::unique_ptr<IDataSource> DataSourceFactory::create(
    const DataSourceConfig& config) {
    auto it = creators_.find(config.type);
    if (it == creators_.end()) {
        throw std::runtime_error("Unsupported database type: " + config.type);
    }

    try {
        return it->second(config);
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to create datasource '" + config.type +
                                 "': " + e.what());
    }
}

std::vector<std::string> DataSourceFactory::get_supported_types() {
    std::vector<std::string> types;
    for (const auto& pair : creators_) {
        types.push_back(pair.first);
    }
    return types;
}

void DataSourceFactory::register_built_in_creators() {
    // 注册MySQL
    register_creator("mysql", [](const DataSourceConfig& config) {
        return std::make_unique<MySQLDataSource>(config);
    });

    // 注册PostgreSQL
    register_creator("postgresql", [](const DataSourceConfig& config) {
        return std::make_unique<PostgreSQLDataSource>(config);
    });

    // 注册MongoDB
    register_creator("mongodb", [](const DataSourceConfig& config) {
        return std::make_unique<MongoDataSource>(config);
    });

    // 注册Redis
    register_creator("redis", [](const DataSourceConfig& config) {
        return std::make_unique<RedisDataSource>(config);
    });

    // 注册Elasticsearch
    register_creator("elasticsearch", [](const DataSourceConfig& config) {
        return std::make_unique<ElasticsearchDataSource>(config);
    });

    std::cout << "[DataSourceFactory] Registered built-in data sources: "
              << "mysql, postgresql, mongodb, redis, elasticsearch"
              << std::endl;
}

// =====================================
// Repository基类实现
// =====================================

template <typename T>
std::future<std::optional<T>> BaseRepository<T>::find_by_id(
    const DataValue& id) {
    return std::async(std::launch::async, [this, id]() -> std::optional<T> {
        QueryBuilder query(collection_name_);
        query.where(Criteria::where("id")->equals(id));

        auto result_future = data_source_->find_one(query);
        auto result = result_future.get();

        if (result.success && !result.rows.empty()) {
            return entity_from_row(result.rows[0]);
        }

        return std::nullopt;
    });
}

template <typename T>
std::future<std::vector<T>> BaseRepository<T>::find_all() {
    return std::async(std::launch::async, [this]() {
        QueryBuilder query(collection_name_);

        auto result_future = data_source_->find(query);
        auto result = result_future.get();

        return entities_from_result(result);
    });
}

template <typename T>
std::future<std::vector<T>> BaseRepository<T>::find_by(
    const std::shared_ptr<Criteria>& criteria) {
    return std::async(std::launch::async, [this, criteria]() {
        QueryBuilder query(collection_name_);
        query.where(criteria);

        auto result_future = data_source_->find(query);
        auto result = result_future.get();

        return entities_from_result(result);
    });
}

template <typename T>
std::future<std::vector<T>> BaseRepository<T>::find_by(
    const std::shared_ptr<Criteria>& criteria, const Pageable& pageable) {
    return std::async(std::launch::async, [this, criteria, pageable]() {
        QueryBuilder query(collection_name_);
        query.where(criteria).page(pageable);

        auto result_future = data_source_->find(query);
        auto result = result_future.get();

        return entities_from_result(result);
    });
}

template <typename T>
std::future<T> BaseRepository<T>::save(const T& entity) {
    return std::async(std::launch::async, [this, entity]() -> T {
        DataRow row = entity.to_data_row();
        DataValue id = entity.get_id();

        if (!id.is_null()) {
            // 更新现有实体
            QueryBuilder query(collection_name_);
            query.where(Criteria::where(entity.get_id_field())->equals(id))
                .set(row);

            auto result_future = data_source_->update(query);
            auto result = result_future.get();

            if (result.success) {
                return entity;
            } else {
                throw std::runtime_error("Failed to update entity: " +
                                         result.error);
            }
        } else {
            // 插入新实体
            auto result_future = data_source_->insert(collection_name_, row);
            auto result = result_future.get();

            if (result.success) {
                T updated_entity = entity;
                if (result.last_insert_id.has_value()) {
                    // 设置生成的ID
                    DataRow updated_row = row;
                    updated_row[entity.get_id_field()] = *result.last_insert_id;
                    updated_entity.from_data_row(updated_row);
                }
                return updated_entity;
            } else {
                throw std::runtime_error("Failed to insert entity: " +
                                         result.error);
            }
        }
    });
}

template <typename T>
T BaseRepository<T>::entity_from_row(const DataRow& row) {
    T entity;
    entity.from_data_row(row);
    return entity;
}

template <typename T>
std::vector<T> BaseRepository<T>::entities_from_result(
    const QueryResult& result) {
    std::vector<T> entities;

    if (result.success) {
        entities.reserve(result.rows.size());
        for (const auto& row : result.rows) {
            entities.push_back(entity_from_row(row));
        }
    }

    return entities;
}

}  // namespace shield::data