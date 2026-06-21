// [SHIELD_PLUGIN] Leaderboard plugin C ABI
//
// High-level leaderboard abstraction supporting multi-field sorting.
//
// Storage backends:
//   - Redis ZSET: distributed, single-score only
//   - In-memory Skip List: fast, multi-field, no persistence
//   - SQLite: persistent, multi-field, queryable
//   - JSON file: simple persistence for small leaderboards
//
// Multi-field sorting:
//   The leaderboard supports sorting by multiple fields. Fields are
//   compared in order of priority. Example:
//     field_defs = [{name="score", desc=true}, {name="level", desc=true}, {name="updated_at", desc=false}]
//   This sorts by score DESC, then level DESC, then updated_at ASC.

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_LEADERBOARD_ABI_VERSION 1

// Sort direction for a field.
enum shield_sort_direction {
    SHIELD_SORT_DESC = 0,  // Higher is better (default for scores)
    SHIELD_SORT_ASC  = 1,  // Lower is better (for timestamps, completion time)
};

// Field definition for multi-field sorting.
struct shield_leaderboard_field_def {
    const char* name;                // field name (e.g. "score", "level")
    enum shield_sort_direction dir;  // sort direction
};

// A single leaderboard entry with multiple fields.
struct shield_leaderboard_entry {
    const char* player_id;
    // Fields as key-value pairs (parallel arrays).
    const char* const* field_names;
    const double* field_values;
    int field_count;
    // Computed rank (1-based, 0 = not ranked).
    int64_t rank;
};

// Query result.
struct shield_leaderboard_result {
    int success;
    const char* error_code;
    const char* error_msg;
    struct shield_leaderboard_entry* entries;
    int entry_count;
};

// Leaderboard configuration.
struct shield_leaderboard_config {
    const char* name;                // leaderboard name (e.g. "global", "weekly")
    // Field definitions for sorting.
    struct shield_leaderboard_field_def* field_defs;
    int field_count;
    // Storage backend: "memory", "redis", "sqlite", "json"
    const char* backend;
    // Backend-specific config (JSON string).
    const char* backend_config_json;
    // Max entries (0 = unlimited).
    int max_entries;
    // Persistence: auto-save interval in seconds (0 = manual save only).
    int auto_save_seconds;
};

struct shield_leaderboard_plugin {
    uint32_t abi_version;
    const char* name;
    const char* version;

    // --- Lifecycle ------------------------------------------------------

    // Initialize the plugin.
    int (*init)(const void* host, const void* host_api,
                char* err_buf, int err_buf_size);
    void (*shutdown)(void);

    // --- Board Management -----------------------------------------------

    // Create a leaderboard with the given config.
    int (*create_board)(const struct shield_leaderboard_config* config,
                        char* err_buf, int err_buf_size);

    // Delete a leaderboard.
    int (*delete_board)(const char* board_name);

    // --- Entry Operations -----------------------------------------------

    // Add or update a player's entry. Fields are passed as parallel arrays.
    int (*set_entry)(const char* board_name,
                     const char* player_id,
                     const char* const* field_names,
                     const double* field_values,
                     int field_count);

    // Remove a player from the leaderboard.
    int (*remove_entry)(const char* board_name, const char* player_id);

    // Get a player's entry (all fields + rank).
    int (*get_entry)(const char* board_name, const char* player_id,
                     struct shield_leaderboard_entry* out);

    // --- Ranking Queries ------------------------------------------------

    // Get a player's rank (1-based, 0 = not ranked).
    int (*get_rank)(const char* board_name, const char* player_id,
                    int64_t* out_rank);

    // Get top N entries.
    int (*get_top)(const char* board_name, int limit,
                   struct shield_leaderboard_result* out);

    // Get entries around a player (player_rank ± range).
    int (*get_around)(const char* board_name, const char* player_id,
                      int range, struct shield_leaderboard_result* out);

    // Get a range of entries (rank start..end, 1-based).
    int (*get_range)(const char* board_name, int64_t start, int64_t end,
                     struct shield_leaderboard_result* out);

    // --- Statistics -----------------------------------------------------

    // Get total entry count.
    int (*count)(const char* board_name, int64_t* out_count);

    // --- Persistence ----------------------------------------------------

    // Save leaderboard to backend (for JSON/SQLite backends).
    int (*save)(const char* board_name);

    // Load leaderboard from backend.
    int (*load)(const char* board_name);

    // --- Memory ---------------------------------------------------------
    void (*free_result)(struct shield_leaderboard_result* result);
};

#ifdef __cplusplus
}
#endif
