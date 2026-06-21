// [SHIELD_PLUGIN] shield.queue.v1 interface.
//
// Message queue / pub-sub provider. connect() yields a handle bound to the
// configured transport (e.g. redis pub/sub).
#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_QUEUE_INTERFACE "shield.queue.v1"

struct shield_queue_conn;  // opaque, plugin-defined

struct shield_queue_config {
    const char* host;
    int port;
    const char* password;
    int db;
    int connect_timeout_ms;
    int command_timeout_ms;
    const char* extra_json;
};

typedef void (*shield_queue_on_message)(const char* channel,
                                        const char* data,
                                        int data_len,
                                        void* user_data);

struct shield_queue_v1 {
    uint32_t struct_size;
    const char* name;
    const char* version;

    struct shield_queue_conn* (*connect)(const struct shield_queue_config* cfg,
                                         char* err_buf, int err_buf_size);
    void (*disconnect)(struct shield_queue_conn* conn);

    int (*publish)(struct shield_queue_conn* conn, const char* channel,
                   const char* data, int data_len);
    int (*subscribe)(struct shield_queue_conn* conn, const char* channel,
                     shield_queue_on_message callback, void* user_data);
    int (*unsubscribe)(struct shield_queue_conn* conn, const char* channel);
};

#ifdef __cplusplus
}
#endif
