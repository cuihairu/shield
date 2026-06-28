// [SHIELD_PLUGIN] shield.database.v1 interface.
//
// SQL database provider interface. Function signatures are inherited
// verbatim from the legacy shield_db_plugin so plugins written against
// the pre-v1 ABI migrate by renaming the type only.
//
// A package providing "shield.database.v1" returns a shield_database_v1*
// from instance->get_interface("shield.database.v1"). The plugin owns
// its own connection pool (per-instance); this vtable is the per-driver
// connection factory + SQL surface.

#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Interface name a database provider must hand to get_interface().
#define SHIELD_DATABASE_INTERFACE "shield.database.v1"

// Opaque connection handle. The plugin defines the real struct; the host
// treats it as a black pointer.
struct shield_db_conn;

// Connection parameters. The host packs the instance config (host/port/
// database/user/password + driver-specific extras) here. The plugin reads
// what it needs and ignores the rest.
struct shield_db_connect_args {
    const char* host;        // may be NULL (e.g. sqlite uses file path)
    int         port;        // 0 = plugin default
    const char* user;        // may be NULL
    const char* password;    // may be NULL
    const char* database;    // database name OR file path (sqlite)
    const char* extra_json;  // NULL or driver-specific JSON, UTF-8

    // Timeouts in milliseconds. 0 = plugin default.
    int connect_timeout_ms;
    int query_timeout_ms;
};

// Result of a query/execute call. Owned by the plugin until the host calls
// free_result. Cells are flat row-major: cell[i * col_count + j].
// NULL cell pointer means SQL NULL; empty string ("") is distinct from NULL.
struct shield_db_result {
    int         success;        // 1 = ok, 0 = error (see error_msg/code)
    const char* error_msg;      // NULL or UTF-8; plugin-owned
    const char* error_code;     // stable code, e.g. "connection_lost"
    int64_t     affected_rows;  // UPDATE/DELETE count, 0 if N/A
    int64_t     last_insert_id; // AUTO_INCREMENT/ROWID, 0 if N/A

    int          row_count;
    int          col_count;
    const char** cells;         // size = row_count * col_count; may be NULL
};

// Database provider vtable. Pointers are REQUIRED (non-NULL).
struct shield_database_v1 {
    uint32_t    struct_size;    // == sizeof(shield_database_v1)
    const char* interface_name; // "shield.database.v1"
    const char* name;           // "sqlite" | "mysql" | "postgresql" | ...
    const char* version;        // human-readable plugin version

    // --- Connection lifecycle --------------------------------------

    // Create a new connected handle. On failure returns NULL and optionally
    // writes a message into err_buf (NULL-terminated).
    struct shield_db_conn* (*connect)(const struct shield_db_connect_args* args,
                                      char* err_buf, int err_buf_size);

    // Destroy a handle. Safe to call with NULL (no-op).
    void (*disconnect)(struct shield_db_conn* conn);

    // Lightweight liveness check. Returns 1 if alive, 0 otherwise.
    int (*ping)(struct shield_db_conn* conn);

    // --- SQL operations --------------------------------------------
    //
    // All return 0 on success (result.success may still be 0 for SQL-level
    // errors — that's a query failure, not a hard error). Return non-zero
    // only on transport/protocol errors so the host can poison the conn.

    int (*query)(struct shield_db_conn* conn,
                 const char* sql,
                 const char* const* params,   // may be NULL if n_params == 0
                 int n_params,
                 struct shield_db_result* out_result);

    int (*execute)(struct shield_db_conn* conn,
                   const char* sql,
                   const char* const* params,
                   int n_params,
                   struct shield_db_result* out_result);

    // --- Transactions ----------------------------------------------

    int (*begin)(struct shield_db_conn* conn,
                 struct shield_db_result* out_result);
    int (*commit)(struct shield_db_conn* conn,
                  struct shield_db_result* out_result);
    int (*rollback)(struct shield_db_conn* conn,
                    struct shield_db_result* out_result);

    // --- Memory ----------------------------------------------------

    // Release plugin-allocated memory referenced by *result (error_msg,
    // error_code, cells, cell strings). Does not free `result` itself.
    void (*free_result)(struct shield_db_result* result);
};

#ifdef __cplusplus
}  // extern "C"
#endif
