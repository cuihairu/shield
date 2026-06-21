// [SHIELD_PLUGIN] Matchmaking plugin C ABI
//
// Stable C interface for matchmaking backends (ELO, MMR, skill-based,
// party matching, custom algorithms, etc.).
//
// Common in multiplayer games for finding opponents/teammates.
//
// Integration with shield_plugin system:
//   type = SHIELD_PLUGIN_TYPE_MATCHMAKING, vtable → shield_matchmaking_plugin*

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_MATCHMAKING_ABI_VERSION 1

struct shield_matchmaking_conn;

struct shield_matchmaking_config {
    int max_players_per_match;
    int match_timeout_seconds;
    const char* extra_json;        // algorithm-specific config
};

// A player in the matchmaking queue.
struct shield_matchmaking_player {
    const char* player_id;
    double rating;                 // ELO/MMR rating
    const char* party_id;          // NULL if solo
    const char* region;            // e.g. "us-east", "eu-west"
    const char* metadata_json;     // extra player data
};

// A formed match result.
struct shield_matchmaking_match {
    const char* match_id;
    struct shield_matchmaking_player* players;
    int player_count;
    const char* server_address;    // allocated game server, NULL if pending
};

// Matchmaking result.
struct shield_matchmaking_result {
    int success;
    const char* error_code;
    const char* error_msg;
    struct shield_matchmaking_match* matches;
    int match_count;
};

// Match found callback.
typedef void (*shield_matchmaking_on_match)(
    const struct shield_matchmaking_match* match,
    void* user_data);

struct shield_matchmaking_plugin {
    uint32_t abi_version;
    const char* name;              // "elo", "mmr", "custom"
    const char* version;

    // Connection
    struct shield_matchmaking_conn* (*connect)(
        const struct shield_matchmaking_config* config,
        char* err_buf, int err_buf_size);
    void (*disconnect)(struct shield_matchmaking_conn* conn);

    // --- Queue Operations -----------------------------------------------

    // Add a player to the matchmaking queue.
    int (*enqueue)(struct shield_matchmaking_conn* conn,
                   const struct shield_matchmaking_player* player);

    // Remove a player from the queue.
    int (*dequeue)(struct shield_matchmaking_conn* conn,
                   const char* player_id);

    // Check if a player is in the queue.
    int (*is_queued)(struct shield_matchmaking_conn* conn,
                     const char* player_id);

    // --- Match Operations -----------------------------------------------

    // Try to form matches from the current queue.
    // Returns formed matches in the result struct.
    int (*try_match)(struct shield_matchmaking_conn* conn,
                     struct shield_matchmaking_result* out);

    // Update a player's rating after a match.
    int (*update_rating)(struct shield_matchmaking_conn* conn,
                         const char* player_id,
                         double new_rating);

    // Get a player's current rating.
    int (*get_rating)(struct shield_matchmaking_conn* conn,
                      const char* player_id,
                      double* out_rating);

    // --- Statistics -----------------------------------------------------

    // Get the current queue size.
    int (*queue_size)(struct shield_matchmaking_conn* conn,
                      int* out_size);

    // Memory
    void (*free_result)(struct shield_matchmaking_result* result);
};

// Entry point exported by every matchmaking plugin DLL.
#ifdef _WIN32
#define SHIELD_MATCHMAKING_EXPORT __declspec(dllexport)
#else
#define SHIELD_MATCHMAKING_EXPORT __attribute__((visibility("default")))
#endif

SHIELD_MATCHMAKING_EXPORT
const struct shield_matchmaking_plugin* shield_matchmaking_plugin_api(void);

#ifdef __cplusplus
}
#endif
