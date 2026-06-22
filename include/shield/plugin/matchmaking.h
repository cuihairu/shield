// [SHIELD_PLUGIN] shield.matchmaking.v1 interface.
//
// NOTE: matchmaking is not in the first batch of 7 interfaces from
// docs/plugin-system.md. This header is provided so the existing elo plugin
// migrates to the new ABI; it carries the same conn-handle shape.
#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_MATCHMAKING_INTERFACE "shield.matchmaking.v1"

struct shield_matchmaking_conn;  // opaque, plugin-defined

struct shield_matchmaking_config {
    int max_players_per_match;
    int match_timeout_seconds;
    const char* extra_json;
};

struct shield_matchmaking_player {
    const char* player_id;
    double rating;
    const char* party_id;       // NULL if solo
    const char* region;
    const char* metadata_json;
};

struct shield_matchmaking_match {
    const char* match_id;
    struct shield_matchmaking_player* players;
    int player_count;
    const char* server_address;
};

struct shield_matchmaking_result {
    int success;
    const char* error_code;
    const char* error_msg;
    struct shield_matchmaking_match* matches;
    int match_count;
};

struct shield_matchmaking_v1 {
    static constexpr const char* interface_name = SHIELD_MATCHMAKING_INTERFACE;

    uint32_t struct_size;
    const char* name;
    const char* version;

    struct shield_matchmaking_conn* (*connect)(
        const struct shield_matchmaking_config* cfg,
        char* err_buf, int err_buf_size);
    void (*disconnect)(struct shield_matchmaking_conn* conn);

    int (*enqueue)(struct shield_matchmaking_conn* conn,
                   const struct shield_matchmaking_player* player);
    int (*dequeue)(struct shield_matchmaking_conn* conn,
                   const char* player_id);
    int (*is_queued)(struct shield_matchmaking_conn* conn,
                     const char* player_id);
    int (*try_match)(struct shield_matchmaking_conn* conn,
                     struct shield_matchmaking_result* out);
    int (*update_rating)(struct shield_matchmaking_conn* conn,
                         const char* player_id, double new_rating);
    int (*get_rating)(struct shield_matchmaking_conn* conn,
                      const char* player_id, double* out_rating);
    int (*queue_size)(struct shield_matchmaking_conn* conn, int* out_size);

    void (*free_result)(struct shield_matchmaking_result* result);
};

#ifdef __cplusplus
}
#endif
