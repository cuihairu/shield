// [SHIELD_PLUGIN] queue.redis — C-ABI executor: the
// shield.queue.v1 surface (connect/publish/subscribe with one
// consumer thread per channel over XREADGROUP BLOCK rounds) over
// the subscription's own redis++ connection.
// Split out of shield_queue_redis.cpp.
#include <sw/redis++/redis++.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/pool_stats.h"
#include "shield/plugin/queue.h"
#include "shield/plugin/redis.h"
#include "shield_lua_plugin_binding.hpp"
#include "shield_queue_redis_impl.hpp"

namespace {
char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}
}  // namespace

// Ensure the consumer group exists. Idempotent — ignores "BUSYGROUP" error.
void ensure_group(sw::redis::Redis& redis, const std::string& stream,
                  const std::string& group) {
    try {
        redis.command("XGROUP", "CREATE", stream, group, "0", "MKSTREAM");
    } catch (const sw::redis::Error& e) {
        // BUSYGROUP = group already exists; ignore.
        if (std::string(e.what()).find("BUSYGROUP") == std::string::npos) {
            throw;
        }
    }
}

namespace {
// Consumer thread: XREADGROUP loop for one channel/stream. `stop` is the
// worker's own retirement signal, checked alongside `running` between BLOCK
// rounds. It exists so a single worker can be retired (re-subscribe /
// unsubscribe) without tearing down the whole connection.
void consumer_loop(shield_queue_conn* c, const std::string& channel,
                   const std::string& consumer_name,
                   std::shared_ptr<std::atomic<bool>> stop) {
    while (c->running.load() && !stop->load()) {
        try {
            // XREADGROUP GROUP <group> <consumer> COUNT <n> BLOCK <ms> STREAMS
            // <stream> >
            auto result = c->redis->command(
                "XREADGROUP", "GROUP", kGroupName, consumer_name, "COUNT",
                std::to_string(kReadCount), "BLOCK", std::to_string(kBlockMs),
                "STREAMS", channel, ">");
            // result is an array of streams; each stream is [stream_name, [[id,
            // fields], ...]]
            if (!result || result->type == REDIS_REPLY_NIL) continue;
            if (result->type != REDIS_REPLY_ARRAY) continue;
            for (size_t si = 0; si < result->elements; ++si) {
                auto* stream_reply = result->element[si];
                if (!stream_reply || stream_reply->elements < 2) continue;
                auto* entries = stream_reply->element[1];
                if (!entries || entries->type != REDIS_REPLY_ARRAY) continue;
                for (size_t ei = 0; ei < entries->elements; ++ei) {
                    auto* entry = entries->element[ei];
                    if (!entry || entry->elements < 2) continue;
                    // entry->element[0] = message id, entry->element[1] =
                    // fields array
                    auto* id_reply = entry->element[0];
                    auto* fields = entry->element[1];
                    if (!id_reply || !fields) continue;
                    std::string msg_id(id_reply->str, id_reply->len);
                    // Extract "payload" field from the fields array.
                    // Fields are [key, value, key, value, ...]
                    std::string payload;
                    for (size_t fi = 0; fi + 1 < fields->elements; fi += 2) {
                        auto* key = fields->element[fi];
                        auto* val = fields->element[fi + 1];
                        if (key && val && key->str &&
                            std::string(key->str, key->len) == "payload") {
                            payload.assign(val->str, val->len);
                            break;
                        }
                    }
                    // Dispatch to callback
                    SubEntry entry_cb;
                    {
                        std::lock_guard<std::mutex> lock(c->subs_mu);
                        auto it = c->subs.find(channel);
                        if (it == c->subs.end()) continue;
                        entry_cb = it->second;
                    }
                    if (entry_cb.cb) {
                        entry_cb.cb(channel.c_str(), payload.c_str(),
                                    static_cast<int>(payload.size()),
                                    entry_cb.user_data);
                    }
                    // XACK after successful dispatch
                    try {
                        c->redis->command("XACK", channel, kGroupName, msg_id);
                    } catch (...) {
                    }
                }
            }
        } catch (const sw::redis::Error&) {
            // Transient errors — back off and retry.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// v1 queue vtable
// ---------------------------------------------------------------------------

const shield_queue_v1& queue_vtable() {
    static const shield_queue_v1 v = {
        sizeof(shield_queue_v1),
        SHIELD_QUEUE_INTERFACE,
        "redis",
        "1.0.0",
        // connect
        [](const shield_queue_config* cfg, char* err_buf,
           int err_buf_size) -> shield_queue_conn* {
            if (!cfg) return nullptr;
            try {
                sw::redis::ConnectionOptions opts;
                opts.host = cfg->host && cfg->host[0] ? cfg->host : "localhost";
                opts.port = cfg->port > 0 ? cfg->port : 6379;
                if (cfg->password && cfg->password[0])
                    opts.password = cfg->password;
                opts.db = cfg->db > 0 ? cfg->db : 0;
                opts.connect_timeout = std::chrono::milliseconds(
                    cfg->connect_timeout_ms > 0 ? cfg->connect_timeout_ms
                                                : 5000);
                opts.socket_timeout = std::chrono::milliseconds(
                    cfg->command_timeout_ms > 0 ? cfg->command_timeout_ms
                                                : 2000);
                auto redis = std::make_shared<sw::redis::Redis>(opts);
                auto* conn = new shield_queue_conn(redis);
                conn->running.store(true);
                return conn;
            } catch (const std::exception& e) {
                if (err_buf && err_buf_size > 0)
                    std::snprintf(err_buf, err_buf_size, "%s", e.what());
                return nullptr;
            }
        },
        // disconnect
        [](shield_queue_conn* c) {
            if (!c) return;
            c->running.store(false);
            {
                std::lock_guard<std::mutex> lock(c->workers_mu);
                for (auto& [ch, w] : c->workers) {
                    if (w.stop) w.stop->store(true);
                }
                for (auto& [ch, w] : c->workers) {
                    if (w.thread.joinable()) w.thread.join();
                }
                c->workers.clear();
            }
            delete c;
        },
        // publish — XADD channel * payload <data>
        [](shield_queue_conn* c, const char* channel, const char* data,
           int data_len) -> int {
            if (!c || !c->redis || !channel || !data) return -1;
            try {
                std::string msg(data,
                                data_len > 0 ? data_len : std::strlen(data));
                c->redis->command("XADD", channel, "*", "payload", msg);
                return 0;
            } catch (...) {
                return -1;
            }
        },
        // subscribe — XGROUP CREATE + spawn consumer thread
        [](shield_queue_conn* c, const char* channel,
           shield_queue_on_message callback, void* user_data) -> int {
            if (!c || !c->redis || !channel) return -1;
            try {
                std::string ch(channel);
                ensure_group(*c->redis, ch, kGroupName);
                {
                    std::lock_guard<std::mutex> lock(c->subs_mu);
                    c->subs[ch] = {callback, user_data};
                }
                // Consumer name: shield-<pointer>-<channel>
                std::string consumer = "shield-" + ch;
                // Retire any existing worker for this channel. A re-subscribe
                // must not leave a joinable std::thread in the map: erasing it
                // without a join destroys a joinable thread (std::terminate),
                // and two XREADGROUP loops on one consumer group would both
                // deliver every message. Set the worker's stop flag and join
                // it (observed within ~kBlockMs) before replacing.
                shield_queue_worker old;
                {
                    std::lock_guard<std::mutex> lock(c->workers_mu);
                    auto it = c->workers.find(ch);
                    if (it != c->workers.end()) {
                        old = std::move(it->second);
                        c->workers.erase(it);
                    }
                }
                if (old.thread.joinable()) {
                    if (old.stop) old.stop->store(true);
                    // subscribe never runs on the consumer thread itself, so
                    // a self-join is impossible here.
                    old.thread.join();
                }
                shield_queue_worker w;
                w.stop = std::make_shared<std::atomic<bool>>(false);
                w.thread = std::thread(consumer_loop, c, ch, consumer, w.stop);
                {
                    std::lock_guard<std::mutex> lock(c->workers_mu);
                    c->workers[ch] = std::move(w);
                }
                return 0;
            } catch (...) {
                return -1;
            }
        },
        // unsubscribe — remove callback and retire the worker (join before
        // erase so the map never holds a joinable, destroyable thread)
        [](shield_queue_conn* c, const char* channel) -> int {
            if (!c || !channel) return -1;
            std::string ch(channel);
            {
                std::lock_guard<std::mutex> lock(c->subs_mu);
                c->subs.erase(ch);
            }
            shield_queue_worker old;
            {
                std::lock_guard<std::mutex> lock(c->workers_mu);
                auto it = c->workers.find(ch);
                if (it != c->workers.end()) {
                    old = std::move(it->second);
                    c->workers.erase(it);
                }
            }
            if (old.thread.joinable()) {
                if (old.stop) old.stop->store(true);
                old.thread.join();
            }
            return 0;
        },
    };
    return v;
}
