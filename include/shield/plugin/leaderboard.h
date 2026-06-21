// [SHIELD_PLUGIN] shield.leaderboard.v1 interface.
//
// Leaderboard provider with multi-field sorting. connect() yields a handle
// bound to the configured backend (redis ZSET, in-memory, sqlite, ...).
#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_LEADERBOARD_INTERFACE "shield.leaderboard.v1"

struct shield_leaderboard_conn;  // opaque, plugin-defined

enum shield_sort_direction {
    SHIELD_SORT_DESC = 0,  // higher is better
    SHIELD_SORT_ASC  = 1,  // lower is better
};

struct shield_leaderboard_field_def {
    const char* name;
    enum shield_sort_direction dir;
};

struct shield_leaderboard_entry {
    const char* player_id;
    const char* const* field_names;
    const double* field_values;
    int field_count;
    int64_t rank;  // 1-based, 0 = not ranked
};

struct shield_leaderboard_result {
    int success;
    const char* error_code;
    const char* error_msg;
    struct shield_leaderboard_entry* entries;
    int entry_count;
};

struct shield_leaderboard_config {
    const char* name;  // board name
    struct shield_leaderboard_field_def* field_defs;
    int field_count;
    const char* backend;            // "memory"|"redis"|"sqlite"
    const char* backend_config_json;
    int max_entries;
    int auto_save_seconds;
};

struct shield_leaderboard_connect_args {
    const char* host;               // backend host (redis)
    int port;
    const char* password;
    int db;
    int connect_timeout_ms;
    int command_timeout_ms;
    const char* extra_json;
};

struct shield_leaderboard_v1 {
    uint32_t struct_size;
    const char* name;
    const char* version;

    struct shield_leaderboard_conn* (*connect)(
        const struct shield_leaderboard_connect_args* args,
        char* err_buf, int err_buf_size);
    void (*disconnect)(struct shield_leaderboard_conn* conn);

    int (*create_board)(struct shield_leaderboard_conn* conn,
                        const struct shield_leaderboard_config* config,
                        char* err_buf, int err_buf_size);
    int (*delete_board)(struct shield_leaderboard_conn* conn,
                        const char* board_name);

    int (*set_entry)(struct shield_leaderboard_conn* conn,
                     const char* board_name, const char* player_id,
                     const char* const* field_names, const double* field_values,
                     int field_count);
    int (*remove_entry)(struct shield_leaderboard_conn* conn,
                        const char* board_name, const char* player_id);
    int (*get_entry)(struct shield_leaderboard_conn* conn,
                     const char* board_name, const char* player_id,
                     struct shield_leaderboard_entry* out);

    int (*get_rank)(struct shield_leaderboard_conn* conn,
                    const char* board_name, const char* player_id,
                    int64_t* out_rank);
    int (*top_n)(struct shield_leaderboard_conn* conn,
                 const char* board_name, int n,
                 struct shield_leaderboard_result* out);

    void (*free_result)(struct shield_leaderboard_result* result);
    void (*free_entry)(struct shield_leaderboard_entry* entry);
};

#ifdef __cplusplus
}
#endif
