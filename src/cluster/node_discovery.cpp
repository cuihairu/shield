// [SHIELD_CLUSTER] Node discovery: loop + redis++ backend.
//
// The discovery loop registers this node in the backend, renews the
// registration with heartbeats, scans for peers, and reports appearances
// (dial) and disappearances (suspect) to the manager via callbacks. All
// storage calls go through the NodeDiscoveryBackend seam so tests drive the
// loop with an in-memory fake — no Redis server needed.

#include "shield/cluster/node_discovery.hpp"

#include <sw/redis++/redis++.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "shield/log/logger.hpp"

namespace shield::cluster {
namespace {

using namespace std::chrono_literals;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------
// Redis backend
// ---------------------------------------------------------------------------

class RedisDiscoveryBackend final : public NodeDiscoveryBackend {
public:
    explicit RedisDiscoveryBackend(const RedisDiscoveryConfig& cfg)
        : cfg_(cfg) {}

    void register_node(const std::string& node_id, const std::string& address,
                       int ttl_seconds) override {
        auto redis = connect();
        const std::string key = node_key(node_id);
        redis->hset(key, "addr", address);
        redis->hset(key, "status", "online");
        redis->hset(key, "started_at", std::to_string(now_ms()));
        redis->hset(key, "last_heartbeat", std::to_string(now_ms()));
        redis->expire(key, ttl_seconds);
        redis->sadd(set_key(), node_id);
    }

    void heartbeat(const std::string& node_id, int ttl_seconds) override {
        auto redis = connect();
        redis->hset(node_key(node_id), "last_heartbeat",
                    std::to_string(now_ms()));
        redis->expire(node_key(node_id), ttl_seconds);
    }

    std::vector<DiscoveredNode> scan(const std::string& self_id) override {
        auto redis = connect();
        std::vector<DiscoveredNode> out;
        std::set<std::string> members;
        redis->smembers(set_key(), std::inserter(members, members.begin()));
        for (const auto& id : members) {
            if (id == self_id) continue;
            std::map<std::string, std::string> fields;
            redis->hgetall(node_key(id), std::inserter(fields, fields.begin()));
            auto it = fields.find("addr");
            if (it == fields.end() || it->second.empty()) continue;
            DiscoveredNode node;
            node.node_id = id;
            node.address = it->second;
            out.push_back(std::move(node));
        }
        return out;
    }

    void unregister_node(const std::string& node_id) override {
        auto redis = connect();
        redis->del(node_key(node_id));
        redis->srem(set_key(), node_id);
    }

private:
    const std::string& node_key(const std::string& node_id) const {
        static thread_local std::string key;
        key = cfg_.prefix + ":" + node_id;
        return key;
    }
    const std::string& set_key() const {
        static thread_local std::string key;
        key = cfg_.prefix;
        return key;
    }

    // Lazy connect: construction must not block, so the Redis handle is
    // built on first use. Every command call re-resolves the handle.
    std::shared_ptr<sw::redis::Redis> connect() {
        std::lock_guard<std::mutex> lock(mu_);
        if (redis_) return redis_;
        sw::redis::ConnectionOptions opts;
        opts.host = cfg_.host;
        opts.port = cfg_.port > 0 ? cfg_.port : 6379;
        if (!cfg_.password.empty()) opts.password = cfg_.password;
        opts.db = cfg_.db > 0 ? cfg_.db : 0;
        opts.connect_timeout = 2000ms;
        opts.socket_timeout = 2000ms;
        redis_ = std::make_shared<sw::redis::Redis>(opts);
        return redis_;
    }

    RedisDiscoveryConfig cfg_;
    std::mutex mu_;
    std::shared_ptr<sw::redis::Redis> redis_;
};

}  // namespace

// ---------------------------------------------------------------------------
// NodeDiscovery loop
// ---------------------------------------------------------------------------

struct NodeDiscovery::Impl {
    std::string node_id;
    std::string address;
    int ttl_seconds;
    int interval_ms;
    std::unique_ptr<NodeDiscoveryBackend> backend;
    NodeDiscovery::DiscoveredFn on_discovered;
    NodeDiscovery::LostFn on_lost;

    std::mutex mu;
    std::map<std::string, std::string> known;  // node_id -> address
    std::jthread thread;
    bool started = false;

    // Callbacks run under mu: the manager's handlers only touch the
    // manager's own state, never back into this loop.
    void diff(const std::vector<DiscoveredNode>& nodes);
};

NodeDiscovery::NodeDiscovery(std::string node_id, std::string listen_address,
                             std::unique_ptr<NodeDiscoveryBackend> backend,
                             int heartbeat_interval_ms, int scan_interval_ms,
                             int ttl_seconds)
    : impl_(std::make_unique<Impl>()) {
    impl_->node_id = std::move(node_id);
    impl_->address = std::move(listen_address);
    impl_->backend = std::move(backend);
    // Loop cadence: the finer of the two intervals; heartbeats and scans
    // share the loop so a short test interval drives both.
    impl_->interval_ms =
        std::max(1, std::min(heartbeat_interval_ms, scan_interval_ms));
    impl_->ttl_seconds = std::max(1, ttl_seconds);
}

NodeDiscovery::~NodeDiscovery() { stop(); }

void NodeDiscovery::set_callbacks(DiscoveredFn on_discovered, LostFn on_lost) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->on_discovered = std::move(on_discovered);
    impl_->on_lost = std::move(on_lost);
}

void NodeDiscovery::start() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->started) return;
    impl_->started = true;
    try {
        impl_->backend->register_node(impl_->node_id, impl_->address,
                                      impl_->ttl_seconds);
    } catch (const std::exception& e) {
        SHIELD_LOG_WARNING(
            shield::log::get_logger("cluster"),
            std::string("discovery register failed: ") + e.what());
    }
    impl_->thread = std::jthread([impl = impl_.get()](std::stop_token stop) {
        while (!stop.stop_requested()) {
            try {
                impl->backend->heartbeat(impl->node_id, impl->ttl_seconds);
            } catch (const std::exception& e) {
                SHIELD_LOG_WARNING(
                    shield::log::get_logger("cluster"),
                    std::string("discovery heartbeat failed: ") + e.what());
            }
            // A failed scan must NOT reach diff(): an empty result would
            // report every known peer lost. Skip the diff entirely and let
            // the next tick retry.
            std::vector<DiscoveredNode> nodes;
            bool scanned = false;
            try {
                nodes = impl->backend->scan(impl->node_id);
                scanned = true;
            } catch (const std::exception& e) {
                SHIELD_LOG_WARNING(
                    shield::log::get_logger("cluster"),
                    std::string("discovery scan failed: ") + e.what());
            }
            if (scanned) impl->diff(nodes);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(impl->interval_ms));
        }
    });
}

void NodeDiscovery::stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (!impl_->started) return;
        impl_->started = false;
    }
    if (impl_->thread.joinable()) impl_->thread.request_stop();
    if (impl_->thread.joinable()) impl_->thread.join();
    try {
        impl_->backend->unregister_node(impl_->node_id);
    } catch (const std::exception& e) {
        SHIELD_LOG_WARNING(
            shield::log::get_logger("cluster"),
            std::string("discovery unregister failed: ") + e.what());
    }
}

void NodeDiscovery::Impl::diff(const std::vector<DiscoveredNode>& nodes) {
    std::lock_guard<std::mutex> lock(mu);
    std::map<std::string, std::string> current;
    for (const auto& n : nodes) current[n.node_id] = n.address;

    // Appearances: nodes in the scan result that were not known before.
    for (const auto& n : nodes) {
        if (known.find(n.node_id) == known.end()) {
            known[n.node_id] = n.address;
            if (on_discovered) on_discovered(n);
        }
    }
    // Disappearances: known nodes missing from the scan result. The
    // backend already filters the self id.
    std::vector<std::string> lost;
    for (const auto& [id, addr] : known) {
        if (current.find(id) == current.end()) lost.push_back(id);
    }
    for (const auto& id : lost) {
        known.erase(id);
        if (on_lost) on_lost(id);
    }
}

std::unique_ptr<NodeDiscoveryBackend> make_redis_discovery_backend(
    const RedisDiscoveryConfig& cfg) {
    return std::make_unique<RedisDiscoveryBackend>(cfg);
}

std::unique_ptr<NodeDiscovery> make_node_discovery(
    const DiscoveryConfig& cfg, const std::string& node_id,
    const std::string& listen_address) {
    if (cfg.type == "redis") {
        RedisDiscoveryConfig redis;
        redis.host = cfg.host;
        redis.port = cfg.port;
        redis.password = cfg.password;
        redis.db = cfg.db;
        redis.prefix = cfg.prefix;
        redis.ttl_seconds = cfg.ttl_seconds;
        return std::make_unique<NodeDiscovery>(
            node_id, listen_address, make_redis_discovery_backend(redis),
            cfg.heartbeat_interval_ms, cfg.scan_interval_ms, cfg.ttl_seconds);
    }
    if (!cfg.type.empty()) {  // GCOVR_EXCL_BR_LINE (empty/known/unknown all
                              // driven by tests)
        SHIELD_LOG_WARNING(shield::log::get_logger("cluster"),
                           "Unknown cluster.discovery.type: " + cfg.type +
                               " (discovery disabled)");
    }
    return nullptr;
}

}  // namespace shield::cluster
