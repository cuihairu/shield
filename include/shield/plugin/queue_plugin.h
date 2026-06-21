// [SHIELD_PLUGIN] Message queue plugin C ABI
//
// Stable C interface for message queue backends (Redis pub/sub, RabbitMQ,
// NATS, Kafka, in-memory, etc.).
//
// Integration with shield_plugin system:
//   type = SHIELD_PLUGIN_TYPE_QUEUE, vtable → shield_queue_plugin*

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_QUEUE_ABI_VERSION 1

struct shield_queue_conn;

struct shield_queue_config {
    const char* host;
    int port;
    const char* password;
    int connect_timeout_ms;
    const char* extra_json;        // driver-specific options
};

// A received message.
struct shield_queue_message {
    uint64_t id;                   // message ID (0 if not supported)
    const char* channel;
    const char* data;
    int data_len;
};

// Message receive callback.
typedef void (*shield_queue_on_message)(
    const char* channel,
    const char* data,
    int data_len,
    void* user_data);

struct shield_queue_plugin {
    uint32_t abi_version;
    const char* name;              // "redis", "rabbitmq", "nats", "memory"
    const char* version;

    // Connection
    struct shield_queue_conn* (*connect)(const struct shield_queue_config* config,
                                         char* err_buf, int err_buf_size);
    void (*disconnect)(struct shield_queue_conn* conn);
    int (*ping)(struct shield_queue_conn* conn);

    // Publish a message to a channel.
    int (*publish)(struct shield_queue_conn* conn,
                   const char* channel,
                   const char* data,
                   int data_len);

    // Subscribe to a channel (async, callback-based).
    int (*subscribe)(struct shield_queue_conn* conn,
                     const char* channel,
                     shield_queue_on_message callback,
                     void* user_data);

    // Unsubscribe from a channel.
    int (*unsubscribe)(struct shield_queue_conn* conn,
                       const char* channel);

    // Consumer group operations (Kafka/Redis Streams style).
    // Returns 0 on success, -1 if not supported by this backend.
    int (*consume_group)(struct shield_queue_conn* conn,
                         const char* channel,
                         const char* group_id,
                         const char* consumer_id,
                         struct shield_queue_message* out);

    // Acknowledge a message (for reliable delivery).
    int (*ack)(struct shield_queue_conn* conn,
               const char* channel,
               uint64_t message_id);

    // Memory
    void (*free_message)(struct shield_queue_message* msg);
};

// Entry point exported by every queue plugin DLL.
#ifdef _WIN32
#define SHIELD_QUEUE_EXPORT __declspec(dllexport)
#else
#define SHIELD_QUEUE_EXPORT __attribute__((visibility("default")))
#endif

SHIELD_QUEUE_EXPORT
const struct shield_queue_plugin* shield_queue_plugin_api(void);

#ifdef __cplusplus
}
#endif
