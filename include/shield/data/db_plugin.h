// [SHIELD_DATA] Database backend plugin C ABI
//
// Stable C interface implemented by each database backend DLL
// (plugins/shield_db_<driver>.dll/.so). The core shield_data library
// loads the appropriate plugin at runtime via shield::data::DatabasePool
// and dispatches calls through this function table.
//
// ABI stability rules:
//   - Appending new function pointers to the END of shield_db_plugin_t
//     is allowed (callers must check abi_version before invoking).
//   - Reordering, removing, or repurposing existing slots is NOT allowed.
//   - All strings crossing the boundary are NULL-terminated UTF-8.
//   - Memory allocated by the plugin must be freed via the plugin's
//     free_result / free_error callbacks. The core never free()s plugin
//     memory directly.
//
// Threading:
//   - Each shield_db_conn_t* is owned by a single thread at a time
//     (the pool serialises handoff).
//   - The plugin may be called from multiple threads concurrently
//     on DIFFERENT connections, but never on the SAME connection.
//   - Plugin-wide operations (init, shutdown) may be serialised by the
//     host; the plugin must document if it needs extra locking.

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define SHIELD_DB_EXPORT __declspec(dllexport)
#else
#define SHIELD_DB_EXPORT __attribute__((visibility("default")))
#endif

// Bump when the layout of shield_db_plugin_t changes in a
// backward-incompatible way. Callers MUST refuse plugins whose
// abi_version differs from SHIELD_DB_ABI_VERSION.
//
// History:
//   1 - initial release
#define SHIELD_DB_ABI_VERSION 1

// Opaque connection handle. The plugin defines the real struct;
// the host treats it as a black pointer.
struct shield_db_conn;

// Connection configuration passed to connect(). The host packs the
// generic database.* config keys here; the plugin reads what it needs
// and ignores the rest. Future-proofed via extra_options_json for
// driver-specific knobs (e.g. SQLite's page_size, MySQL's ssl_mode).
struct shield_db_connect_args {
    const char* host;        // may be NULL (e.g. SQLite uses path)
    int         port;        // 0 = plugin default
    const char* user;        // may be NULL
    const char* password;    // may be NULL
    const char* database;    // database name OR file path (SQLite)
    const char* extra_json;  // NULL or driver-specific JSON, UTF-8

    // Timeouts in milliseconds. 0 = plugin default.
    int connect_timeout_ms;
    int query_timeout_ms;
};

// Result of a query or execute call. Owned by the plugin until the
// host calls free_result. Cells are flat row-major:
//   cell[i * col_count + j]  for i in [0, row_count), j in [0, col_count)
// NULL cell pointer means SQL NULL; empty string ("") is distinct from NULL.
struct shield_db_result {
    int      success;         // 1 = ok, 0 = error (see error_msg/code)
    const char* error_msg;    // NULL or UTF-8 message; plugin-owned
    const char* error_code;   // stable code, e.g. "connection_lost"
    int64_t  affected_rows;   // UPDATE/DELETE count, 0 if N/A
    int64_t  last_insert_id;  // AUTO_INCREMENT/ROWID, 0 if N/A

    int      row_count;
    int      col_count;
    const char** cells;       // size = row_count * col_count; may be NULL
};

// Plugin function table. Pointers are REQUIRED (non-NULL) unless
// explicitly noted. The host checks abi_version first.
struct shield_db_plugin {
    uint32_t   abi_version;   // must equal SHIELD_DB_ABI_VERSION
    const char* name;         // "mysql", "postgresql", "sqlite", ...
    const char* version;      // human-readable plugin version

    // --- Connection lifecycle --------------------------------------

    // Create a new connected handle. On failure returns NULL and
    // optionally writes a message into err_buf (NULL-terminated).
    struct shield_db_conn* (*connect)(
        const struct shield_db_connect_args* args,
        char* err_buf, int err_buf_size);

    // Destroy a handle. Safe to call with NULL (no-op).
    void (*disconnect)(struct shield_db_conn* conn);

    // Lightweight liveness check. Returns 1 if alive, 0 otherwise.
    int (*ping)(struct shield_db_conn* conn);

    // --- SQL operations --------------------------------------------
    //
    // All return 0 on success (result.success may still be 0 for SQL
    // errors - that's a query-level failure, not a hard error).
    // Return non-zero if the plugin hit a transport/protocol error
    // and the host should treat the connection as poisoned.

    int (*query)(
        struct shield_db_conn* conn,
        const char* sql,
        const char* const* params,   // may be NULL if n_params == 0
        int n_params,
        struct shield_db_result* out_result);

    int (*execute)(
        struct shield_db_conn* conn,
        const char* sql,
        const char* const* params,
        int n_params,
        struct shield_db_result* out_result);

    // --- Transactions ----------------------------------------------
    //
    // All three fill out_result->success = 1 on commit ok.

    int (*begin)(struct shield_db_conn* conn,
                 struct shield_db_result* out_result);
    int (*commit)(struct shield_db_conn* conn,
                  struct shield_db_result* out_result);
    int (*rollback)(struct shield_db_conn* conn,
                    struct shield_db_result* out_result);

    // --- Memory ----------------------------------------------------

    // Release any plugin-allocated memory referenced by *result
    // (error_msg, error_code, cells, cell strings). Does not free
    // `result` itself (host owns the struct).
    void (*free_result)(struct shield_db_result* result);
};

// Entry point exported by every backend DLL. Returns a pointer to a
// static, plugin-owned shield_db_plugin_t. The pointer is valid for
// the lifetime of the loaded module.
//
// If the plugin cannot initialise (e.g. missing system deps), it MAY
// return NULL. The host treats this as "driver unavailable".
SHIELD_DB_EXPORT
const struct shield_db_plugin* shield_db_plugin_api(void);

#ifdef __cplusplus
}  // extern "C"
#endif
