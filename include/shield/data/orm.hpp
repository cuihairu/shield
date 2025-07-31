#pragma once

#include <reflection>  // C++未来标准或第三方反射库
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>

#include "shield/data/data_access_framework.hpp"

namespace shield::data::orm {

// =====================================
// 字段映射注解
// =====================================

// 字段映射属性
struct FieldMapping {
    std::string column_name;
    std::string column_type;
    bool primary_key = false;
    bool auto_increment = false;
    bool nullable = true;
    size_t max_length = 0;
    std::string default_value;
    bool unique = false;
    bool indexed = false;
};

// 表映射属性
struct TableMapping {
    std::string table_name;
    std::string schema;
    std::vector<std::string> indexes;
    std::vector<std::string> unique_constraints;
};

// =====================================
// 注解宏定义
// =====================================

#define SHIELD_ENTITY(table_name)                                       \
    static constexpr shield::data::orm::TableMapping _table_mapping = { \
        table_name, "", {}, {}};                                        \
    static constexpr const char* _entity_table_name = table_name;

#define SHIELD_FIELD(field_name, column_name, ...)   \
    static constexpr shield::data::orm::FieldMapping \
        _field_##field_name##_mapping = {column_name, ##__VA_ARGS__};

#define SHIELD_PRIMARY_KEY(field_name, column_name)  \
    static constexpr shield::data::orm::FieldMapping \
        _field_##field_name##_mapping = {column_name, "", true, false, false};

#define SHIELD_AUTO_INCREMENT(field_name, column_name) \
    static constexpr shield::data::orm::FieldMapping   \
        _field_##field_name##_mapping = {column_name, "", true, true, false};

// =====================================
// 实体基类
// =====================================

class BaseEntity {
public:
    virtual ~BaseEntity() = default;

    // 核心接口
    virtual std::string get_table_name() const = 0;
    virtual DataRow to_data_row() const = 0;
    virtual void from_data_row(const DataRow& row) = 0;
    virtual std::string get_primary_key_field() const = 0;
    virtual DataValue get_primary_key_value() const = 0;
    virtual void set_primary_key_value(const DataValue& value) = 0;

    // 实体状态
    enum class EntityState {
        NEW,       // 新建，未保存
        MANAGED,   // 已保存，被管理
        DETACHED,  // 已分离，不被管理
        REMOVED    // 已标记删除
    };

    EntityState get_state() const { return state_; }
    void set_state(EntityState state) { state_ = state; }

    bool is_new() const { return state_ == EntityState::NEW; }
    bool is_managed() const { return state_ == EntityState::MANAGED; }
    bool is_detached() const { return state_ == EntityState::DETACHED; }
    bool is_removed() const { return state_ == EntityState::REMOVED; }

    // 脏字段跟踪
    const std::set<std::string>& get_dirty_fields() const {
        return dirty_fields_;
    }
    void mark_field_dirty(const std::string& field) {
        dirty_fields_.insert(field);
    }
    void clear_dirty_fields() { dirty_fields_.clear(); }
    bool has_dirty_fields() const { return !dirty_fields_.empty(); }

protected:
    EntityState state_ = EntityState::NEW;
    std::set<std::string> dirty_fields_;
};

// =====================================
// 实体元数据
// =====================================

template <typename EntityType>
struct EntityMetadata {
    using entity_type = EntityType;

    std::string table_name;
    std::string primary_key_field;
    std::unordered_map<std::string, FieldMapping> field_mappings;
    std::unordered_map<std::string, std::string> property_to_column;
    std::unordered_map<std::string, std::string> column_to_property;

    // 通过反射或手动注册初始化
    void initialize() {
        // 这里需要反射支持来自动发现字段映射
        // 或者通过宏和模板特化来手动注册

        if constexpr (requires { EntityType::_entity_table_name; }) {
            table_name = EntityType::_entity_table_name;
        }

        // 注册字段映射（需要编译期反射或手动注册）
        register_field_mappings();
    }

private:
    void register_field_mappings() {
        // 这里需要编译期反射来自动发现
        // 暂时使用手动注册的方式
    }
};

// =====================================
// 实体管理器
// =====================================

template <typename EntityType>
class EntityManager {
private:
    std::shared_ptr<IDataSource> data_source_;
    EntityMetadata<EntityType> metadata_;

    // 一级缓存 (Identity Map)
    std::unordered_map<DataValue, std::shared_ptr<EntityType>> identity_map_;
    mutable std::mutex cache_mutex_;

public:
    explicit EntityManager(std::shared_ptr<IDataSource> data_source)
        : data_source_(data_source) {
        metadata_.initialize();
    }

    // =====================================
    // CRUD操作
    // =====================================

    std::future<std::shared_ptr<EntityType>> find(const DataValue& id) {
        return std::async(
            std::launch::async, [this, id]() -> std::shared_ptr<EntityType> {
                // 首先检查一级缓存
                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    auto it = identity_map_.find(id);
                    if (it != identity_map_.end()) {
                        return it->second;
                    }
                }

                // 从数据库查询
                QueryBuilder query(metadata_.table_name);
                query.where(
                    Criteria::where(metadata_.primary_key_field)->equals(id));

                auto result_future = data_source_->find_one(query);
                auto result = result_future.get();

                if (result.success && !result.rows.empty()) {
                    auto entity = std::make_shared<EntityType>();
                    entity->from_data_row(result.rows[0]);
                    entity->set_state(BaseEntity::EntityState::MANAGED);
                    entity->clear_dirty_fields();

                    // 加入一级缓存
                    {
                        std::lock_guard<std::mutex> lock(cache_mutex_);
                        identity_map_[id] = entity;
                    }

                    return entity;
                }

                return nullptr;
            });
    }

    std::future<std::vector<std::shared_ptr<EntityType>>> find_all() {
        return std::async(std::launch::async, [this]() {
            QueryBuilder query(metadata_.table_name);

            auto result_future = data_source_->find(query);
            auto result = result_future.get();

            std::vector<std::shared_ptr<EntityType>> entities;

            if (result.success) {
                entities.reserve(result.rows.size());

                for (const auto& row : result.rows) {
                    auto entity = std::make_shared<EntityType>();
                    entity->from_data_row(row);
                    entity->set_state(BaseEntity::EntityState::MANAGED);
                    entity->clear_dirty_fields();

                    // 检查是否已在缓存中
                    DataValue pk = entity->get_primary_key_value();
                    {
                        std::lock_guard<std::mutex> lock(cache_mutex_);
                        auto it = identity_map_.find(pk);
                        if (it != identity_map_.end()) {
                            entities.push_back(it->second);
                        } else {
                            identity_map_[pk] = entity;
                            entities.push_back(entity);
                        }
                    }
                }
            }

            return entities;
        });
    }

    std::future<std::vector<std::shared_ptr<EntityType>>> find_by_criteria(
        std::shared_ptr<Criteria> criteria) {
        return std::async(std::launch::async, [this, criteria]() {
            QueryBuilder query(metadata_.table_name);
            query.where(criteria);

            auto result_future = data_source_->find(query);
            auto result = result_future.get();

            return entities_from_result(result);
        });
    }

    std::future<std::shared_ptr<EntityType>> save(
        std::shared_ptr<EntityType> entity) {
        return std::async(
            std::launch::async,
            [this, entity]() -> std::shared_ptr<EntityType> {
                if (!entity) {
                    throw std::invalid_argument("Entity cannot be null");
                }

                if (entity->is_new()) {
                    return insert_entity(entity);
                } else if (entity->has_dirty_fields()) {
                    return update_entity(entity);
                } else {
                    // 没有变更，直接返回
                    return entity;
                }
            });
    }

    std::future<void> remove(std::shared_ptr<EntityType> entity) {
        return std::async(std::launch::async, [this, entity]() {
            if (!entity) {
                throw std::invalid_argument("Entity cannot be null");
            }

            DataValue pk = entity->get_primary_key_value();
            if (pk.is_null()) {
                throw std::runtime_error(
                    "Cannot delete entity without primary key");
            }

            QueryBuilder query(metadata_.table_name);
            query.where(
                Criteria::where(metadata_.primary_key_field)->equals(pk));

            auto result_future = data_source_->remove(query);
            auto result = result_future.get();

            if (result.success) {
                entity->set_state(BaseEntity::EntityState::REMOVED);

                // 从缓存中移除
                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    identity_map_.erase(pk);
                }
            } else {
                throw std::runtime_error("Failed to delete entity: " +
                                         result.error);
            }
        });
    }

    // =====================================
    // 批量操作
    // =====================================

    std::future<std::vector<std::shared_ptr<EntityType>>> save_all(
        const std::vector<std::shared_ptr<EntityType>>& entities) {
        return std::async(std::launch::async, [this, entities]() {
            std::vector<std::shared_ptr<EntityType>> saved_entities;
            saved_entities.reserve(entities.size());

            // 分离新实体和更新实体
            std::vector<DataRow> new_entities_data;
            std::vector<std::shared_ptr<EntityType>> entities_to_update;

            for (const auto& entity : entities) {
                if (entity->is_new()) {
                    new_entities_data.push_back(entity->to_data_row());
                } else if (entity->has_dirty_fields()) {
                    entities_to_update.push_back(entity);
                }
            }

            // 批量插入新实体
            if (!new_entities_data.empty()) {
                auto insert_future = data_source_->insert_many(
                    metadata_.table_name, new_entities_data);
                auto insert_result = insert_future.get();

                if (!insert_result.success) {
                    throw std::runtime_error("Batch insert failed: " +
                                             insert_result.error);
                }
            }

            // 逐个更新现有实体
            for (const auto& entity : entities_to_update) {
                auto updated = update_entity(entity);
                saved_entities.push_back(updated);
            }

            return saved_entities;
        });
    }

    // =====================================
    // 查询构建器支持
    // =====================================

    class TypedQueryBuilder {
    private:
        EntityManager<EntityType>* manager_;
        QueryBuilder query_;

    public:
        TypedQueryBuilder(EntityManager<EntityType>* manager)
            : manager_(manager), query_(manager->metadata_.table_name) {}

        TypedQueryBuilder& where(std::shared_ptr<Criteria> criteria) {
            query_.where(criteria);
            return *this;
        }

        template <typename FieldType>
        TypedQueryBuilder& where_field_equals(const std::string& field,
                                              const FieldType& value) {
            query_.where(Criteria::where(field)->equals(DataValue(value)));
            return *this;
        }

        TypedQueryBuilder& order_by(const std::vector<Sort>& sorts) {
            query_.order_by(sorts);
            return *this;
        }

        TypedQueryBuilder& limit(size_t count) {
            query_.limit(count);
            return *this;
        }

        TypedQueryBuilder& offset(size_t count) {
            query_.offset(count);
            return *this;
        }

        std::future<std::vector<std::shared_ptr<EntityType>>> execute() {
            auto result_future = manager_->data_source_->find(query_);

            return std::async(
                std::launch::async,
                [this, result_future = std::move(result_future)]() {
                    auto result = result_future.get();
                    return manager_->entities_from_result(result);
                });
        }

        std::future<std::shared_ptr<EntityType>> execute_single() {
            query_.limit(1);
            auto entities_future = execute();

            return std::async(
                std::launch::async,
                [entities_future = std::move(
                     entities_future)]() -> std::shared_ptr<EntityType> {
                    auto entities = entities_future.get();
                    return entities.empty() ? nullptr : entities[0];
                });
        }

        std::future<size_t> count() {
            return manager_->data_source_->count(query_);
        }

        std::future<bool> exists() {
            return manager_->data_source_->exists(query_);
        }
    };

    TypedQueryBuilder query() { return TypedQueryBuilder(this); }

    // =====================================
    // 缓存管理
    // =====================================

    void clear_cache() {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        identity_map_.clear();
    }

    void evict(const DataValue& id) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        identity_map_.erase(id);
    }

    void evict(std::shared_ptr<EntityType> entity) {
        if (entity) {
            evict(entity->get_primary_key_value());
        }
    }

    size_t cache_size() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return identity_map_.size();
    }

    // =====================================
    // 事务支持
    // =====================================

    class Transaction {
    private:
        std::unique_ptr<ITransaction> db_transaction_;
        EntityManager<EntityType>* manager_;
        std::vector<std::shared_ptr<EntityType>> managed_entities_;

    public:
        Transaction(EntityManager<EntityType>* manager) : manager_(manager) {
            db_transaction_ = manager_->data_source_->begin_transaction();
        }

        ~Transaction() {
            if (db_transaction_ && db_transaction_->is_active()) {
                rollback();
            }
        }

        void add_entity(std::shared_ptr<EntityType> entity) {
            managed_entities_.push_back(entity);
        }

        bool commit() {
            if (db_transaction_) {
                bool success = db_transaction_->commit();

                if (success) {
                    // 更新实体状态
                    for (auto& entity : managed_entities_) {
                        entity->set_state(BaseEntity::EntityState::MANAGED);
                        entity->clear_dirty_fields();
                    }
                }

                return success;
            }
            return false;
        }

        bool rollback() {
            if (db_transaction_) {
                bool success = db_transaction_->rollback();

                // 恢复实体状态
                for (auto& entity : managed_entities_) {
                    entity->set_state(BaseEntity::EntityState::DETACHED);
                }

                return success;
            }
            return false;
        }

        bool is_active() const {
            return db_transaction_ && db_transaction_->is_active();
        }
    };

    std::unique_ptr<Transaction> begin_transaction() {
        return std::make_unique<Transaction>(this);
    }

private:
    std::shared_ptr<EntityType> insert_entity(
        std::shared_ptr<EntityType> entity) {
        DataRow row = entity->to_data_row();

        auto result_future = data_source_->insert(metadata_.table_name, row);
        auto result = result_future.get();

        if (result.success) {
            // 设置自增主键
            if (result.last_insert_id.has_value()) {
                entity->set_primary_key_value(*result.last_insert_id);
            }

            entity->set_state(BaseEntity::EntityState::MANAGED);
            entity->clear_dirty_fields();

            // 加入缓存
            DataValue pk = entity->get_primary_key_value();
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                identity_map_[pk] = entity;
            }

            return entity;
        } else {
            throw std::runtime_error("Failed to insert entity: " +
                                     result.error);
        }
    }

    std::shared_ptr<EntityType> update_entity(
        std::shared_ptr<EntityType> entity) {
        DataValue pk = entity->get_primary_key_value();
        if (pk.is_null()) {
            throw std::runtime_error(
                "Cannot update entity without primary key");
        }

        // 只更新脏字段
        std::unordered_map<std::string, DataValue> updates;
        DataRow full_row = entity->to_data_row();

        for (const std::string& field : entity->get_dirty_fields()) {
            auto it = full_row.find(field);
            if (it != full_row.end()) {
                updates[field] = it->second;
            }
        }

        if (updates.empty()) {
            return entity;  // 没有实际更新
        }

        QueryBuilder query(metadata_.table_name);
        query.where(Criteria::where(metadata_.primary_key_field)->equals(pk))
            .set(updates);

        auto result_future = data_source_->update(query);
        auto result = result_future.get();

        if (result.success) {
            entity->clear_dirty_fields();
            return entity;
        } else {
            throw std::runtime_error("Failed to update entity: " +
                                     result.error);
        }
    }

    std::vector<std::shared_ptr<EntityType>> entities_from_result(
        const QueryResult& result) {
        std::vector<std::shared_ptr<EntityType>> entities;

        if (result.success) {
            entities.reserve(result.rows.size());

            for (const auto& row : result.rows) {
                auto entity = std::make_shared<EntityType>();
                entity->from_data_row(row);
                entity->set_state(BaseEntity::EntityState::MANAGED);
                entity->clear_dirty_fields();

                // 检查缓存
                DataValue pk = entity->get_primary_key_value();
                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    auto it = identity_map_.find(pk);
                    if (it != identity_map_.end()) {
                        entities.push_back(it->second);
                    } else {
                        identity_map_[pk] = entity;
                        entities.push_back(entity);
                    }
                }
            }
        }

        return entities;
    }
};

// =====================================
// ORM配置
// =====================================

struct ORMConfig {
    bool enable_lazy_loading = true;
    bool enable_dirty_tracking = true;
    bool enable_identity_map = true;
    size_t identity_map_max_size = 10000;
    std::chrono::seconds cache_ttl{3600};
    bool log_sql = false;
    bool validate_entities = true;
};

// =====================================
// ORM会话管理
// =====================================

class ORMSession {
private:
    std::shared_ptr<IDataSource> data_source_;
    ORMConfig config_;
    std::unordered_map<std::string, std::shared_ptr<void>> entity_managers_;
    mutable std::mutex managers_mutex_;

public:
    explicit ORMSession(std::shared_ptr<IDataSource> data_source,
                        const ORMConfig& config = {})
        : data_source_(data_source), config_(config) {}

    template <typename EntityType>
    std::shared_ptr<EntityManager<EntityType>> get_entity_manager() {
        std::lock_guard<std::mutex> lock(managers_mutex_);

        std::string type_name = typeid(EntityType).name();
        auto it = entity_managers_.find(type_name);

        if (it != entity_managers_.end()) {
            return std::static_pointer_cast<EntityManager<EntityType>>(
                it->second);
        }

        auto manager =
            std::make_shared<EntityManager<EntityType>>(data_source_);
        entity_managers_[type_name] = manager;

        return manager;
    }

    template <typename EntityType>
    std::future<std::shared_ptr<EntityType>> find(const DataValue& id) {
        auto manager = get_entity_manager<EntityType>();
        return manager->find(id);
    }

    template <typename EntityType>
    std::future<std::shared_ptr<EntityType>> save(
        std::shared_ptr<EntityType> entity) {
        auto manager = get_entity_manager<EntityType>();
        return manager->save(entity);
    }

    template <typename EntityType>
    std::future<void> remove(std::shared_ptr<EntityType> entity) {
        auto manager = get_entity_manager<EntityType>();
        return manager->remove(entity);
    }

    void clear_all_caches() {
        std::lock_guard<std::mutex> lock(managers_mutex_);
        // 这里需要类型擦除的缓存清理接口
        entity_managers_.clear();
    }

    const ORMConfig& get_config() const { return config_; }
};

// =====================================
// ORM工厂
// =====================================

class ORMFactory {
public:
    static std::shared_ptr<ORMSession> create_session(
        std::shared_ptr<IDataSource> data_source,
        const ORMConfig& config = {}) {
        return std::make_shared<ORMSession>(data_source, config);
    }

    static std::shared_ptr<ORMSession> create_session_with_pool(
        const std::string& pool_name, const ORMConfig& config = {}) {
        // 这里需要集成连接池管理器
        // auto pool = ConnectionPoolManager::instance().get_pool(pool_name);
        // auto data_source = create_pooled_data_source(pool);
        // return create_session(data_source, config);
        return nullptr;
    }
};

}  // namespace shield::data::orm