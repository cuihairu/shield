// [SHIELD_PLUGIN] queue.redis — Lua facade: dual-path command
// helpers (redis.driver vtable when wired, otherwise the
// subscription's own redis++ connection) and the per-instance
// proxy whose consumer threads post messages to the owning
// service actor, plus register_lua.
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

// ---------------------------------------------------------------------------
// Lua subscription (Streams-based)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Dual-path command helpers.
//
// When the instance is wired to a redis.driver dependency, commands run
// through the driver vtable's raw command() escape hatch on the driver's
// shared connection pool; otherwise they run on the subscription's own
// redis++ connection. NOTE: a blocking XREADGROUP through the driver pool
// holds one pooled connection for up to kBlockMs per loop iteration — size
// the driver's pool_size accordingly.
// ---------------------------------------------------------------------------

namespace {
// (msg_id, payload) pairs extracted from one XREADGROUP reply.
using StreamDeliveries = std::vector<std::pair<std::string, std::string>>;
}  // namespace

namespace {
// XREADGROUP via the fallback redis++ connection. Returns false on error
// (caller backs off and retries). An empty/NIL reply is not an error.
bool read_group_own(sw::redis::Redis& redis, const std::string& channel,
                    const std::string& consumer, StreamDeliveries* out) {
    auto result =
        redis.command("XREADGROUP", "GROUP", kGroupName, consumer, "COUNT",
                      std::to_string(kReadCount), "BLOCK",
                      std::to_string(kBlockMs), "STREAMS", channel, ">");
    // result is an array of streams; each stream is [stream_name, [[id,
    // fields], ...]]
    if (!result || result->type == REDIS_REPLY_NIL) return true;
    if (result->type != REDIS_REPLY_ARRAY) return true;
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
            // Extract "payload" field. Fields are [key, value, key, value, ...]
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
            out->emplace_back(std::move(msg_id), std::move(payload));
        }
    }
    return true;
}

// XREADGROUP via the redis.driver vtable. Same reply shape as read_group_own
// but delivered as shield_redis_value_v1 trees. Returns false on error.
bool read_group_driver(const shield_redis_v2* drv, void* handle,
                       const std::string& channel, const std::string& consumer,
                       StreamDeliveries* out) {
    std::string count = std::to_string(kReadCount);
    std::string block = std::to_string(kBlockMs);
    const shield_redis_arg_v1 args[] = {
        {"XREADGROUP", 10},
        {"GROUP", 5},
        {kGroupName, std::strlen(kGroupName)},
        {consumer.data(), consumer.size()},
        {"COUNT", 5},
        {count.data(), count.size()},
        {"BLOCK", 5},
        {block.data(), block.size()},
        {"STREAMS", 7},
        {channel.data(), channel.size()},
        {">", 1},
    };
    shield_redis_value_v1* reply = nullptr;
    shield_error_v1 err{};
    const int rc = drv->command(handle, args, 11, &reply, &err);
    if (rc != 0) {
        if (reply) drv->free_value(reply);
        return false;
    }
    if (!reply) return true;
    if (reply->type == SHIELD_REDIS_ARRAY) {
        for (uint64_t si = 0; si < reply->item_count; ++si) {
            auto* stream = &reply->items[si];
            if (stream->type != SHIELD_REDIS_ARRAY || stream->item_count < 2)
                continue;
            auto* entries = &stream->items[1];
            if (entries->type != SHIELD_REDIS_ARRAY) continue;
            for (uint64_t ei = 0; ei < entries->item_count; ++ei) {
                auto* entry = &entries->items[ei];
                if (entry->type != SHIELD_REDIS_ARRAY || entry->item_count < 2)
                    continue;
                auto* id_v = &entry->items[0];
                auto* fields = &entry->items[1];
                if (id_v->type != SHIELD_REDIS_STRING || !id_v->str) continue;
                if (fields->type != SHIELD_REDIS_ARRAY) continue;
                std::string msg_id(id_v->str,
                                   static_cast<size_t>(id_v->str_len));
                std::string payload;
                for (uint64_t fi = 0; fi + 1 < fields->item_count; fi += 2) {
                    auto* key = &fields->items[fi];
                    auto* val = &fields->items[fi + 1];
                    if (key->type == SHIELD_REDIS_STRING && key->str &&
                        std::string(key->str, static_cast<size_t>(
                                                  key->str_len)) == "payload" &&
                        val->type == SHIELD_REDIS_STRING && val->str) {
                        payload.assign(val->str,
                                       static_cast<size_t>(val->str_len));
                        break;
                    }
                }
                out->emplace_back(std::move(msg_id), std::move(payload));
            }
        }
    }
    drv->free_value(reply);
    return true;
}

// XACK one message through whichever connection the subscription uses.
void xack(const lua_subscription& s, const std::string& msg_id) {
    try {
        if (s.redis_driver && s.redis_handle) {
            const shield_redis_arg_v1 args[] = {
                {"XACK", 4},
                {s.channel.data(), s.channel.size()},
                {kGroupName, std::strlen(kGroupName)},
                {msg_id.data(), msg_id.size()},
            };
            shield_redis_value_v1* out = nullptr;
            shield_error_v1 err{};
            s.redis_driver->command(s.redis_handle, args, 4, &out, &err);
            if (out) s.redis_driver->free_value(out);
        } else if (s.redis) {
            s.redis->command("XACK", s.channel, kGroupName, msg_id);
        }
    } catch (...) {
    }
}

// XGROUP CREATE via the driver vtable. Idempotent — BUSYGROUP is ignored.
bool ensure_group_driver(const shield_redis_v2* drv, void* handle,
                         const std::string& stream, std::string* err_msg) {
    const shield_redis_arg_v1 args[] = {
        {"XGROUP", 6},
        {"CREATE", 6},
        {stream.data(), stream.size()},
        {kGroupName, std::strlen(kGroupName)},
        {"0", 1},
        {"MKSTREAM", 8},
    };
    shield_redis_value_v1* out = nullptr;
    shield_error_v1 err{};
    const int rc = drv->command(handle, args, 6, &out, &err);
    bool ok = true;
    if (rc != 0 || (out && out->type == SHIELD_REDIS_ERROR)) {
        // BUSYGROUP = group already exists; ignore.
        const std::string msg =
            (out && out->type == SHIELD_REDIS_ERROR && out->str)
                ? std::string(out->str, static_cast<size_t>(out->str_len))
                : (err.message ? err.message : "XGROUP CREATE failed");
        if (msg.find("BUSYGROUP") == std::string::npos) {
            ok = false;
            if (err_msg) *err_msg = msg;
        }
    }
    if (out) drv->free_value(out);
    return ok;
}
}  // namespace

namespace {
shd::table make_error_table(shd::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}

bool lua_to_message(const shd::object& v, std::string* out, std::string* err) {
    if (!v.valid() || v == shd::nil) {
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
            if (v.is<bool>())
                j = v.as<bool>();
            else if (v.is<lua_Integer>())
                j = v.as<lua_Integer>();
            else
                j = v.as<double>();
            *out = j.dump();
            return true;
        } catch (const std::exception& e) {
            if (err) *err = e.what();
            return false;
        }
    }
    if (v.is<shd::table>()) {
        try {
            std::function<nlohmann::json(shd::object)> encode;
            encode = [&encode](shd::object o) -> nlohmann::json {
                if (!o.valid() || o == shd::nil) return nullptr;
                if (o.is<bool>()) return o.as<bool>();
                if (o.is<lua_Integer>()) return o.as<lua_Integer>();
                if (o.is<double>()) return o.as<double>();
                if (o.is<std::string>()) return o.as<std::string>();
                if (o.is<shd::table>()) {
                    shd::table t = o.as<shd::table>();
                    bool sequence = true;
                    int max_index = 0;
                    for (const auto& kv : t) {
                        if (kv.first.get_type() != shd::type::number) {
                            sequence = false;
                            break;
                        }
                        lua_Integer idx = kv.first.as<lua_Integer>();
                        if (idx < 1) {
                            sequence = false;
                            break;
                        }
                        if (idx > max_index) max_index = static_cast<int>(idx);
                    }
                    if (sequence && max_index > 0) {
                        nlohmann::json arr = nlohmann::json::array();
                        std::vector<nlohmann::json> tmp(
                            static_cast<size_t>(max_index), nullptr);
                        for (const auto& kv : t) {
                            int idx =
                                static_cast<int>(kv.first.as<lua_Integer>());
                            tmp[static_cast<size_t>(idx - 1)] =
                                encode(kv.second);
                        }
                        for (auto& e : tmp) arr.push_back(std::move(e));
                        return arr;
                    }
                    nlohmann::json obj = nlohmann::json::object();
                    for (const auto& kv : t) {
                        std::string key;
                        if (kv.first.get_type() == shd::type::string)
                            key = kv.first.as<std::string>();
                        else if (kv.first.get_type() == shd::type::number)
                            key = std::to_string(kv.first.as<lua_Integer>());
                        else
                            continue;
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
}  // namespace

namespace {
// Lua proxy — Streams-based publish/subscribe
shd::table make_instance_proxy(shd::state_view lua, queue_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "publish",
        [inst](shd::this_state s, std::string channel,
               shd::object message) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string msg, msg_err;
            if (!lua_to_message(message, &msg, &msg_err)) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "invalid_message", msg_err));
                return results;
            }
            // Driver path: XADD through redis.driver's shared pool.
            if (inst->redis_driver && inst->redis_handle) {
                const shield_redis_arg_v1 args[] = {
                    {"XADD", 4},    {channel.data(), channel.size()}, {"*", 1},
                    {"payload", 7}, {msg.data(), msg.size()},
                };
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                const int rc = inst->redis_driver->command(inst->redis_handle,
                                                           args, 5, &out, &err);
                const bool ok =
                    rc == 0 && !(out && out->type == SHIELD_REDIS_ERROR);
                const std::string emsg =
                    ok ? std::string()
                    : (out && out->type == SHIELD_REDIS_ERROR && out->str)
                        ? std::string(out->str,
                                      static_cast<size_t>(out->str_len))
                        : (err.message ? err.message : "XADD failed");
                if (out) inst->redis_driver->free_value(out);
                if (ok) {
                    results.push_back(shd::make_object(lua, true));
                } else {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(
                        make_error_table(lua, "redis_command_failed", emsg));
                }
                return results;
            }
            std::string conn_err;
            auto redis = inst->make_redis(&conn_err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", conn_err));
                return results;
            }
            try {
                redis->command("XADD", channel, "*", "payload", msg);
                results.push_back(shd::make_object(lua, true));
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "redis_command_failed", e.what()));
            }
            return results;
        });

    proxy.set_function(
        "subscribe",
        [inst](shd::this_state s, std::string channel,
               shd::protected_function callback) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            {
                std::lock_guard<std::mutex> lk(inst->subs_mu);
                if (inst->subs.find(channel) != inst->subs.end()) {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua, "already_subscribed",
                        "channel already has a subscriber; unsubscribe first"));
                    return results;
                }
            }
            // The Lua callback must run on the owning service actor, so the
            // owner has to be known at subscribe time.
            const char* owner = nullptr;
            if (inst->host_api && inst->host_api->lua_current_service_id) {
                owner = inst->host_api->lua_current_service_id(inst->ctx);
            }
            if (!owner || !owner[0]) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "invalid_context",
                                     "subscribe must be called from a service "
                                     "handler or on_init"));
                return results;
            }
            // Dual-path: use redis.driver's pool when the dependency is
            // wired, otherwise a self-owned connection.
            const bool use_driver = inst->redis_driver && inst->redis_handle;
            std::shared_ptr<sw::redis::Redis> redis;
            if (use_driver) {
                std::string grp_err;
                if (!ensure_group_driver(inst->redis_driver, inst->redis_handle,
                                         channel, &grp_err)) {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(
                        make_error_table(lua, "redis_command_failed", grp_err));
                    return results;
                }
            } else {
                std::string conn_err;
                redis = inst->make_redis(&conn_err);
                if (!redis) {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(
                        make_error_table(lua, "connection_failed", conn_err));
                    return results;
                }
                // Ensure consumer group
                try {
                    ensure_group(*redis, channel, kGroupName);
                } catch (const std::exception& e) {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua, "redis_command_failed", e.what()));
                    return results;
                }
            }
            auto sub = use_driver ? std::make_shared<lua_subscription>(
                                        inst->redis_driver, inst->redis_handle,
                                        callback, channel, owner)
                                  : std::make_shared<lua_subscription>(
                                        redis, callback, channel, owner);
            sub->running.store(true);
            auto sub_ptr = sub;
            const shield_host_api_v1* host_api = inst->host_api;
            shield_plugin_context_v1* ctx = inst->ctx;
            sub->worker = std::thread([sub_ptr, host_api, ctx]() {
                // Posted delivery: runs on the owning service actor via
                // host_api.lua_post_to_service. The Lua callback and the
                // XACK both execute on the actor thread; the consumer thread
                // only does XREADGROUP. Messages that cannot be posted
                // (service gone) stay pending in Redis (at-least-once).
                struct Delivery {
                    std::weak_ptr<lua_subscription> sub;
                    std::string msg_id;
                    std::string payload;
                };
                while (sub_ptr->running.load()) {
                    try {
                        // Only the connection that executes commands differs
                        // (driver pool vs own); the delivery semantics are
                        // unchanged: XREADGROUP here, Lua callback + XACK on
                        // the owning service actor via lua_post_to_service.
                        StreamDeliveries deliveries;
                        const bool read_ok =
                            (sub_ptr->redis_driver && sub_ptr->redis_handle)
                                ? read_group_driver(
                                      sub_ptr->redis_driver,
                                      sub_ptr->redis_handle, sub_ptr->channel,
                                      sub_ptr->consumer_name, &deliveries)
                                : read_group_own(
                                      *sub_ptr->redis, sub_ptr->channel,
                                      sub_ptr->consumer_name, &deliveries);
                        if (!read_ok) {
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(100));
                            continue;
                        }
                        for (auto& [msg_id, payload] : deliveries) {
                            auto* d = new Delivery{sub_ptr, msg_id, payload};
                            const int rc =
                                host_api && host_api->lua_post_to_service
                                    ? host_api->lua_post_to_service(
                                          ctx, sub_ptr->service_id.c_str(),
                                          [](void* ud) {
                                              auto* d =
                                                  static_cast<Delivery*>(ud);
                                              auto s = d->sub.lock();
                                              if (!s || !s->running.load())
                                                  return;
                                              try {
                                                  shd::protected_function_result
                                                      r = s->callback(
                                                          s->channel,
                                                          d->payload);
                                                  (void)r.valid();
                                              } catch (...) {
                                              }
                                              xack(*s, d->msg_id);
                                          },
                                          d,
                                          [](void* ud) {
                                              delete static_cast<Delivery*>(ud);
                                          })
                                    : -1;
                            if (rc != 0) {
                                // Service gone or hook unavailable:
                                // leave the entry pending in Redis.
                                delete d;
                            }
                        }
                    } catch (const sw::redis::Error&) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(100));
                    } catch (...) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(100));
                    }
                }
            });
            {
                std::lock_guard<std::mutex> lk(inst->subs_mu);
                inst->subs[channel] = std::move(sub);
            }
            results.push_back(shd::make_object(lua, true));
            return results;
        });

    proxy.set_function("unsubscribe",
                       [inst](shd::this_state s,
                              std::string channel) -> shd::variadic_results {
                           shd::state_view lua(s);
                           shd::variadic_results results;
                           inst->stop_one(channel);
                           results.push_back(shd::make_object(lua, true));
                           return results;
                       });

    return proxy;
}
}  // namespace

int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err) {
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "queue.redis: lua_State is null";
        }
        return 1;
    }
    auto* current = reinterpret_cast<queue_instance*>(self);
    if (!current || !current->host_api ||
        !current->host_api->binding_instance_id) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "queue.redis: host binding resolver is null";
        }
        return 1;
    }
    shd::state_view lua(L);
    shd::table shield =
        shield::plugins::get_or_create_global_subtable(lua, "shield");
    shd::table queue = shield::plugins::get_or_create_subtable(shield, "queue");
    shd::object existing = queue["redis"];
    if (!existing.is<shd::table>()) {
        auto ns = lua.create_table();
        auto mt = lua.create_table();
        const shield_host_api_v1* host_api = current->host_api;
        shield_plugin_context_v1* ctx = current->ctx;
        mt.set_function(
            "__call",
            [host_api, ctx](
                shd::this_state s, shd::table,
                std::optional<std::string> binding) -> shd::variadic_results {
                shd::state_view lua(s);
                shd::variadic_results results;
                std::string logical = binding.value_or("");
                auto* inst = shield::plugins::resolve_lua_binding(
                    host_api, ctx, logical, find_instance);
                if (!inst) {
                    shield::plugins::push_module_unavailable(results, lua,
                                                             logical);
                    return results;
                }
                results.push_back(
                    shd::make_object(lua, make_instance_proxy(lua, inst)));
                return results;
            });
        shield::plugins::set_metatable(ns, mt);
        queue["redis"] = ns;
    }
    return 0;
}
