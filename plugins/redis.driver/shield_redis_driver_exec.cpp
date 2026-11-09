// [SHIELD_PLUGIN] redis.driver — C-ABI executor: the value-tree
// constructors (reply_to_value and the make_* family), the
// process-wide instance registry backing connect()'s self-pointer
// resolution and the typed shield.redis.v2 vtable
// (kv/hash/zset/pipeline/command).
// Split out of shield_redis_driver.cpp.
#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/redis.h"
#include "shield_lua_plugin_binding.hpp"
#include "shield_redis_driver_impl.hpp"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {
char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}
}  // namespace

namespace {
char* dup_bytes(const void* data, uint64_t len) {
    if (!data || len == 0) return nullptr;
    char* out = static_cast<char*>(std::malloc(static_cast<size_t>(len)));
    if (out) std::memcpy(out, data, static_cast<size_t>(len));
    return out;
}
}  // namespace

namespace {
// Allocate a single shield_redis_value_v1 on the heap.
shield_redis_value_v1* alloc_value() {
    auto* v = static_cast<shield_redis_value_v1*>(
        std::calloc(1, sizeof(shield_redis_value_v1)));
    return v;
}
}  // namespace

namespace {
// Build a STRING value from a std::string.
shield_redis_value_v1* make_string_value(const std::string& s) {
    auto* v = alloc_value();
    if (!v) return nullptr;
    v->type = SHIELD_REDIS_STRING;
    v->str = dup_bytes(s.data(), s.size());
    v->str_len = s.size();
    return v;
}
}  // namespace

namespace {
// Build an INTEGER value.
shield_redis_value_v1* make_integer_value(int64_t n) {
    auto* v = alloc_value();
    if (!v) return nullptr;
    v->type = SHIELD_REDIS_INTEGER;
    v->integer = n;
    return v;
}
}  // namespace

namespace {
// Build a NIL value.
shield_redis_value_v1* make_nil_value() {
    auto* v = alloc_value();
    if (!v) return nullptr;
    v->type = SHIELD_REDIS_NIL;
    return v;
}
}  // namespace

namespace {
// Build an ERROR value from a string.
shield_redis_value_v1* make_error_value(const std::string& msg) {
    auto* v = alloc_value();
    if (!v) return nullptr;
    v->type = SHIELD_REDIS_ERROR;
    v->str = dup_string(msg.c_str());
    v->str_len = msg.size();
    return v;
}
}  // namespace

namespace {
// Recursively convert a hiredis redisReply to a shield_redis_value_v1.
// redis++ returns replies as std::unique_ptr<redisReply> (ReplyUPtr).
// We work with the raw redisReply* from hiredis directly.
shield_redis_value_v1* reply_to_value(const redisReply* reply) {
    if (!reply) return make_nil_value();
    switch (reply->type) {
        case REDIS_REPLY_NIL:
            return make_nil_value();
        case REDIS_REPLY_STRING:
        case REDIS_REPLY_STATUS:
            return make_string_value(
                std::string(reply->str, static_cast<size_t>(reply->len)));
        case REDIS_REPLY_INTEGER:
            return make_integer_value(reply->integer);
        case REDIS_REPLY_ERROR:
            return make_error_value(
                std::string(reply->str, static_cast<size_t>(reply->len)));
        case REDIS_REPLY_ARRAY: {
            auto* v = alloc_value();
            if (!v) return nullptr;
            v->type = SHIELD_REDIS_ARRAY;
            v->item_count = reply->elements;
            if (reply->elements == 0 || reply->element == nullptr) {
                v->items = nullptr;
            } else {
                v->items = static_cast<shield_redis_value_v1*>(std::calloc(
                    reply->elements, sizeof(shield_redis_value_v1)));
                for (size_t i = 0; i < reply->elements; ++i) {
                    auto* item = reply_to_value(reply->element[i]);
                    if (item) {
                        v->items[i] = *item;
                        std::free(item);  // shallow copy into array
                    }
                }
            }
            return v;
        }
        default:
            return make_nil_value();
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// Process-wide instance registry (for Lua proxy resolution)
// ---------------------------------------------------------------------------

namespace {
std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, redis_instance*>& instances_map() {
    static std::map<std::string, redis_instance*> m;
    return m;
}
}  // namespace

void register_instance(redis_instance* inst) {
    std::lock_guard lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}
void unregister_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    instances_map().erase(id);
}
redis_instance* find_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

namespace {
// Release all contents owned by a node: its string and its items array,
// recursing into each embedded child's contents. Does NOT free the node
// itself — children live inline inside the parent's items array (shallow
// copy from reply_to_value), so only the heap-allocated root is freed.
// XREADGROUP and other raw replies nest arrays (streams -> entries ->
// fields) far deeper than the old three-level walk; recursing to any depth
// prevents deep strings and array nodes from leaking.
void free_value_contents(shield_redis_value_v1* value) {
    if (!value) return;
    if (value->str) {
        std::free(const_cast<char*>(value->str));
        value->str = nullptr;
    }
    if (value->items) {
        for (uint64_t i = 0; i < value->item_count; ++i) {
            free_value_contents(&value->items[i]);
        }
        std::free(value->items);
        value->items = nullptr;
    }
    value->str_len = 0;
    value->item_count = 0;
}
}  // namespace

namespace {
// Release a value tree rooted at a heap-allocated node. The node returned by
// the driver (reply_to_value allocates it with calloc) is freed after its
// contents; nested nodes are embedded in parent items arrays and are handled
// by the recursive content walk.
void free_value_impl(shield_redis_value_v1* value) {
    if (!value) return;
    free_value_contents(value);
    std::free(value);
}
}  // namespace

// ---------------------------------------------------------------------------
// v2 vtable — typed methods
//
// The function pointers are shared, but every instance gets its own vtable
// copy (redis_instance::vtable_) filled by init_vtable(). connect() receives
// that vtable pointer as `self` and resolves the owning instance through the
// registry, so a consumer always lands on the instance its dependency was
// wired to — never "the first started instance".
// ---------------------------------------------------------------------------

void init_vtable(shield_redis_v2* out) {
    *out = shield_redis_v2{
        sizeof(shield_redis_v2),
        SHIELD_REDIS_V2,

        // connect — returns the driver instance that owns `self` as an opaque
        // handle. cfg is ignored (the driver uses its own config from
        // startup). Dependent plugins call this once to get a handle for all
        // methods.
        [](const shield_redis_v2* self, const void* /*cfg*/, char* err_buf,
           int err_buf_size) -> void* {
            // Resolve the owning instance via vtable identity: get_interface()
            // returned exactly &inst->vtable_ for some started instance.
            if (!self) {
                if (err_buf && err_buf_size > 0) {
                    std::snprintf(
                        err_buf, err_buf_size,
                        "redis.driver: connect called with null vtable");
                }
                return nullptr;
            }
            std::lock_guard lk(instances_mu());
            for (auto& [id, inst] : instances_map()) {
                if (inst && &inst->vtable_ == self) {
                    if (!inst->redis) break;  // instance not started yet
                    return static_cast<void*>(inst);
                }
            }
            if (err_buf && err_buf_size > 0) {
                std::snprintf(
                    err_buf, err_buf_size,
                    "redis.driver: no started instance for this vtable");
            }
            return nullptr;
        },

        // disconnect — no-op. The pool is owned by the driver instance.
        [](void* /*handle*/) {},

        // get
        [](void* inst, const char* key, shield_redis_value_v1** out,
           shield_error_v1* err) -> int {
            if (!inst || !key || !out) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                auto val = ri->redis->get(std::string(key));
                if (val) {
                    *out = make_string_value(*val);
                } else {
                    *out = make_nil_value();
                }
                return 0;
            } catch (const std::exception& e) {
                if (err) {
                    err->code = "redis.command.failed";
                    err->message = e.what();
                }
                *out = make_nil_value();
                return -1;
            }
        },

        // set
        [](void* inst, const char* key, const char* val, int ttl_sec,
           shield_error_v1* err) -> int {
            if (!inst || !key || !val) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                if (ttl_sec > 0) {
                    ri->redis->set(std::string(key), std::string(val),
                                   std::chrono::seconds(ttl_sec));
                } else {
                    ri->redis->set(std::string(key), std::string(val));
                }
                return 0;
            } catch (const std::exception& e) {
                if (err) {
                    err->code = "redis.command.failed";
                    err->message = e.what();
                }
                return -1;
            }
        },

        // del
        [](void* inst, const char* key, shield_error_v1* err) -> int {
            if (!inst || !key) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                ri->redis->del(std::string(key));
                return 0;
            } catch (const std::exception& e) {
                if (err) {
                    err->code = "redis.command.failed";
                    err->message = e.what();
                }
                return -1;
            }
        },

        // hget
        [](void* inst, const char* key, const char* field,
           shield_redis_value_v1** out, shield_error_v1* err) -> int {
            if (!inst || !key || !field || !out) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                auto val =
                    ri->redis->hget(std::string(key), std::string(field));
                if (val) {
                    *out = make_string_value(*val);
                } else {
                    *out = make_nil_value();
                }
                return 0;
            } catch (const std::exception& e) {
                if (err) {
                    err->code = "redis.command.failed";
                    err->message = e.what();
                }
                *out = make_nil_value();
                return -1;
            }
        },

        // hset
        [](void* inst, const char* key, const char* field, const char* val,
           shield_error_v1* err) -> int {
            if (!inst || !key || !field || !val) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                ri->redis->hset(std::string(key), std::string(field),
                                std::string(val));
                return 0;
            } catch (const std::exception& e) {
                if (err) {
                    err->code = "redis.command.failed";
                    err->message = e.what();
                }
                return -1;
            }
        },

        // hgetall
        [](void* inst, const char* key, shield_redis_value_v1** out,
           shield_error_v1* err) -> int {
            if (!inst || !key || !out) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                std::map<std::string, std::string> result;
                ri->redis->hgetall(std::string(key),
                                   std::inserter(result, result.begin()));
                auto* v = alloc_value();
                if (!v) return -1;
                v->type = SHIELD_REDIS_ARRAY;
                v->item_count = result.size() * 2;
                if (result.empty()) {
                    v->items = nullptr;
                } else {
                    v->items = static_cast<shield_redis_value_v1*>(std::calloc(
                        v->item_count, sizeof(shield_redis_value_v1)));
                    size_t i = 0;
                    for (const auto& [f, val] : result) {
                        // field
                        v->items[i].type = SHIELD_REDIS_STRING;
                        v->items[i].str = dup_bytes(f.data(), f.size());
                        v->items[i].str_len = f.size();
                        ++i;
                        // value
                        v->items[i].type = SHIELD_REDIS_STRING;
                        v->items[i].str = dup_bytes(val.data(), val.size());
                        v->items[i].str_len = val.size();
                        ++i;
                    }
                }
                *out = v;
                return 0;
            } catch (const std::exception& e) {
                if (err) {
                    err->code = "redis.command.failed";
                    err->message = e.what();
                }
                *out = make_nil_value();
                return -1;
            }
        },

        // zadd
        [](void* inst, const char* key, double score, const char* member,
           shield_error_v1* err) -> int {
            if (!inst || !key || !member) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                ri->redis->zadd(std::string(key), std::string(member), score);
                return 0;
            } catch (const std::exception& e) {
                if (err) {
                    err->code = "redis.command.failed";
                    err->message = e.what();
                }
                return -1;
            }
        },

        // zrange
        [](void* inst, const char* key, int start, int stop,
           shield_redis_value_v1** out, shield_error_v1* err) -> int {
            if (!inst || !key || !out) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                std::vector<std::string> result;
                ri->redis->zrange(std::string(key), start, stop,
                                  std::back_inserter(result));
                auto* v = alloc_value();
                if (!v) return -1;
                v->type = SHIELD_REDIS_ARRAY;
                v->item_count = result.size();
                if (result.empty()) {
                    v->items = nullptr;
                } else {
                    v->items = static_cast<shield_redis_value_v1*>(std::calloc(
                        result.size(), sizeof(shield_redis_value_v1)));
                    for (size_t i = 0; i < result.size(); ++i) {
                        v->items[i].type = SHIELD_REDIS_STRING;
                        v->items[i].str =
                            dup_bytes(result[i].data(), result[i].size());
                        v->items[i].str_len = result[i].size();
                    }
                }
                *out = v;
                return 0;
            } catch (const std::exception& e) {
                if (err) {
                    err->code = "redis.command.failed";
                    err->message = e.what();
                }
                *out = make_nil_value();
                return -1;
            }
        },

        // pipeline
        [](void* inst, const shield_redis_command_v1* cmds, uint64_t count,
           shield_redis_value_v1*** out_array, uint64_t* out_count,
           shield_error_v1* err) -> int {
            if (!inst || !cmds || !out_array || !out_count) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                auto pipe = ri->redis->pipeline();
                for (uint64_t i = 0; i < count; ++i) {
                    const auto& cmd = cmds[i];
                    if (cmd.argc == 0 || !cmd.args) continue;
                    std::vector<std::string> argv;
                    for (uint64_t a = 0; a < cmd.argc; ++a) {
                        argv.emplace_back(
                            static_cast<const char*>(cmd.args[a].data),
                            static_cast<size_t>(cmd.args[a].len));
                    }
                    pipe.command(argv.begin(), argv.end());
                }
                // exec() returns QueuedReplies; use .get(idx) for each reply.
                auto replies = pipe.exec();
                *out_count = replies.size();
                *out_array = static_cast<shield_redis_value_v1**>(std::calloc(
                    replies.size(), sizeof(shield_redis_value_v1*)));
                if (replies.size() != 0 && !*out_array) {
                    throw std::bad_alloc();
                }
                for (size_t i = 0; i < replies.size(); ++i) {
                    (*out_array)[i] = reply_to_value(&replies.get(i));
                }
                return 0;
            } catch (const std::exception& e) {
                if (err) {
                    err->code = "redis.pipeline.failed";
                    err->message = e.what();
                }
                return -1;
            }
        },

        // command (raw escape hatch)
        [](void* inst, const shield_redis_arg_v1* args, uint64_t argc,
           shield_redis_value_v1** out, shield_error_v1* err) -> int {
            if (!inst || !args || argc == 0 || !out) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                std::vector<std::string> argv;
                for (uint64_t i = 0; i < argc; ++i) {
                    argv.emplace_back(static_cast<const char*>(args[i].data),
                                      static_cast<size_t>(args[i].len));
                }
                // command() returns ReplyUPtr (unique_ptr<redisReply>)
                auto reply = ri->redis->command(argv.begin(), argv.end());
                *out = reply_to_value(reply.get());
                return 0;
            } catch (const std::exception& e) {
                if (err) {
                    err->code = "redis.command.failed";
                    err->message = e.what();
                }
                *out = make_nil_value();
                return -1;
            }
        },

        // free_value
        [](shield_redis_value_v1* value) { free_value_impl(value); },
    };
}
