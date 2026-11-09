// [SHIELD_PLUGIN] queue.redis — internal cross-TU declarations
// for the plugin split.
//
// Not a public contract: the exported ABI stays in
// shield/plugin/queue.h. The connection structs (global scope in
// the original for lambda-to-fnptr conversion), the consumer-group
// constants, the Lua subscription and the instance struct move
// here verbatim so the shield_queue_redis*.cpp translation units
// share one layout; the file-local helpers shared across the split
// are defined flat (external linkage) in their own TU, one
// declaration each.
#pragma once

#include <sw/redis++/redis++.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/queue.h"
#include "shield/plugin/redis.h"

// ---------------------------------------------------------------------------
// Opaque connection handle (global scope for lambda-to-fnptr conversion)
// ---------------------------------------------------------------------------

struct SubEntry {
    shield_queue_on_message cb;
    void* user_data;
};

// A consumer worker thread plus its own stop signal. The stop flag is the
// only way to retire a worker before the whole connection goes down, and it
// is checked between XREADGROUP BLOCK rounds so a stop is observed within
// ~kBlockMs.
struct shield_queue_worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> stop;
};

struct shield_queue_conn {
    std::shared_ptr<sw::redis::Redis> redis;
    std::unordered_map<std::string, SubEntry> subs;
    std::mutex subs_mu;
    std::unordered_map<std::string, shield_queue_worker> workers;
    // Guards `workers` only. `subs_mu` guards the callback map; the two are
    // never held together (subscribe/unsubscribe take them sequentially).
    std::mutex workers_mu;
    std::atomic<bool> running{false};

    explicit shield_queue_conn(std::shared_ptr<sw::redis::Redis> r)
        : redis(std::move(r)) {}
};

constexpr const char* kGroupName = "shield";
constexpr int kReadCount = 10;
constexpr int kBlockMs = 2000;

struct lua_subscription {
    std::shared_ptr<sw::redis::Redis> redis;  // fallback: own connection
    // Preferred (Phase 2 dual-path): redis.driver vtable + handle. When set,
    // redis stays null and all commands run through the driver's shared pool
    // via the raw command() escape hatch.
    const shield_redis_v2* redis_driver = nullptr;
    void* redis_handle = nullptr;
    shd::protected_function callback;
    std::thread worker;
    std::atomic<bool> running{false};
    std::string channel;
    std::string consumer_name;
    // Owning service actor. The consumer thread never touches Lua; messages
    // are posted to this service via host_api.lua_post_to_service and the
    // Lua callback runs on the service actor's dispatch context.
    std::string service_id;

    lua_subscription(std::shared_ptr<sw::redis::Redis> r,
                     shd::protected_function cb, const std::string& ch,
                     std::string owner_service_id)
        : redis(std::move(r)),
          callback(std::move(cb)),
          channel(ch),
          consumer_name("shield-lua-" + ch),
          service_id(std::move(owner_service_id)) {}

    lua_subscription(const shield_redis_v2* drv, void* handle,
                     shd::protected_function cb, const std::string& ch,
                     std::string owner_service_id)
        : redis_driver(drv),
          redis_handle(handle),
          callback(std::move(cb)),
          channel(ch),
          consumer_name("shield-lua-" + ch),
          service_id(std::move(owner_service_id)) {}
};

struct queue_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* ctx = nullptr;
    std::string host = "127.0.0.1";
    int port = 6379;
    int db = 0;
    std::string password;
    int connect_timeout_ms = 5000;
    int command_timeout_ms = 2000;
    const shield_redis_v2* redis_driver = nullptr;
    void* redis_handle = nullptr;

    std::mutex subs_mu;
    std::unordered_map<std::string, std::shared_ptr<lua_subscription>> subs;

    std::shared_ptr<sw::redis::Redis> make_redis(std::string* err) const {
        try {
            sw::redis::ConnectionOptions opts;
            opts.host = host;
            opts.port = port;
            if (!password.empty()) opts.password = password;
            opts.db = db;
            opts.connect_timeout =
                std::chrono::milliseconds(connect_timeout_ms);
            opts.socket_timeout = std::chrono::milliseconds(command_timeout_ms);
            return std::make_shared<sw::redis::Redis>(opts);
        } catch (const std::exception& e) {
            if (err) *err = e.what();
            return nullptr;
        }
    }

    void stop_all() {
        std::unordered_map<std::string, std::shared_ptr<lua_subscription>>
            snapshot;
        {
            std::lock_guard<std::mutex> lk(subs_mu);
            snapshot.swap(subs);
        }
        for (auto& [ch, s] : snapshot) {
            s->running.store(false);
            if (s->worker.joinable()) s->worker.join();
        }
    }

    void stop_one(const std::string& channel) {
        std::shared_ptr<lua_subscription> s;
        {
            std::lock_guard<std::mutex> lk(subs_mu);
            auto it = subs.find(channel);
            if (it == subs.end()) return;
            s = it->second;
            subs.erase(it);
        }
        s->running.store(false);
        if (s->worker.joinable()) s->worker.join();
    }
};

// Defined in shield_queue_redis_exec.cpp: idempotent XGROUP CREATE
// on the subscription's own redis++ connection (both the C vtable
// and the Lua proxy subscribe paths).
void ensure_group(sw::redis::Redis& redis, const std::string& stream,
                  const std::string& group);

// Defined in shield_queue_redis_exec.cpp: the legacy C-ABI surface,
// served through get_interface() in queue_create.
const shield_queue_v1& queue_vtable();

// Defined in shield_queue_redis.cpp: the process-wide registry
// (create registers, shutdown unregisters, the Lua facade resolves).
void register_instance(queue_instance* inst);
void unregister_instance(const std::string& id);
queue_instance* find_instance(const std::string& id);

// Defined in shield_queue_redis_lua.cpp: register_lua implementation,
// installed by queue_create on the shell.
int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err);
