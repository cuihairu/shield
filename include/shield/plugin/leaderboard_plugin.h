// [SHIELD_PLUGIN] Leaderboard plugin C ABI
//
// High-level leaderboard abstraction. Depends on a Redis plugin for
// sorted set operations.
//
// Usage:
//   const shield_plugin* p = host_api->find_plugin(host, "shield_leaderboard");
//   const shield_leaderboard_plugin* lb = p->vtable;
//   lb->add_score("global", "player:123", 1500.0);
//   lb->get_top("global", 10, &result);

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_LEADERBOARD_ABI_VERSION 1

struct shield_leaderboard_entry {
    const char* player_id;
    double score;
    int64_t rank;                  // 1-based, 0 = not ranked
};

struct shield_leaderboard_result {
    int success;
    const char* error_code;
    const char* error_msg;
    struct shield_leaderboard_entry* entries;
    int entry_count;
};

struct shield_leaderboard_plugin {
    uint32_t abi_version;
    const char* name;
    const char* version;

    // Initialize with a reference to the Redis plugin.
    int (*init)(const void* redis_plugin, char* err_buf, int err_buf_size);
    void (*shutdown)(void);

    // Add or update a player's score on a named leaderboard.
    int (*add_score)(const char* board_name, const char* player_id, double score);

    // Remove a player from a leaderboard.
    int (*remove)(const char* board_name, const char* player_id);

    // Get a player's score.
    int (*get_score)(const char* board_name, const char* player_id, double* out);

    // Get a player's rank (1-based, 0 = not ranked).
    int (*get_rank)(const char* board_name, const char* player_id, int64_t* out);

    // Get top N entries (rank 1..limit).
    int (*get_top)(const char* board_name, int limit,
                   struct shield_leaderboard_result* out);

    // Get entries around a player (player_rank ± range).
    int (*get_around)(const char* board_name, const char* player_id,
                      int range, struct shield_leaderboard_result* out);

    // Get a range of entries (rank start..end, 1-based).
    int (*get_range)(const char* board_name, int64_t start, int64_t end,
                     struct shield_leaderboard_result* out);

    // Delete an entire leaderboard.
    int (*delete_board)(const char* board_name);

    // Get total player count on a leaderboard.
    int (*count)(const char* board_name, int64_t* out);

    // Memory
    void (*free_result)(struct shield_leaderboard_result* result);
};

#ifdef __cplusplus
}
#endif
