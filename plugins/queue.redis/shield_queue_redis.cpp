// [SHIELD_PLUGIN] queue.redis — Redis Streams provider for shield.queue.v1.
//
// v1 ABI. Uses Redis Streams (XADD / XREADGROUP / XACK) for persistent,
// consumer-group-based message delivery with at-least-once guarantees.
//
// publish() calls XADD to append a message to a Stream (keyed by channel).
// subscribe() creates a consumer group (XGROUP CREATE) and spawns a
// consumer thread that loops XREADGROUP. Messages are dispatched to the
// registered callback; on callback return, XACK is called automatically.
// If the callback throws, the message stays pending (at-least-once).
//
// Lua autonomy: shield.queue.redis(binding) callable namespace.

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/queue.h"
#include "shield/plugin/redis.h"

#include <sw/redis++/redis++.h>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Opaque connection handle (global scope for lambda-to-fnptr conversion)
// ---------------------------------------------------------------------------

struct SubEntry {
    shield_queue_on_message cb;
    void* user_data;
};

struct shield_queue_conn {
    std::shared_ptr<sw::redis::Redis> redis;
    std::unordered_map<std::string, SubEntry> subs;
    std::mutex subs_mu;
    std::unordered_map<std::string, std::thread> workers;
    std::atomic<bool> running{false};

    explicit shield_queue_conn(std::shared_ptr<sw::redis::Redis> r)
        : redis(std::move(r)) {}
};

namespace {

constexpr const char* kGroupName = "shield";
constexpr int kReadCount = 10;
constexpr int kBlockMs = 2000;

char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

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

// Consumer thread: XREADGROUP loop for one channel/stream.
void consumer_loop(shield_queue_conn* c, const std::string& channel,
                   const std::string& consumer_name) {
    while (c->running.load()) {
        try {
            // XREADGROUP GROUP <group> <consumer> COUNT <n> BLOCK <ms> STREAMS <stream> >
            auto result = c->redis->command(
                "XREADGROUP", "GROUP", kGroupName, consumer_name,
                "COUNT", std::to_string(kReadCount),
                "BLOCK", std::to_string(kBlockMs),
                "STREAMS", channel, ">");
            // result is an array of streams; each stream is [stream_name, [[id, fields], ...]]
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
                    // entry->element[0] = message id, entry->element[1] = fields array
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
                    } catch (...) {}
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
        [](const shield_queue_config* cfg,
           char* err_buf, int err_buf_size) -> shield_queue_conn* {
            if (!cfg) return nullptr;
            try {
                sw::redis::ConnectionOptions opts;
                opts.host = cfg->host && cfg->host[0] ? cfg->host : "localhost";
                opts.port = cfg->port > 0 ? cfg->port : 6379;
                if (cfg->password && cfg->password[0])
                    opts.password = cfg->password;
                opts.db = cfg->db > 0 ? cfg->db : 0;
                opts.connect_timeout = std::chrono::milliseconds(
                    cfg->connect_timeout_ms > 0 ? cfg->connect_timeout_ms : 5000);
                opts.socket_timeout = std::chrono::milliseconds(
                    cfg->command_timeout_ms > 0 ? cfg->command_timeout_ms : 2000);
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
            for (auto& [ch, t] : c->workers) {
                if (t.joinable()) t.join();
            }
            delete c;
        },
        // publish — XADD channel * payload <data>
        [](shield_queue_conn* c, const char* channel,
           const char* data, int data_len) -> int {
            if (!c || !c->redis || !channel || !data) return -1;
            try {
                std::string msg(data, data_len > 0 ? data_len : std::strlen(data));
                c->redis->command("XADD", channel, "*", "payload", msg);
                return 0;
            } catch (...) { return -1; }
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
                // Stop existing worker for this channel if any
                {
                    auto it = c->workers.find(ch);
                    if (it != c->workers.end()) {
                        // Worker will exit on next BLOCK timeout
                        c->workers.erase(it);
                    }
                }
                c->workers[ch] = std::thread(consumer_loop, c, ch, consumer);
                return 0;
            } catch (...) { return -1; }
        },
        // unsubscribe — remove callback; worker exits on next loop iteration
        [](shield_queue_conn* c, const char* channel) -> int {
            if (!c || !channel) return -1;
            std::string ch(channel);
            {
                std::lock_guard<std::mutex> lock(c->subs_mu);
                c->subs.erase(ch);
            }
            auto it = c->workers.find(ch);
            if (it != c->workers.end()) {
                if (it->second.joinable()) it->second.join();
                c->workers.erase(it);
            }
            return 0;
        },
    };
    return v;
}

// ---------------------------------------------------------------------------
// Lua subscription (Streams-based)
// ---------------------------------------------------------------------------
struct lua_subscription {
    std::shared_ptr<sw::redis::Redis> redis;
    sol::protected_function callback;
    std::thread worker;
    std::atomic<bool> running{false};
    std::string channel;
    std::string consumer_name;

    lua_subscription(std::shared_ptr<sw::redis::Redis> r,
                     sol::protected_function cb, const std::string& ch)
        : redis(std::move(r)),
          callback(std::move(cb)),
          channel(ch),
          consumer_name("shield-lua-" + ch) {}
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
    const shield_redis_v1* redis_driver = nullptr;
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
            opts.connect_timeout = std::chrono::milliseconds(connect_timeout_ms);
            opts.socket_timeout = std::chrono::milliseconds(command_timeout_ms);
            return std::make_shared<sw::redis::Redis>(opts);
        } catch (const std::exception& e) {
            if (err) *err = e.what();
            return nullptr;
        }
    }

    void stop_all() {
        std::unordered_map<std::string, std::shared_ptr<lua_subscription>> snapshot;
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

// Process-wide registry
std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, queue_instance*>& instances_map() {
    static std::map<std::string, queue_instance*> m;
    return m;
}
void register_instance(queue_instance* inst) {
    std::lock_guard lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}
void unregister_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    instances_map().erase(id);
}
queue_instance* find_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

void parse_instance_config(queue_instance* inst, const char* config_json) {
    if (!config_json || !config_json[0]) return;
    try {
        auto j = nlohmann::json::parse(config_json);
        if (j.contains("host") && j["host"].is_string())
            inst->host = j["host"].get<std::string>();
        if (j.contains("port") && j["port"].is_number_integer())
            inst->port = j["port"].get<int>();
        if (j.contains("db") && j["db"].is_number_integer())
            inst->db = j["db"].get<int>();
        if (j.contains("password") && j["password"].is_string())
            inst->password = j["password"].get<std::string>();
        if (j.contains("connect_timeout_ms") && j["connect_timeout_ms"].is_number_integer())
            inst->connect_timeout_ms = j["connect_timeout_ms"].get<int>();
        if (j.contains("command_timeout_ms") && j["command_timeout_ms"].is_number_integer())
            inst->command_timeout_ms = j["command_timeout_ms"].get<int>();
    } catch (...) {}
}

sol::table make_error_table(sol::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}

bool lua_to_message(const sol::object& v, std::string* out, std::string* err) {
    if (!v.valid() || v == sol::nil) {
        if (err) *err = "message is nil";
        return false;
    }
    if (v.is<std::string>()) { *out = v.as<std::string>(); return true; }
    if (v.is<bool>() || v.is<double>() || v.is<lua_Integer>()) {
        try {
            nlohmann::json j;
            if (v.is<bool>()) j = v.as<bool>();
            else if (v.is<lua_Integer>()) j = v.as<lua_Integer>();
            else j = v.as<double>();
            *out = j.dump();
            return true;
        } catch (const std::exception& e) {
            if (err) *err = e.what();
            return false;
        }
    }
    if (v.is<sol::table>()) {
        try {
            std::function<nlohmann::json(sol::object)> encode;
            encode = [&encode](sol::object o) -> nlohmann::json {
                if (!o.valid() || o == sol::nil) return nullptr;
                if (o.is<bool>())    return o.as<bool>();
                if (o.is<lua_Integer>()) return o.as<lua_Integer>();
                if (o.is<double>())  return o.as<double>();
                if (o.is<std::string>()) return o.as<std::string>();
                if (o.is<sol::table>()) {
                    sol::table t = o.as<sol::table>();
                    bool sequence = true;
                    int max_index = 0;
                    for (auto& kv : t) {
                        if (kv.first.get_type() != sol::type::number) { sequence = false; break; }
                        lua_Integer idx = kv.first.as<lua_Integer>();
                        if (idx < 1) { sequence = false; break; }
                        if (idx > max_index) max_index = static_cast<int>(idx);
                    }
                    if (sequence && max_index > 0) {
                        nlohmann::json arr = nlohmann::json::array();
                        std::vector<nlohmann::json> tmp(static_cast<size_t>(max_index), nullptr);
                        for (auto& kv : t) {
                            int idx = static_cast<int>(kv.first.as<lua_Integer>());
                            tmp[static_cast<size_t>(idx - 1)] = encode(kv.second);
                        }
                        for (auto& e : tmp) arr.push_back(std::move(e));
                        return arr;
                    }
                    nlohmann::json obj = nlohmann::json::object();
                    for (auto& kv : t) {
                        std::string key;
                        if (kv.first.get_type() == sol::type::string)
                            key = kv.first.as<std::string>();
                        else if (kv.first.get_type() == sol::type::number)
                            key = std::to_string(kv.first.as<lua_Integer>());
                        else continue;
                        obj[key] = encode(kv.second);
                    }
                    return obj;
                }
                return nullptr;
            };
            *out = encode(v).dump();
            return true;
        } catch (const std::exception& e) {
            if (err) *err = e.what();
            return false;
        }
    }
    if (err) *err = "unsupported message type";
    return false;
}

// Lua proxy — Streams-based publish/subscribe
sol::table make_instance_proxy(sol::state_view lua, queue_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function("publish",
        [inst](sol::this_state s, std::string channel,
               sol::object message) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string msg, msg_err;
            if (!lua_to_message(message, &msg, &msg_err)) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "invalid_message", msg_err));
                return results;
            }
            std::string conn_err;
            auto redis = inst->make_redis(&conn_err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", conn_err));
                return results;
            }
            try {
                redis->command("XADD", channel, "*", "payload", msg);
                results.push_back(sol::make_object(lua, true));
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "redis_command_failed", e.what()));
            }
            return results;
        });

    proxy.set_function("subscribe",
        [inst](sol::this_state s, std::string channel,
               sol::protected_function callback) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            {
                std::lock_guard<std::mutex> lk(inst->subs_mu);
                if (inst->subs.find(channel) != inst->subs.end()) {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(lua, "already_subscribed",
                        "channel already has a subscriber; unsubscribe first"));
                    return results;
                }
            }
            std::string conn_err;
            auto redis = inst->make_redis(&conn_err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", conn_err));
                return results;
            }
            // Ensure consumer group
            try {
                ensure_group(*redis, channel, kGroupName);
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "redis_command_failed", e.what()));
                return results;
            }
            auto sub = std::make_shared<lua_subscription>(redis, callback, channel);
            sub->running.store(true);
            auto sub_ptr = sub;
            sub->worker = std::thread([sub_ptr]() {
                while (sub_ptr->running.load()) {
                    try {
                        auto result = sub_ptr->redis->command(
                            "XREADGROUP", "GROUP", kGroupName, sub_ptr->consumer_name,
                            "COUNT", std::to_string(kReadCount),
                            "BLOCK", std::to_string(kBlockMs),
                            "STREAMS", sub_ptr->channel, ">");
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
                                auto* id_reply = entry->element[0];
                                auto* fields = entry->element[1];
                                if (!id_reply || !fields) continue;
                                std::string msg_id(id_reply->str, id_reply->len);
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
                                try {
                                    sol::protected_function_result r =
                                        sub_ptr->callback(sub_ptr->channel, payload);
                                    (void)r.valid();
                                } catch (...) {}
                                try {
                                    sub_ptr->redis->command("XACK", sub_ptr->channel,
                                                            kGroupName, msg_id);
                                } catch (...) {}
                            }
                        }
                    } catch (const sw::redis::Error&) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    } catch (...) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }
            });
            {
                std::lock_guard<std::mutex> lk(inst->subs_mu);
                inst->subs[channel] = std::move(sub);
            }
            results.push_back(sol::make_object(lua, true));
            return results;
        });

    proxy.set_function("unsubscribe",
        [inst](sol::this_state s, std::string channel) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            inst->stop_one(channel);
            results.push_back(sol::make_object(lua, true));
            return results;
        });

    return proxy;
}

int register_lua_impl(shield_plugin_instance_v1* self,
                      struct lua_State* L,
                      shield_error_v1* err) {
    if (!L) {
        if (err) { err->code = "plugin.lua_register.failed"; err->message = "queue.redis: lua_State is null"; }
        return 1;
    }
    auto* current = reinterpret_cast<queue_instance*>(self);
    if (!current || !current->host_api || !current->host_api->binding_instance_id) {
        if (err) { err->code = "plugin.lua_register.failed"; err->message = "queue.redis: host binding resolver is null"; }
        return 1;
    }
    sol::state_view lua(L);
    auto shield = lua["shield"].get_or_create<sol::table>();
    auto queue = shield["queue"].get_or_create<sol::table>();
    sol::object existing = queue["redis"];
    if (!existing.is<sol::table>()) {
        auto ns = lua.create_table();
        auto mt = lua.create_table();
        const shield_host_api_v1* host_api = current->host_api;
        shield_plugin_context_v1* ctx = current->ctx;
        mt.set_function("__call",
            [host_api, ctx](sol::this_state s, sol::table, std::string binding) -> sol::object {
                sol::state_view lua(s);
                const char* instance_id = host_api->binding_instance_id(ctx, binding.c_str());
                if (!instance_id) return sol::nil;
                auto* inst = find_instance(instance_id);
                if (!inst) return sol::nil;
                return sol::make_object(lua, make_instance_proxy(lua, inst));
            });
        ns[sol::metatable_key] = mt;
        queue["redis"] = ns;
    }
    return 0;
}

int queue_create(const shield_plugin_create_args_v1* args,
                 shield_plugin_instance_v1** out,
                 shield_error_v1* err) {
    (void)err;
    auto* inst = new queue_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->host_api = args->host_api;
    inst->ctx = args->ctx;
    parse_instance_config(inst, args->config_json);
    register_instance(inst);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1*, const char* iface, shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_QUEUE_INTERFACE) == 0) return &queue_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1* self, shield_error_v1*) -> int {
        auto* qi = reinterpret_cast<queue_instance*>(self);
        if (qi->host_api && qi->host_api->dependency) {
            auto* drv = static_cast<const shield_redis_v1*>(
                qi->host_api->dependency(qi->ctx, "redis", SHIELD_REDIS_V1));
            if (drv && drv->connect) {
                char err_buf[256] = {};
                void* handle = drv->connect(nullptr, err_buf, sizeof(err_buf));
                if (handle) { qi->redis_driver = drv; qi->redis_handle = handle; }
            }
        }
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        auto* qi = reinterpret_cast<queue_instance*>(self);
        qi->stop_all();
        if (qi->redis_driver && qi->redis_handle) {
            qi->redis_driver->disconnect(qi->redis_handle);
            qi->redis_handle = nullptr;
            qi->redis_driver = nullptr;
        }
        unregister_instance(qi->instance_id);
        delete qi;
    };
    inst->shell.register_lua = &register_lua_impl;
    *out = &inst->shell;
    return 0;
}

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT
const struct shield_plugin_abi_v1* shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "queue.redis",
        "1.0.0",
        queue_create,
    };
    return &abi;
}
