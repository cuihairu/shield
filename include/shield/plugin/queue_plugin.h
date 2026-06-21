// [SHIELD_PLUGIN] Message queue plugin C ABI
//
// High-level message queue abstraction. Depends on a Redis plugin for
// pub/sub transport.
//
// Usage from another plugin:
//   const shield_plugin* p = host_api->find_plugin(host, "shield_queue");
//   const shield_queue_plugin* queue = p->vtable;
//   queue->publish("chat.world", "hello", 5);

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_QUEUE_ABI_VERSION 1

// Message receive callback.
typedef void (*shield_queue_on_message)(
    const char* channel,
    const char* data,
    int data_len,
    void* user_data);

struct shield_queue_plugin {
    uint32_t abi_version;
    const char* name;
    const char* version;

    // Initialize with a reference to the Redis plugin.
    int (*init)(const void* redis_plugin, char* err_buf, int err_buf_size);
    void (*shutdown)(void);

    // Publish a message to a channel.
    int (*publish)(const char* channel, const char* data, int data_len);

    // Subscribe to a channel (async, callback-based).
    int (*subscribe)(const char* channel,
                     shield_queue_on_message callback,
                     void* user_data);

    // Unsubscribe from a channel.
    int (*unsubscribe)(const char* channel);
};

#ifdef __cplusplus
}
#endif
