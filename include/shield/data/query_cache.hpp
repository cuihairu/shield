#pragma once

#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "shield/data/data_access_framework.hpp"

namespace shield::data::cache {

// =====================================
// 缓存配置
// =====================================

struct CacheConfig {
    size_t max_entries = 1000;
    std::chrono::seconds default_ttl{300};      // 5分钟
    std::chrono::seconds cleanup_interval{60};  // 1分钟清理一次
    bool enable_statistics = true;
    bool enable_async_refresh = true;
    double hit_ratio_threshold = 0.8;  // 命中率阈值
};

// =====================================
// 缓存键管理
// =====================================

class CacheKey {
private:
    std::string query_hash_;
    std::string collection_;
    std::string params_hash_;

public:
    CacheKey(const std::string& collection, const QueryBuilder& query);
    CacheKey(const std::string& collection, const std::string& native_query,
             const std::vector<DataValue>& params);

    std::string to_string() const;
    bool operator==(const CacheKey& other) const;
    bool operator<(const CacheKey& other) const;

private:
    std::string hash_query(const QueryBuilder& query) const;
    std::string hash_params(const std::vector<DataValue>& params) const;
    std::string compute_sha256(const std::string& input) const;
};

// CacheKey哈希函数
struct CacheKeyHash {
    std::size_t operator()(const CacheKey& key) const {
        return std::hash<std::string>{}(key.to_string());
    }
};

// =====================================
// 缓存条目
// =====================================

class CacheEntry {
private:
    QueryResult result_;
    std::chrono::steady_clock::time_point created_at_;
    std::chrono::steady_clock::time_point last_accessed_;
    std::chrono::seconds ttl_;
    std::atomic<size_t> access_count_{0};
    mutable std::mutex access_mutex_;

public:
    CacheEntry(const QueryResult& result, std::chrono::seconds ttl);

    const QueryResult& get_result() const;
    bool is_expired() const;
    void update_access_time();

    std::chrono::steady_clock::time_point get_created_time() const {
        return created_at_;
    }
    std::chrono::steady_clock::time_point get_last_accessed_time() const {
        return last_accessed_;
    }
    size_t get_access_count() const { return access_count_.load(); }
    std::chrono::seconds get_ttl() const { return ttl_; }

    std::chrono::milliseconds get_age() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - created_at_);
    }
};

// =====================================
// LRU缓存实现
// =====================================

template <typename Key, typename Value>
class LRUCache {
private:
    struct Node {
        Key key;
        Value value;
        std::shared_ptr<Node> prev;
        std::shared_ptr<Node> next;

        Node(const Key& k, const Value& v) : key(k), value(v) {}
    };

    size_t capacity_;
    std::unordered_map<Key, std::shared_ptr<Node>, CacheKeyHash> cache_;
    std::shared_ptr<Node> head_;
    std::shared_ptr<Node> tail_;
    mutable std::mutex cache_mutex_;

public:
    explicit LRUCache(size_t capacity) : capacity_(capacity) {
        head_ = std::make_shared<Node>(Key{}, Value{});
        tail_ = std::make_shared<Node>(Key{}, Value{});
        head_->next = tail_;
        tail_->prev = head_;
    }

    std::optional<Value> get(const Key& key) {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        auto it = cache_.find(key);
        if (it == cache_.end()) {
            return std::nullopt;
        }

        // 移动到头部
        move_to_head(it->second);
        return it->second->value;
    }

    void put(const Key& key, const Value& value) {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        auto it = cache_.find(key);
        if (it != cache_.end()) {
            // 更新现有条目
            it->second->value = value;
            move_to_head(it->second);
        } else {
            // 添加新条目
            auto node = std::make_shared<Node>(key, value);

            if (cache_.size() >= capacity_) {
                // 移除尾部节点
                auto tail_node = remove_tail();
                cache_.erase(tail_node->key);
            }

            cache_[key] = node;
            add_to_head(node);
        }
    }

    void remove(const Key& key) {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        auto it = cache_.find(key);
        if (it != cache_.end()) {
            remove_node(it->second);
            cache_.erase(it);
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cache_.clear();
        head_->next = tail_;
        tail_->prev = head_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return cache_.size();
    }

    std::vector<Key> get_all_keys() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        std::vector<Key> keys;
        keys.reserve(cache_.size());

        for (const auto& pair : cache_) {
            keys.push_back(pair.first);
        }
        return keys;
    }

private:
    void add_to_head(std::shared_ptr<Node> node) {
        node->prev = head_;
        node->next = head_->next;
        head_->next->prev = node;
        head_->next = node;
    }

    void remove_node(std::shared_ptr<Node> node) {
        node->prev->next = node->next;
        node->next->prev = node->prev;
    }

    void move_to_head(std::shared_ptr<Node> node) {
        remove_node(node);
        add_to_head(node);
    }

    std::shared_ptr<Node> remove_tail() {
        auto last_node = tail_->prev;
        remove_node(last_node);
        return last_node;
    }
};

// =====================================
// 查询缓存统计
// =====================================

struct CacheStatistics {
    std::atomic<size_t> total_requests{0};
    std::atomic<size_t> cache_hits{0};
    std::atomic<size_t> cache_misses{0};
    std::atomic<size_t> cache_evictions{0};
    std::atomic<size_t> cache_size{0};

    std::chrono::steady_clock::time_point start_time;

    CacheStatistics() : start_time(std::chrono::steady_clock::now()) {}

    double get_hit_ratio() const {
        size_t total = total_requests.load();
        return total > 0 ? static_cast<double>(cache_hits.load()) / total : 0.0;
    }

    double get_miss_ratio() const { return 1.0 - get_hit_ratio(); }

    std::chrono::milliseconds get_uptime() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
    }

    void reset() {
        total_requests = 0;
        cache_hits = 0;
        cache_misses = 0;
        cache_evictions = 0;
        start_time = std::chrono::steady_clock::now();
    }
};

// =====================================
// 主查询缓存管理器
// =====================================

class QueryCacheManager {
private:
    CacheConfig config_;
    LRUCache<CacheKey, std::shared_ptr<CacheEntry>> cache_;
    CacheStatistics statistics_;

    std::atomic<bool> running_{false};
    std::thread cleanup_thread_;
    std::thread refresh_thread_;

    mutable std::mutex refresh_mutex_;
    std::unordered_set<std::string> refreshing_keys_;

public:
    explicit QueryCacheManager(const CacheConfig& config = {});
    ~QueryCacheManager();

    // 缓存操作
    std::optional<QueryResult> get(const CacheKey& key);
    void put(const CacheKey& key, const QueryResult& result,
             std::chrono::seconds ttl = std::chrono::seconds{0});
    void invalidate(const CacheKey& key);
    void invalidate_collection(const std::string& collection);
    void clear();

    // 统计信息
    CacheStatistics get_statistics() const { return statistics_; }
    void reset_statistics() { statistics_.reset(); }

    // 生命周期管理
    void start();
    void stop();

    // 高级功能
    void preload_cache(
        const std::vector<std::pair<CacheKey, QueryResult>>& entries);
    std::vector<CacheKey> get_expired_keys() const;
    void refresh_expired_entries_async();

private:
    void cleanup_loop();
    void refresh_loop();
    void cleanup_expired_entries();
    void update_statistics_on_hit();
    void update_statistics_on_miss();
    void update_statistics_on_eviction();
};

// =====================================
// 缓存装饰器数据源
// =====================================

class CachedDataSource : public IDataSource {
private:
    std::shared_ptr<IDataSource> underlying_datasource_;
    std::shared_ptr<QueryCacheManager> cache_manager_;
    CacheConfig cache_config_;

public:
    CachedDataSource(std::shared_ptr<IDataSource> datasource,
                     std::shared_ptr<QueryCacheManager> cache_manager,
                     const CacheConfig& config = {});

    // 带缓存的数据源操作
    std::future<QueryResult> find(const QueryBuilder& query) override;
    std::future<QueryResult> find_one(const QueryBuilder& query) override;
    std::future<size_t> count(const QueryBuilder& query) override;
    std::future<bool> exists(const QueryBuilder& query) override;

    // 写操作会触发缓存失效
    std::future<QueryResult> insert(const std::string& collection,
                                    const DataRow& data) override;
    std::future<QueryResult> insert_many(
        const std::string& collection,
        const std::vector<DataRow>& data) override;
    std::future<QueryResult> update(const QueryBuilder& query) override;
    std::future<QueryResult> remove(const QueryBuilder& query) override;

    // 委托给底层数据源
    std::unique_ptr<ITransaction> begin_transaction() override;
    std::future<QueryResult> execute_native(
        const std::string& query,
        const std::vector<DataValue>& params = {}) override;

    bool is_connected() const override;
    bool test_connection() override;
    void close() override;

    std::string get_database_type() const override;
    std::vector<std::string> get_collections() const override;

    // 缓存管理
    void invalidate_collection_cache(const std::string& collection);
    CacheStatistics get_cache_statistics() const;

private:
    template <typename ResultType>
    std::future<ResultType> execute_with_cache(
        const std::string& collection, const QueryBuilder& query,
        std::function<std::future<ResultType>()> executor);

    std::chrono::seconds get_cache_ttl_for_operation(
        const std::string& operation) const;
};

// =====================================
// 二级缓存（分布式缓存支持）
// =====================================

class DistributedCacheProvider {
public:
    virtual ~DistributedCacheProvider() = default;

    virtual std::future<std::optional<std::string>> get_async(
        const std::string& key) = 0;
    virtual std::future<bool> set_async(const std::string& key,
                                        const std::string& value,
                                        std::chrono::seconds ttl) = 0;
    virtual std::future<bool> delete_async(const std::string& key) = 0;
    virtual std::future<std::vector<std::string>> keys_async(
        const std::string& pattern) = 0;
};

class RedisDistributedCache : public DistributedCacheProvider {
private:
    std::shared_ptr<RedisDataSource> redis_client_;

public:
    explicit RedisDistributedCache(
        std::shared_ptr<RedisDataSource> redis_client);

    std::future<std::optional<std::string>> get_async(
        const std::string& key) override;
    std::future<bool> set_async(const std::string& key,
                                const std::string& value,
                                std::chrono::seconds ttl) override;
    std::future<bool> delete_async(const std::string& key) override;
    std::future<std::vector<std::string>> keys_async(
        const std::string& pattern) override;
};

class L2QueryCacheManager {
private:
    std::shared_ptr<QueryCacheManager> l1_cache_;         // 本地缓存
    std::shared_ptr<DistributedCacheProvider> l2_cache_;  // 分布式缓存
    CacheConfig config_;

public:
    L2QueryCacheManager(std::shared_ptr<QueryCacheManager> l1_cache,
                        std::shared_ptr<DistributedCacheProvider> l2_cache,
                        const CacheConfig& config = {});

    std::future<std::optional<QueryResult>> get_async(const CacheKey& key);
    std::future<bool> put_async(const CacheKey& key, const QueryResult& result,
                                std::chrono::seconds ttl = std::chrono::seconds{
                                    0});
    std::future<bool> invalidate_async(const CacheKey& key);

private:
    std::string serialize_query_result(const QueryResult& result) const;
    QueryResult deserialize_query_result(const std::string& data) const;
};

// =====================================
// 性能监控和优化
// =====================================

struct QueryPerformanceMetrics {
    std::string query_signature;
    std::chrono::milliseconds avg_execution_time{0};
    std::chrono::milliseconds min_execution_time{
        std::chrono::milliseconds::max()};
    std::chrono::milliseconds max_execution_time{0};
    size_t execution_count = 0;
    size_t cache_hit_count = 0;
    double cache_hit_ratio = 0.0;
    std::chrono::steady_clock::time_point last_executed;
};

class QueryPerformanceMonitor {
private:
    std::unordered_map<std::string, QueryPerformanceMetrics> metrics_;
    mutable std::mutex metrics_mutex_;
    std::atomic<bool> monitoring_enabled_{true};

public:
    void record_query_execution(const std::string& signature,
                                std::chrono::milliseconds execution_time,
                                bool cache_hit = false);

    QueryPerformanceMetrics get_metrics(const std::string& signature) const;
    std::vector<QueryPerformanceMetrics> get_top_slow_queries(
        size_t limit = 10) const;
    std::vector<QueryPerformanceMetrics> get_most_frequent_queries(
        size_t limit = 10) const;

    void enable_monitoring() { monitoring_enabled_ = true; }
    void disable_monitoring() { monitoring_enabled_ = false; }
    bool is_monitoring_enabled() const { return monitoring_enabled_; }

    void reset_metrics();
    void export_metrics_to_json(const std::string& filename) const;
};

// =====================================
// 自适应缓存策略
// =====================================

class AdaptiveCacheStrategy {
private:
    std::shared_ptr<QueryPerformanceMonitor> performance_monitor_;
    CacheConfig base_config_;

    std::mutex strategy_mutex_;
    std::unordered_map<std::string, std::chrono::seconds>
        collection_ttl_overrides_;

public:
    explicit AdaptiveCacheStrategy(
        std::shared_ptr<QueryPerformanceMonitor> monitor,
        const CacheConfig& base_config = {});

    std::chrono::seconds get_optimal_ttl(const std::string& collection,
                                         const QueryBuilder& query) const;
    bool should_cache_query(const std::string& collection,
                            const QueryBuilder& query) const;
    size_t get_optimal_cache_size() const;

    void analyze_and_adjust();

private:
    std::chrono::seconds calculate_ttl_based_on_performance(
        const std::string& signature) const;
    bool is_query_worth_caching(const QueryPerformanceMetrics& metrics) const;
};

}  // namespace shield::data::cache