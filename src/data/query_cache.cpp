#include "shield/data/query_cache.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace shield::data::cache {

// =====================================
// CacheKey实现
// =====================================

CacheKey::CacheKey(const std::string& collection, const QueryBuilder& query)
    : collection_(collection) {
    query_hash_ = hash_query(query);
    // 从query中提取参数并hash
    std::vector<DataValue> params;  // 需要从query中提取参数
    params_hash_ = hash_params(params);
}

CacheKey::CacheKey(const std::string& collection,
                   const std::string& native_query,
                   const std::vector<DataValue>& params)
    : collection_(collection),
      query_hash_(compute_sha256(native_query)),
      params_hash_(hash_params(params)) {}

std::string CacheKey::to_string() const {
    return collection_ + ":" + query_hash_ + ":" + params_hash_;
}

bool CacheKey::operator==(const CacheKey& other) const {
    return collection_ == other.collection_ &&
           query_hash_ == other.query_hash_ &&
           params_hash_ == other.params_hash_;
}

bool CacheKey::operator<(const CacheKey& other) const {
    if (collection_ != other.collection_)
        return collection_ < other.collection_;
    if (query_hash_ != other.query_hash_)
        return query_hash_ < other.query_hash_;
    return params_hash_ < other.params_hash_;
}

std::string CacheKey::hash_query(const QueryBuilder& query) const {
    std::ostringstream oss;

    // 序列化查询构建器的各个部分
    oss << "SELECT:";
    for (const auto& field : query.get_select_fields()) {
        oss << field << ",";
    }

    oss << "|WHERE:";
    if (query.get_criteria()) {
        // 这里需要递归序列化criteria
        oss << "criteria_present";
    }

    oss << "|SORT:";
    for (const auto& sort : query.get_sorts()) {
        oss << sort.field << ":"
            << (sort.direction == Sort::ASC ? "asc" : "desc") << ",";
    }

    oss << "|UPDATES:";
    for (const auto& update : query.get_updates()) {
        oss << update.first << ":" << update.second.to_string() << ",";
    }

    return compute_sha256(oss.str());
}

std::string CacheKey::hash_params(const std::vector<DataValue>& params) const {
    if (params.empty()) return "no_params";

    std::ostringstream oss;
    for (const auto& param : params) {
        oss << param.to_string() << "|";
    }

    return compute_sha256(oss.str());
}

std::string CacheKey::compute_sha256(const std::string& input) const {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, input.c_str(), input.length());
    SHA256_Final(hash, &sha256);

    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(hash[i]);
    }
    return oss.str();
}

// =====================================
// CacheEntry实现
// =====================================

CacheEntry::CacheEntry(const QueryResult& result, std::chrono::seconds ttl)
    : result_(result), ttl_(ttl) {
    auto now = std::chrono::steady_clock::now();
    created_at_ = now;
    last_accessed_ = now;
}

const QueryResult& CacheEntry::get_result() const {
    std::lock_guard<std::mutex> lock(access_mutex_);
    access_count_++;
    const_cast<CacheEntry*>(this)->last_accessed_ =
        std::chrono::steady_clock::now();
    return result_;
}

bool CacheEntry::is_expired() const {
    if (ttl_.count() == 0) return false;  // 永不过期

    auto now = std::chrono::steady_clock::now();
    return (now - created_at_) > ttl_;
}

void CacheEntry::update_access_time() {
    std::lock_guard<std::mutex> lock(access_mutex_);
    last_accessed_ = std::chrono::steady_clock::now();
}

// =====================================
// QueryCacheManager实现
// =====================================

QueryCacheManager::QueryCacheManager(const CacheConfig& config)
    : config_(config), cache_(config.max_entries) {}

QueryCacheManager::~QueryCacheManager() { stop(); }

std::optional<QueryResult> QueryCacheManager::get(const CacheKey& key) {
    auto entry = cache_.get(key);
    if (!entry.has_value()) {
        update_statistics_on_miss();
        return std::nullopt;
    }

    if ((*entry)->is_expired()) {
        cache_.remove(key);
        update_statistics_on_miss();
        return std::nullopt;
    }

    update_statistics_on_hit();
    return (*entry)->get_result();
}

void QueryCacheManager::put(const CacheKey& key, const QueryResult& result,
                            std::chrono::seconds ttl) {
    if (ttl.count() == 0) {
        ttl = config_.default_ttl;
    }

    auto entry = std::make_shared<CacheEntry>(result, ttl);
    cache_.put(key, entry);
    statistics_.cache_size = cache_.size();
}

void QueryCacheManager::invalidate(const CacheKey& key) {
    cache_.remove(key);
    statistics_.cache_size = cache_.size();
}

void QueryCacheManager::invalidate_collection(const std::string& collection) {
    auto all_keys = cache_.get_all_keys();
    for (const auto& key : all_keys) {
        if (key.to_string().find(collection + ":") == 0) {
            cache_.remove(key);
        }
    }
    statistics_.cache_size = cache_.size();
}

void QueryCacheManager::clear() {
    cache_.clear();
    statistics_.cache_size = 0;
}

void QueryCacheManager::start() {
    if (running_) return;

    running_ = true;
    cleanup_thread_ = std::thread(&QueryCacheManager::cleanup_loop, this);

    if (config_.enable_async_refresh) {
        refresh_thread_ = std::thread(&QueryCacheManager::refresh_loop, this);
    }

    std::cout << "[QueryCache] Started with max_entries=" << config_.max_entries
              << ", ttl=" << config_.default_ttl.count() << "s" << std::endl;
}

void QueryCacheManager::stop() {
    running_ = false;

    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }

    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }

    std::cout << "[QueryCache] Stopped. Final statistics - Hits: "
              << statistics_.cache_hits.load()
              << ", Misses: " << statistics_.cache_misses.load()
              << ", Hit ratio: " << std::fixed << std::setprecision(2)
              << (statistics_.get_hit_ratio() * 100) << "%" << std::endl;
}

void QueryCacheManager::cleanup_loop() {
    while (running_) {
        std::this_thread::sleep_for(config_.cleanup_interval);

        if (!running_) break;

        try {
            cleanup_expired_entries();
        } catch (const std::exception& e) {
            std::cerr << "[QueryCache] Cleanup error: " << e.what()
                      << std::endl;
        }
    }
}

void QueryCacheManager::refresh_loop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds{30});  // 30秒检查一次

        if (!running_) break;

        try {
            refresh_expired_entries_async();
        } catch (const std::exception& e) {
            std::cerr << "[QueryCache] Refresh error: " << e.what()
                      << std::endl;
        }
    }
}

void QueryCacheManager::cleanup_expired_entries() {
    auto all_keys = cache_.get_all_keys();
    size_t cleaned_count = 0;

    for (const auto& key : all_keys) {
        auto entry = cache_.get(key);
        if (entry.has_value() && (*entry)->is_expired()) {
            cache_.remove(key);
            cleaned_count++;
            update_statistics_on_eviction();
        }
    }

    statistics_.cache_size = cache_.size();

    if (cleaned_count > 0) {
        std::cout << "[QueryCache] Cleaned up " << cleaned_count
                  << " expired entries" << std::endl;
    }
}

std::vector<CacheKey> QueryCacheManager::get_expired_keys() const {
    std::vector<CacheKey> expired_keys;
    auto all_keys = cache_.get_all_keys();

    for (const auto& key : all_keys) {
        auto entry = cache_.get(key);
        if (entry.has_value() && (*entry)->is_expired()) {
            expired_keys.push_back(key);
        }
    }

    return expired_keys;
}

void QueryCacheManager::refresh_expired_entries_async() {
    auto expired_keys = get_expired_keys();

    for (const auto& key : expired_keys) {
        std::lock_guard<std::mutex> lock(refresh_mutex_);
        if (refreshing_keys_.find(key.to_string()) == refreshing_keys_.end()) {
            refreshing_keys_.insert(key.to_string());

            // 异步刷新任务
            std::thread([this, key]() {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});

                // 实际的刷新逻辑需要访问原始数据源，这里只是模拟
                std::cout << "[QueryCache] Async refresh for key: "
                          << key.to_string() << std::endl;

                {
                    std::lock_guard<std::mutex> lock(refresh_mutex_);
                    refreshing_keys_.erase(key.to_string());
                }
            }).detach();
        }
    }
}

void QueryCacheManager::preload_cache(
    const std::vector<std::pair<CacheKey, QueryResult>>& entries) {
    for (const auto& entry : entries) {
        put(entry.first, entry.second);
    }

    std::cout << "[QueryCache] Preloaded " << entries.size() << " cache entries"
              << std::endl;
}

void QueryCacheManager::update_statistics_on_hit() {
    statistics_.total_requests++;
    statistics_.cache_hits++;
}

void QueryCacheManager::update_statistics_on_miss() {
    statistics_.total_requests++;
    statistics_.cache_misses++;
}

void QueryCacheManager::update_statistics_on_eviction() {
    statistics_.cache_evictions++;
}

// =====================================
// CachedDataSource实现
// =====================================

CachedDataSource::CachedDataSource(
    std::shared_ptr<IDataSource> datasource,
    std::shared_ptr<QueryCacheManager> cache_manager, const CacheConfig& config)
    : underlying_datasource_(datasource),
      cache_manager_(cache_manager),
      cache_config_(config) {}

std::future<QueryResult> CachedDataSource::find(const QueryBuilder& query) {
    return execute_with_cache<QueryResult>(
        query.get_collection(), query,
        [this, query]() { return underlying_datasource_->find(query); });
}

std::future<QueryResult> CachedDataSource::find_one(const QueryBuilder& query) {
    return execute_with_cache<QueryResult>(
        query.get_collection(), query,
        [this, query]() { return underlying_datasource_->find_one(query); });
}

std::future<size_t> CachedDataSource::count(const QueryBuilder& query) {
    CacheKey cache_key(query.get_collection() + "_count", query);

    return std::async(std::launch::async, [this, cache_key, query]() -> size_t {
        auto cached_result = cache_manager_->get(cache_key);
        if (cached_result.has_value()) {
            // 从缓存的QueryResult中提取count
            if (!cached_result->rows.empty()) {
                auto it = cached_result->rows[0].find("count");
                if (it != cached_result->rows[0].end()) {
                    return std::stoull(it->second.to_string());
                }
            }
        }

        // 缓存未命中，执行查询
        auto result_future = underlying_datasource_->count(query);
        size_t count = result_future.get();

        // 将结果转换为QueryResult并缓存
        QueryResult cache_result;
        cache_result.success = true;
        cache_result.rows = {
            {{\"count\", DataValue(static_cast<int64_t>(count))}}};

                cache_manager_->put(cache_key, cache_result,
                                    get_cache_ttl_for_operation("count"));

        return count;
    });
}

std::future<bool> CachedDataSource::exists(const QueryBuilder& query) {
    return std::async(std::launch::async, [this, query]() {
        auto count_future = count(query);
        return count_future.get() > 0;
    });
}

std::future<QueryResult> CachedDataSource::insert(const std::string& collection,
                                                  const DataRow& data) {
    return std::async(std::launch::async, [this, collection, data]() {
        auto result_future = underlying_datasource_->insert(collection, data);
        auto result = result_future.get();

        if (result.success) {
            // 插入成功，失效相关缓存
            invalidate_collection_cache(collection);
        }

        return result;
    });
}

std::future<QueryResult> CachedDataSource::insert_many(
    const std::string& collection, const std::vector<DataRow>& data) {
    return std::async(std::launch::async, [this, collection, data]() {
        auto result_future =
            underlying_datasource_->insert_many(collection, data);
        auto result = result_future.get();

        if (result.success) {
            invalidate_collection_cache(collection);
        }

        return result;
    });
}

std::future<QueryResult> CachedDataSource::update(const QueryBuilder& query) {
    return std::async(std::launch::async, [this, query]() {
        auto result_future = underlying_datasource_->update(query);
        auto result = result_future.get();

        if (result.success) {
            invalidate_collection_cache(query.get_collection());
        }

        return result;
    });
}

std::future<QueryResult> CachedDataSource::remove(const QueryBuilder& query) {
    return std::async(std::launch::async, [this, query]() {
        auto result_future = underlying_datasource_->remove(query);
        auto result = result_future.get();

        if (result.success) {
            invalidate_collection_cache(query.get_collection());
        }

        return result;
    });
}

template <typename ResultType>
std::future<ResultType> CachedDataSource::execute_with_cache(
    const std::string& collection, const QueryBuilder& query,
    std::function<std::future<ResultType>()> executor) {
    CacheKey cache_key(collection, query);

    return std::async(
        std::launch::async,
        [this, cache_key, executor, collection]() -> ResultType {
            // 尝试从缓存获取
            auto cached_result = cache_manager_->get(cache_key);
            if (cached_result.has_value()) {
                if constexpr (std::is_same_v<ResultType, QueryResult>) {
                    return *cached_result;
                }
            }

            // 缓存未命中，执行实际查询
            auto result_future = executor();
            auto result = result_future.get();

            if constexpr (std::is_same_v<ResultType, QueryResult>) {
                if (result.success) {
                    // 缓存成功的查询结果
                    cache_manager_->put(cache_key, result,
                                        get_cache_ttl_for_operation("select"));
                }
            }

            return result;
        });
}

std::chrono::seconds CachedDataSource::get_cache_ttl_for_operation(
    const std::string& operation) const {
    if (operation == "count") {
        return std::chrono::seconds{60};  // count查询缓存1分钟
    } else if (operation == "select") {
        return cache_config_.default_ttl;
    }
    return cache_config_.default_ttl;
}

std::unique_ptr<ITransaction> CachedDataSource::begin_transaction() {
    return underlying_datasource_->begin_transaction();
}

std::future<QueryResult> CachedDataSource::execute_native(
    const std::string& query, const std::vector<DataValue>& params) {
    // 原生查询也支持缓存
    CacheKey cache_key("native", query, params);

    return std::async(std::launch::async, [this, cache_key, query, params]() {
        auto cached_result = cache_manager_->get(cache_key);
        if (cached_result.has_value()) {
            return *cached_result;
        }

        auto result_future =
            underlying_datasource_->execute_native(query, params);
        auto result = result_future.get();

        if (result.success) {
            cache_manager_->put(cache_key, result,
                                std::chrono::seconds{300});  // 5分钟TTL
        }

        return result;
    });
}

bool CachedDataSource::is_connected() const {
    return underlying_datasource_->is_connected();
}

bool CachedDataSource::test_connection() {
    return underlying_datasource_->test_connection();
}

void CachedDataSource::close() { underlying_datasource_->close(); }

std::string CachedDataSource::get_database_type() const {
    return underlying_datasource_->get_database_type() + "_cached";
}

std::vector<std::string> CachedDataSource::get_collections() const {
    return underlying_datasource_->get_collections();
}

void CachedDataSource::invalidate_collection_cache(
    const std::string& collection) {
    cache_manager_->invalidate_collection(collection);
}

CacheStatistics CachedDataSource::get_cache_statistics() const {
    return cache_manager_->get_statistics();
}

// =====================================
// QueryPerformanceMonitor实现
// =====================================

void QueryPerformanceMonitor::record_query_execution(
    const std::string& signature, std::chrono::milliseconds execution_time,
    bool cache_hit) {
    if (!monitoring_enabled_) return;

    std::lock_guard<std::mutex> lock(metrics_mutex_);

    auto& metrics = metrics_[signature];
    if (metrics.query_signature.empty()) {
        metrics.query_signature = signature;
        metrics.min_execution_time = execution_time;
        metrics.max_execution_time = execution_time;
    }

    metrics.execution_count++;
    metrics.last_executed = std::chrono::steady_clock::now();

    if (cache_hit) {
        metrics.cache_hit_count++;
    }

    // 更新执行时间统计
    if (execution_time < metrics.min_execution_time) {
        metrics.min_execution_time = execution_time;
    }
    if (execution_time > metrics.max_execution_time) {
        metrics.max_execution_time = execution_time;
    }

    // 计算平均执行时间
    auto total_time =
        metrics.avg_execution_time * (metrics.execution_count - 1) +
        execution_time;
    metrics.avg_execution_time = total_time / metrics.execution_count;

    // 计算缓存命中率
    metrics.cache_hit_ratio =
        static_cast<double>(metrics.cache_hit_count) / metrics.execution_count;
}

QueryPerformanceMetrics QueryPerformanceMonitor::get_metrics(
    const std::string& signature) const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);

    auto it = metrics_.find(signature);
    if (it != metrics_.end()) {
        return it->second;
    }

    return QueryPerformanceMetrics{signature, {}, {}, {}, 0, 0, 0.0, {}};
}

std::vector<QueryPerformanceMetrics>
QueryPerformanceMonitor::get_top_slow_queries(size_t limit) const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);

    std::vector<QueryPerformanceMetrics> all_metrics;
    all_metrics.reserve(metrics_.size());

    for (const auto& pair : metrics_) {
        all_metrics.push_back(pair.second);
    }

    // 按平均执行时间排序
    std::sort(
        all_metrics.begin(), all_metrics.end(),
        [](const QueryPerformanceMetrics& a, const QueryPerformanceMetrics& b) {
            return a.avg_execution_time > b.avg_execution_time;
        });

    if (all_metrics.size() > limit) {
        all_metrics.resize(limit);
    }

    return all_metrics;
}

std::vector<QueryPerformanceMetrics>
QueryPerformanceMonitor::get_most_frequent_queries(size_t limit) const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);

    std::vector<QueryPerformanceMetrics> all_metrics;
    all_metrics.reserve(metrics_.size());

    for (const auto& pair : metrics_) {
        all_metrics.push_back(pair.second);
    }

    // 按执行次数排序
    std::sort(
        all_metrics.begin(), all_metrics.end(),
        [](const QueryPerformanceMetrics& a, const QueryPerformanceMetrics& b) {
            return a.execution_count > b.execution_count;
        });

    if (all_metrics.size() > limit) {
        all_metrics.resize(limit);
    }

    return all_metrics;
}

void QueryPerformanceMonitor::reset_metrics() {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    metrics_.clear();
}

void QueryPerformanceMonitor::export_metrics_to_json(
    const std::string& filename) const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);

    std::ofstream file(filename);
    file << "{\n  \"query_metrics\": [\n";

    bool first = true;
    for (const auto& pair : metrics_) {
        if (!first) file << ",\n";
        first = false;

        const auto& metrics = pair.second;
        file << "    {\n";
        file << "      \"signature\": \"" << metrics.query_signature << "\",\n";
        file << "      \"avg_execution_time_ms\": "
             << metrics.avg_execution_time.count() << ",\n";
        file << "      \"min_execution_time_ms\": "
             << metrics.min_execution_time.count() << ",\n";
        file << "      \"max_execution_time_ms\": "
             << metrics.max_execution_time.count() << ",\n";
        file << "      \"execution_count\": " << metrics.execution_count
             << ",\n";
        file << "      \"cache_hit_count\": " << metrics.cache_hit_count
             << ",\n";
        file << "      \"cache_hit_ratio\": " << std::fixed
             << std::setprecision(4) << metrics.cache_hit_ratio << "\n";
        file << "    }";
    }

    file << "\n  ]\n}\n";
    file.close();

    std::cout << "[PerformanceMonitor] Exported " << metrics_.size()
              << " query metrics to " << filename << std::endl;
}

}  // namespace shield::data::cache