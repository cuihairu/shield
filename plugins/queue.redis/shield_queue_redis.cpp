// [SHIELD_PLUGIN] queue.redis — Redis pub/sub provider for shield.queue.v1.
//
// v1 ABI. The v1 queue interface is intentionally narrow: publish/subscribe/
// unsubscribe only. Legacy shield_redis exposed consumer groups and acks;
// those are NOT in v1 (see docs/plugin-system.md: queue.v1 = pub/sub), and
// the dead code has been removed.
//
// subscribe() semantics: the v1 API expects messages to start flowing to the
// callback after subscribe returns. We spawn a dedicated subscriber thread
// per shield_queue_conn that owns a sw::redis::Subscriber. Unsubscribe stops
// the channel; disconnect stops the thread and joins.
//
// Lua autonomy: the plugin owns its own Lua surface — shield.queue.redis.
// register_lua() installs a callable namespace; __call resolves an instance
// by id and returns a proxy. Per-instance state lives in queue_instance,
// registered process-wide so the Lua proxy can find it. The proxy publishes
// by opening a pooled sw::redis::Redis command connection, and subscribes by
// spinning up a dedicated subscriber connection + consumer thread.

#include "shield/plugin/abi.h"
#include "shield/plugin/queue.h"

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

namespace {

char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

struct SubEntry {
    shield_queue_on_message cb;
    void* user_data;
};

// shield_queue_conn is opaque in queue.h; concrete layout lives here.
// A single Subscriber thread consumes messages and dispatches to the
// per-channel callback registered via subscribe().
struct shield_queue_conn {
    std::shared_ptr<sw::redis::Redis> redis;
    sw::redis::Subscriber subscriber;
    std::unordered_map<std::string, SubEntry> subs;
    std::mutex subs_mu;
    std::thread worker;
    std::atomic<bool> running{false};

    explicit shield_queue_conn(std::shared_ptr<sw::redis::Redis> r)
        : redis(std::move(r)), subscriber(redis->subscriber()) {}
};

void dispatch_message(shield_queue_conn* c, const std::string& channel,
                      const std::string& msg) {
    SubEntry entry;
    {
        std::lock_guard<std::mutex> lock(c->subs_mu);
        auto it = c->subs.find(channel);
        if (it == c->subs.end()) return;
        entry = it->second;
    }
    if (entry.cb) {
        entry.cb(channel.c_str(), msg.c_str(),
                 static_cast<int>(msg.size()), entry.user_data);
    }
}

void subscriber_loop(shield_queue_conn* c) {
    using namespace std::chrono;
    while (c->running.load()) {
        try {
            c->subscriber.consume();
        } catch (const sw::redis::TimeoutError&) {
            // Normal: no messages within the socket timeout; loop.
        } catch (const sw::redis::ClosedError&) {
            break;
        } catch (const std::exception&) {
            // Transient errors — back off briefly and retry.
            std::this_thread::sleep_for(milliseconds(100));
        }
    }
}

// ---------------------------------------------------------------------------
// v1 queue vtable
// ---------------------------------------------------------------------------
const shield_queue_v1& queue_vtable() {
    static const shield_queue_v1 v = {
        sizeof(shield_queue_v1),
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
                    cfg->command_timeout_ms > 0 ? cfg->command_timeout_ms : 1000);

                auto redis = std::make_shared<sw::redis::Redis>(opts);

                auto* conn = new shield_queue_conn(redis);
                conn->subscriber.on_message(
                    [conn](const std::string& channel, const std::string& msg) {
                        dispatch_message(conn, channel, msg);
                    });
                conn->running.store(true);
                conn->worker = std::thread(subscriber_loop, conn);
                return conn;
            } catch (const std::exception& e) {
                if (err_buf && err_buf_size > 0) {
                    std::snprintf(err_buf, err_buf_size, "%s", e.what());
                }
                return nullptr;
            }
        },
        // disconnect
        [](shield_queue_conn* c) {
            if (!c) return;
            c->running.store(false);
            // Force the consumer out of consume(): closing the subscriber is
            // the cleanest cross-platform way to interrupt its blocking read.
            try { c->subscriber.close(); } catch (...) {}
            if (c->worker.joinable()) c->worker.join();
            delete c;
        },
        // publish
        [](shield_queue_conn* c, const char* channel,
           const char* data, int data_len) -> int {
            if (!c || !c->redis || !channel || !data) return -1;
            try {
                std::string msg(data, data_len > 0 ? data_len : std::strlen(data));
                return static_cast<int>(
                    c->redis->publish(std::string(channel), msg));
            } catch (...) { return -1; }
        },
        // subscribe
        [](shield_queue_conn* c, const char* channel,
           shield_queue_on_message callback, void* user_data) -> int {
            if (!c || !channel) return -1;
            try {
                {
                    std::lock_guard<std::mutex> lock(c->subs_mu);
                    c->subs[std::string(channel)] = {callback, user_data};
                }
                c->subscriber.subscribe(std::string(channel));
                return 0;
            } catch (...) { return -1; }
        },
        // unsubscribe
        [](shield_queue_conn* c, const char* channel) -> int {
            if (!c || !channel) return -1;
            try {
                c->subscriber.unsubscribe(std::string(channel));
                {
                    std::lock_guard<std::mutex> lock(c->subs_mu);
                    c->subs.erase(std::string(channel));
                }
                return 0;
            } catch (...) { return -1; }
        },
    };
    return v;
}

// ---------------------------------------------------------------------------
// v1 ABI entry. The instance carries its own config (parsed from
// config_json) and registers itself in a process-wide map so the Lua
// callable namespace `shield.queue.redis(instance_id)` can resolve it.
// ---------------------------------------------------------------------------

// A live Lua-driven subscription. The consumer thread owns the Subscriber
// and pumps messages; the callback is invoked from that thread (documented
// cross-thread limitation — see make_instance_proxy).
struct lua_subscription {
    std::shared_ptr<sw::redis::Redis> redis;  // keeps the connection alive
    sw::redis::Subscriber subscriber;
    sol::protected_function callback;  // copied in; called from worker thread
    std::thread worker;
    std::atomic<bool> running{false};
    std::mutex stop_mu;
    bool stopped{false};

    lua_subscription(std::shared_ptr<sw::redis::Redis> r,
                     sol::protected_function cb)
        : redis(std::move(r)),
          subscriber(redis->subscriber()),
          callback(std::move(cb)) {}
};

struct queue_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    std::string host = "127.0.0.1";
    int port = 6379;
    int db = 0;
    std::string password;
    int connect_timeout_ms = 5000;
    int command_timeout_ms = 1000;

    // Active Lua subscriptions keyed by channel. One consumer thread per
    // channel. stop_all() joins them during shutdown.
    std::mutex subs_mu;
    std::unordered_map<std::string, std::shared_ptr<lua_subscription>> subs;

    // Build a fresh command connection from this instance's settings. Returns
    // nullptr on failure (err, if given, receives the message).
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
        for (auto& kv : snapshot) {
            auto& s = kv.second;
            s->running.store(false);
            try { s->subscriber.close(); } catch (...) {}
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
        try { s->subscriber.close(); } catch (...) {}
        if (s->worker.joinable()) s->worker.join();
    }
};

// Process-wide registry: instance_id -> queue_instance*. The callable Lua
// table's __call metamethod looks up instances by id here. Map is read on
// every proxy creation, so it must be thread-safe.
std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, queue_instance*>& instances_map() {
    static std::map<std::string, queue_instance*> m;
    return m;
}

void register_instance(queue_instance* inst) {
    std::lock_guard<std::mutex> lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}
void unregister_instance(const std::string& id) {
    std::lock_guard<std::mutex> lk(instances_mu());
    instances_map().erase(id);
}
queue_instance* find_instance(const std::string& id) {
    std::lock_guard<std::mutex> lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

// Parse the validated instance config_json. Tolerant — the host already
// checked against config_schema, so we only extract the known keys and fall
// back to defaults for anything missing.
void parse_instance_config(queue_instance* inst, const char* config_json) {
    if (!config_json || !config_json[0]) return;
    try {
        auto j = nlohmann::json::parse(config_json);
        if (j.contains("host") && j["host"].is_string()) {
            inst->host = j["host"].get<std::string>();
        }
        if (j.contains("port") && j["port"].is_number_integer()) {
            inst->port = j["port"].get<int>();
        }
        if (j.contains("db") && j["db"].is_number_integer()) {
            inst->db = j["db"].get<int>();
        }
        if (j.contains("password") && j["password"].is_string()) {
            inst->password = j["password"].get<std::string>();
        }
        if (j.contains("connect_timeout_ms") && j["connect_timeout_ms"].is_number_integer()) {
            inst->connect_timeout_ms = j["connect_timeout_ms"].get<int>();
        }
        if (j.contains("command_timeout_ms") && j["command_timeout_ms"].is_number_integer()) {
            inst->command_timeout_ms = j["command_timeout_ms"].get<int>();
        }
    } catch (...) {
        // Malformed JSON shouldn't happen (host validated), ignore quietly.
    }
}

// Build a Lua error table {code=..., message=...} matching the shape used by
// the host's shield.queue.* facade.
sol::table make_error_table(sol::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}

// Serialize a Lua value to a JSON string for transport over Redis. Strings are
// passed through verbatim; tables/numbers/bools are json-encoded. Returns
// false on unsupported types (functions, userdata, nil message).
bool lua_to_message(const sol::object& v, std::string* out, std::string* err) {
    if (!v.valid() || v == sol::nil) {
        if (err) *err = "message is nil";
        return false;
    }
    if (v.is<std::string>()) {
        *out = v.as<std::string>();
        return true;
    }
    if (v.is<bool>() || v.is<double>() || v.is<lua_Integer>()) {
        try {
            nlohmann::json j;
            if (v.is<bool>()) {
                j = v.as<bool>();
            } else if (v.is<lua_Integer>()) {
                j = v.as<lua_Integer>();
            } else {
                j = v.as<double>();
            }
            *out = j.dump();
            return true;
        } catch (const std::exception& e) {
            if (err) *err = e.what();
            return false;
        }
    }
    if (v.is<sol::table>()) {
        try {
            // sol -> nlohmann is not wired here; encode the common case
            // (string/number/bool values, sequence or map) by hand. Keeps
            // the plugin free of a heavy binding dependency.
            std::function<nlohmann::json(sol::object)> encode;
            encode = [&encode](sol::object o) -> nlohmann::json {
                if (!o.valid() || o == sol::nil) return nullptr;
                if (o.is<bool>())    return o.as<bool>();
                if (o.is<lua_Integer>()) return o.as<lua_Integer>();
                if (o.is<double>())  return o.as<double>();
                if (o.is<std::string>()) return o.as<std::string>();
                if (o.is<sol::table>()) {
                    sol::table t = o.as<sol::table>();
                    // Decide sequence vs map by scanning keys.
                    bool sequence = true;
                    int max_index = 0;
                    for (auto& kv : t) {
                        if (kv.first.get_type() != sol::type::number) {
                            sequence = false;
                            break;
                        }
                        lua_Integer idx = kv.first.as<lua_Integer>();
                        if (idx < 1) { sequence = false; break; }
                        if (idx > max_index) max_index = static_cast<int>(idx);
                    }
                    if (sequence && max_index > 0) {
                        nlohmann::json arr = nlohmann::json::array();
                        // Place each element at its Lua index; gaps become null.
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
                        if (kv.first.get_type() == sol::type::string) {
                            key = kv.first.as<std::string>();
                        } else if (kv.first.get_type() == sol::type::number) {
                            key = std::to_string(kv.first.as<lua_Integer>());
                        } else {
                            continue;  // skip non-stringifiable keys
                        }
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

// Build a per-instance proxy table bound to queue_instance.
//
// publish() opens a fresh command connection per call (redis++ pools
// internally if configured, but a per-call shared_ptr is the simplest
// lifetime model here). subscribe() spins up a dedicated subscriber
// connection + consumer thread, registered on the instance so
// unsubscribe/shutdown can stop it.
//
// THREAD SAFETY (subscribe callback):
//   Lua states are not thread-safe. The subscriber's consumer thread calls
//   the captured sol::protected_function directly. This is the documented
//   v1 limitation (matches the legacy host facade's subscribe pattern).
//   Callers must not re-enter shield.queue.redis from inside the callback on
//   a different thread; marshal back to the Lua worker thread if that is
//   needed.
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
                results.push_back(make_error_table(
                    lua, "invalid_message", msg_err));
                return results;
            }

            std::string conn_err;
            auto redis = inst->make_redis(&conn_err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", conn_err));
                return results;
            }
            try {
                long long receivers = redis->publish(channel, msg);
                results.push_back(sol::make_object(lua, true));
                results.push_back(sol::make_object(
                    lua, static_cast<lua_Integer>(receivers)));
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "redis_command_failed", e.what()));
                return results;
            }
        });

    proxy.set_function("subscribe",
        [inst](sol::this_state s, std::string channel,
               sol::protected_function callback) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;

            // Reject re-subscribe on a channel that already has a consumer —
            // we can only track one callback per channel per instance.
            {
                std::lock_guard<std::mutex> lk(inst->subs_mu);
                if (inst->subs.find(channel) != inst->subs.end()) {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua, "already_subscribed",
                        "channel already has a subscriber; unsubscribe first"));
                    return results;
                }
            }

            std::string conn_err;
            auto redis = inst->make_redis(&conn_err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", conn_err));
                return results;
            }

            auto sub = std::make_shared<lua_subscription>(redis, callback);
            try {
                sub->subscriber.on_message(
                    [cb = sub->callback](
                        const std::string& ch, const std::string& msg) {
                        // Invoked on the consumer thread — see THREAD SAFETY
                        // note above the function. sol::protected_function
                        // is reentrant across threads only if no other thread
                        // is using the same lua_State simultaneously.
                        try {
                            sol::protected_function_result r = cb(ch, msg);
                            (void)r.valid();
                        } catch (...) {
                            // Swallow — a failing user callback must not kill
                            // the consumer thread.
                        }
                    });
                sub->subscriber.subscribe(channel);
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "redis_command_failed", e.what()));
                return results;
            }

            sub->running.store(true);
            auto sub_ptr = sub;  // copy for the thread
            sub->worker = std::thread([sub_ptr]() {
                using namespace std::chrono;
                while (sub_ptr->running.load()) {
                    try {
                        sub_ptr->subscriber.consume();
                    } catch (const sw::redis::TimeoutError&) {
                        // No messages within socket timeout — loop.
                    } catch (const sw::redis::ClosedError&) {
                        break;
                    } catch (const std::exception&) {
                        std::this_thread::sleep_for(milliseconds(100));
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
    // register_lua installs the shared, idempotent callable namespace
    // shield.queue.redis. The per-instance config is already reachable
    // through the global registry (find_instance), so we do not need `self`.
    (void)self;
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "queue.redis: lua_State is null";
        }
        return 1;
    }
    sol::state_view lua(L);

    // Build the callable namespace shield.queue.redis.
    auto shield = lua["shield"].get_or_create<sol::table>();
    auto queue = shield["queue"].get_or_create<sol::table>();

    sol::object existing = queue["redis"];
    if (!existing.is<sol::table>()) {
        auto ns = lua.create_table();
        auto mt = lua.create_table();
        mt.set_function("__call",
            [](sol::this_state s, sol::table /*self*/,
               std::string instance_id) -> sol::object {
                sol::state_view lua(s);
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
    parse_instance_config(inst, args->config_json);
    register_instance(inst);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1*,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_QUEUE_INTERFACE) == 0)
            return &queue_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) { return 0; };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        // shell is the first member of queue_instance (offset 0), so self
        // points at the enclosing queue_instance. Standard C-ABI pattern.
        auto* inst = reinterpret_cast<queue_instance*>(self);
        // Stop all Lua-driven consumer threads BEFORE tearing down state —
        // they hold raw pointers into inst. Once they're joined, no worker
        // will touch inst's members again.
        inst->stop_all();
        unregister_instance(inst->instance_id);
        delete inst;
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
