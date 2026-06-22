// [SHIELD_PLUGIN] shield.document.v1 interface.
//
// Document database provider interface. Designed for MongoDB-class drivers:
// JSON-in / JSON-out over a small, explicit C vtable. BSON / driver-specific
// types never cross the ABI boundary — every document, filter, update, or
// pipeline is a UTF-8 JSON string.
//
// A package providing "shield.document.v1" returns a shield_document_v1*
// from instance->get_interface("shield.document.v1"). The plugin owns its
// own connection pool (per-instance); this vtable is the per-driver
// connection factory + document surface.
//
// Why JSON over the C ABI instead of a BSON view?
//   - Stable ABI: BSON layout changes (or driver upgrades) never break the
//     vtable. The host and any C-ABI consumer parse JSON generically.
//   - Cross-language friendliness: a future Python or Lua host can call
//     this vtable without a BSON binding.
//   - Zero type leakage: nlohmann::json on the host, bsoncxx on the plugin
//     side, and the boundary stays a string.

#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Interface name a document provider must hand to get_interface().
#define SHIELD_DOCUMENT_INTERFACE "shield.document.v1"

// Opaque connection handle. The plugin defines the real struct; the host
// treats it as a black pointer.
struct shield_doc_conn;

// Connection parameters. The host packs the instance config (uri / database
// + driver-specific extras) here. The plugin reads what it needs and
// ignores the rest.
//
// `uri` is the canonical connection form, e.g.
//   "mongodb://user:pass@host:port/db?options"
// `database` selects the logical database; `extra_json` carries any
// driver-specific knobs (readConcern, writeConcern, appName, ...) as a JSON
// object string.
struct shield_doc_connect_args {
    const char* uri;        // may be NULL, plugin falls back to defaults
    const char* database;   // logical database name; may be NULL
    const char* extra_json; // NULL or driver-specific JSON, UTF-8

    // Timeouts in milliseconds. 0 = plugin default.
    int connect_timeout_ms;
    int socket_timeout_ms;

    // Pool size hint. 0 = plugin default. The pool lifecycle is owned by
    // the plugin; this is only a sizing knob.
    int pool_size;
};

// Single-write result (insert/update/delete).
//
// `inserted_id_json` / `upserted_id_json` are JSON-encoded _id values
// (string, number, or {"$oid": "..."} object). NULL when not applicable.
// Plugin-owned; released via free_result.
struct shield_doc_result {
    int         success;        // 1 = ok, 0 = error (see error_msg/code)
    const char* error_msg;      // NULL or UTF-8; plugin-owned
    const char* error_code;     // stable code, e.g. "connection_lost"

    int64_t     matched_count;   // update/delete filter matches
    int64_t     modified_count;  // actual document modifications
    int64_t     inserted_count;  // insert_one/insert_many total

    char*       inserted_id_json; // insert_one: the _id of the new doc
    char*       upserted_id_json; // update with upsert: the new _id
};

// Multi-read result (find / aggregate).
//
// `docs_json` is a JSON array string of documents:
//   [{"_id": {"$oid": "..."}, "name": "alice"}, {...}, ...]
// NULL only on hard error (success == 0). Empty result is "[]".
struct shield_doc_cursor {
    int         success;
    const char* error_msg;
    const char* error_code;

    const char* docs_json;     // JSON array string; plugin-owned
    int64_t     matched_count; // count(*) equivalent; -1 if unknown
};

// Document provider vtable. Pointers are REQUIRED (non-NULL).
struct shield_document_v1 {
    static constexpr const char* interface_name = SHIELD_DOCUMENT_INTERFACE;

    uint32_t    struct_size;    // == sizeof(shield_document_v1)
    const char* name;           // "mongodb" | ...
    const char* version;        // human-readable plugin version

    // --- Connection lifecycle --------------------------------------

    // Create a new connected handle. On failure returns NULL and optionally
    // writes a message into err_buf (NULL-terminated).
    struct shield_doc_conn* (*connect)(const struct shield_doc_connect_args* args,
                                       char* err_buf, int err_buf_size);

    // Destroy a handle. Safe to call with NULL (no-op).
    void (*disconnect)(struct shield_doc_conn* conn);

    // Lightweight liveness check. Returns 1 if alive, 0 otherwise.
    int (*ping)(struct shield_doc_conn* conn);

    // --- CRUD ------------------------------------------------------
    //
    // All return 0 on transport/protocol success (result/cursor.success may
    // still be 0 for driver-level errors — that's a query failure, not a
    // hard error). Non-zero return means the connection is poisoned.
    //
    // `filter_json` / `doc_json` / `update_json` / `pipeline_json_array`
    // are UTF-8 JSON strings; NULL is treated as "{}".
    // `opts_json` may carry driver-specific options (projection, sort,
    // limit, skip, collation, ...); NULL means "no options".

    int (*find)(struct shield_doc_conn* conn,
                const char* collection,
                const char* filter_json,
                const char* opts_json,
                struct shield_doc_cursor* out);

    int (*find_one)(struct shield_doc_conn* conn,
                    const char* collection,
                    const char* filter_json,
                    const char* opts_json,
                // single document as JSON object string, or NULL if no match
                    char** out_doc_json,
                    struct shield_doc_result* out);

    int (*insert_one)(struct shield_doc_conn* conn,
                      const char* collection,
                      const char* doc_json,
                      struct shield_doc_result* out);

    int (*insert_many)(struct shield_doc_conn* conn,
                       const char* collection,
                       const char* docs_json_array,
                       struct shield_doc_result* out);

    int (*update_one)(struct shield_doc_conn* conn,
                      const char* collection,
                      const char* filter_json,
                      const char* update_json,
                      const char* opts_json,  // may carry {upsert: true}
                      struct shield_doc_result* out);

    int (*update_many)(struct shield_doc_conn* conn,
                       const char* collection,
                       const char* filter_json,
                       const char* update_json,
                       struct shield_doc_result* out);

    int (*delete_one)(struct shield_doc_conn* conn,
                      const char* collection,
                      const char* filter_json,
                      struct shield_doc_result* out);

    int (*delete_many)(struct shield_doc_conn* conn,
                       const char* collection,
                       const char* filter_json,
                       struct shield_doc_result* out);

    int (*count)(struct shield_doc_conn* conn,
                 const char* collection,
                 const char* filter_json,
                 const char* opts_json,
                 int64_t* out_count);

    // --- Aggregation -----------------------------------------------

    int (*aggregate)(struct shield_doc_conn* conn,
                     const char* collection,
                     const char* pipeline_json_array,
                     const char* opts_json,
                     struct shield_doc_cursor* out);

    // --- Indexes ---------------------------------------------------

    int (*create_index)(struct shield_doc_conn* conn,
                        const char* collection,
                        const char* keys_json,    // {"field": 1} or {"a.b": -1}
                        const char* opts_json,    // {name, unique, ...}
                        struct shield_doc_result* out);

    int (*drop_index)(struct shield_doc_conn* conn,
                      const char* collection,
                      const char* index_name,
                      struct shield_doc_result* out);

    // --- Transactions (MongoDB 4.0+, replica set required) ---------

    int (*begin)(struct shield_doc_conn* conn,
                 struct shield_doc_result* out);
    int (*commit)(struct shield_doc_conn* conn,
                  struct shield_doc_result* out);
    int (*rollback)(struct shield_doc_conn* conn,
                    struct shield_doc_result* out);

    // --- Memory ----------------------------------------------------

    // Release plugin-allocated memory referenced by *cursor (docs_json,
    // error buffers). Does not free `cursor` itself.
    void (*free_cursor)(struct shield_doc_cursor* cursor);

    // Release plugin-allocated memory referenced by *result (inserted_id,
    // upserted_id, error buffers). Does not free `result` itself.
    void (*free_result)(struct shield_doc_result* result);
};

#ifdef __cplusplus
}  // extern "C"
#endif
