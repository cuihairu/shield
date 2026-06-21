// [SHIELD_PLUGIN] Leaderboard plugin C ABI
//
// Stable C interface for leaderboard backends (Redis ZSET, database,
// in-memory sorted set, etc.).
//
// A leaderboard is a ranked list of players by score. Common in games
// for player rankings, event leaderboards, speedrun boards, etc.
//
// Integration with shield_plugin system:
//   type = SHIELD_PLUGIN_TYPE_LEADERBOARD, vtable → shield_leaderboard_plugin*

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_LEADERBOARD_ABI_VERSION 1

struct shield_leaderboard_conn;

struct shield_leaderboard_config {
    const char* host;
    int port;
    const char* password;
    int connect_timeout_ms;
    const char* extra_json;        // driver-specific options
};

// A single leaderboard entry.
struct shield_leaderboard_entry {
    const char* player_id;
    double score;
    int64_t rank;                  // 1-based rank, 0 = not ranked
};

// Result of a leaderboard query.
struct shield_leaderboard_result {
    int success;
    const char* error_code;
    const char* error_msg;
    struct shield_leaderboard_entry* entries;
    int entry_count;
};

struct shield_leaderboard_plugin {
    uint32_t abi_version;
    const char* name;              // "redis", "database", "memory"
    const char* version;

    // Connection
    struct shield_leaderboard_conn* (*connect)(
        const struct shield_leaderboard_config* config,
        char* err_buf, int err_buf_size);
    void (*disconnect)(struct shield_leaderboard_conn* conn);

    // --- Core Operations ------------------------------------------------

    // Add or update a player's score. If player already exists, update
    // the score (higher is better by default).
    // Returns 0 on success, non-zero on error.
    int (*add_score)(struct shield_leaderboard_conn* conn,
                     const char* board_name,
                     const char* player_id,
                     double score);

    // Remove a player from the leaderboard.
    int (*remove)(struct shield_leaderboard_conn* conn,
                  const char* board_name,
                  const char* player_id);

    // Get a player's current score. Returns 0 if player not found.
    int (*get_score)(struct shield_leaderboard_conn* conn,
                     const char* board_name,
                     const char* player_id,
                     double* out_score);

    // --- Ranking Queries ------------------------------------------------

    // Get a player's rank (1-based). Returns 0 if not ranked.
    int (*get_rank)(struct shield_leaderboard_conn* conn,
                    const char* board_name,
                    const char* player_id,
                    int64_t* out_rank);

    // Get top N entries (rank 1..limit).
    // Returns entries in the result struct.
    int (*get_top)(struct shield_leaderboard_conn* conn,
                   const char* board_name,
                   int limit,
                   struct shield_leaderboard_result* out);

    // Get entries around a player (player_rank ± range).
    // Useful for "players near you" display.
    int (*get_around)(struct shield_leaderboard_conn* conn,
                      const char* board_name,
                      const char* player_id,
                      int range,
                      struct shield_leaderboard_result* out);

    // Get a specific range of entries (rank start..end, 1-based).
    int (*get_range)(struct shield_leaderboard_conn* conn,
                     const char* board_name,
                     int64_t start_rank,
                     int64_t end_rank,
                     struct shield_leaderboard_result* out);

    // --- Board Management -----------------------------------------------

    // Delete an entire leaderboard.
    int (*delete_board)(struct shield_leaderboard_conn* conn,
                        const char* board_name);

    // Get the total number of players on a leaderboard.
    int (*count)(struct shield_leaderboard_conn* conn,
                 const char* board_name,
                 int64_t* out_count);

    // Memory
    void (*free_result)(struct shield_leaderboard_result* result);
};

// Entry point exported by every leaderboard plugin DLL.
#ifdef _WIN32
#define SHIELD_LEADERBOARD_EXPORT __declspec(dllexport)
#else
#define SHIELD_LEADERBOARD_EXPORT __attribute__((visibility("default")))
#endif

SHIELD_LEADERBOARD_EXPORT
const struct shield_leaderboard_plugin* shield_leaderboard_plugin_api(void);

#ifdef __cplusplus
}
#endif
