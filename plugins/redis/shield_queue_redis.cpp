// [SHIELD_PLUGIN] Queue plugin using Redis pub/sub
//
// Delegates to shield_redis_plugin for pub/sub transport.

#include "shield/plugin/plugin.h"
#include "shield/plugin/queue_plugin.h"
#include "shield/plugin/redis_plugin.h"

#include <cstring>
#include <string>

namespace {

const shield_redis_plugin* g_redis = nullptr;
shield_redis_conn* g_conn = nullptr;

int queue_init(const void* redis_plugin, char* err_buf, int err_buf_size) {
    if (!redis_plugin) {
        if (err_buf && err_buf_size > 0)
            std::strncpy(err_buf, "redis plugin not provided", err_buf_size - 1);
        return -1;
    }
    g_redis = static_cast<const shield_redis_plugin*>(redis_plugin);
    return 0;
}

void queue_shutdown() {
    g_redis = nullptr;
    g_conn = nullptr;
}

int queue_publish(const char* channel, const char* data, int data_len) {
    if (!g_redis || !g_conn) return -1;
    return g_redis->publish(g_conn, channel, data, data_len);
}

int queue_subscribe(const char* channel,
                    shield_queue_on_message callback, void* user_data) {
    if (!g_redis || !g_conn) return -1;
    // Delegate to Redis plugin's subscribe.
    // The callback wrapper adapts shield_redis_on_message to shield_queue_on_message.
    return g_redis->subscribe(g_conn, channel,
        [](const char* ch, const char* data, int len, void* ud) {
            auto* cb_data = static_cast<std::pair<shield_queue_on_message, void*>*>(ud);
            cb_data->first(ch, data, len, cb_data->second);
        },
        new std::pair<shield_queue_on_message, void*>(callback, user_data));
}

int queue_unsubscribe(const char* channel) {
    if (!g_redis || !g_conn) return -1;
    return g_redis->unsubscribe(g_conn, channel);
}

int queue_consume_group(shield_queue_conn*, const char*, const char*,
                        const char*, shield_queue_message*) {
    return -1;  // Not supported by basic Redis pub/sub
}

int queue_ack(shield_queue_conn*, const char*, uint64_t) {
    return -1;  // Not supported by basic Redis pub/sub
}

void queue_free_message(shield_queue_message* msg) {
    if (msg) {
        if (msg->channel) std::free(const_cast<char*>(msg->channel));
        if (msg->data) std::free(const_cast<char*>(msg->data));
    }
}

const shield_queue_plugin g_queue_plugin = {
    SHIELD_QUEUE_ABI_VERSION,
    "redis_queue",
    "1.0.0",

    // connect/disconnect/ping - delegate to redis
    nullptr, nullptr, nullptr,

    queue_publish,
    queue_subscribe,
    queue_unsubscribe,
    queue_consume_group,
    queue_ack,
    queue_free_message,
};

const shield_plugin g_plugin = {
    SHIELD_PLUGIN_ABI_VERSION,
    SHIELD_PLUGIN_TYPE_QUEUE,
    "shield_queue",
    "1.0.0",
    "Message queue plugin using Redis pub/sub",
    "Shield",

    [](const shield_host_t host, const shield_host_api* api,
       const shield_plugin_config*, char* err, int err_len) -> int {
        const shield_plugin* redis_p = api->find_plugin(host, "shield_redis");
        if (!redis_p) {
            if (err && err_len > 0) std::strncpy(err, "shield_redis not loaded", err_len - 1);
            return -1;
        }
        return queue_init(redis_p->vtable, err, err_len);
    },

    []() { queue_shutdown(); },
    []() -> int { return 1; },
    [](int) -> const shield_plugin_capability* {
        static shield_plugin_capability cap = {"queue", "1.0.0", "Redis pub/sub queue"};
        return &cap;
    },

    &g_queue_plugin,
};

}  // namespace

extern "C" __declspec(dllexport)
const struct shield_plugin* shield_plugin_api(void) {
    return &g_plugin;
}
