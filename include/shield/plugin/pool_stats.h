// [SHIELD_PLUGIN] shield.pool.stats.v1 — optional pool observability interface.
//
// A package providing "shield.pool.stats.v1" returns a shield_pool_stats_v1*
// from instance->get_interface("shield.pool.stats.v1"). The host sentinel-fills
// a shield_pool_stats, sets struct_size, and calls get_stats for a snapshot.
//
// See docs/plugin-pool-stats.md (proposal; Phase A of
// docs/superpowers/plans/2026-06-27-pool-stats-v1.md implements ABI + host
// collection + cache.redis validation).
#pragma once

#include <stdint.h>

#include "shield/plugin/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_POOL_STATS_INTERFACE "shield.pool.stats.v1"

// Pool snapshot. See docs/plugin-pool-stats.md for the full field semantics.
//
// ABI versioning: struct_size MUST be the first field. The host sentinel-fills
// the entire struct (gauges/counters = -1 unknown, last_error_epoch_ms = -1
// unknown) and sets struct_size = sizeof(shield_pool_stats) before calling.
// The plugin fills only the prefix it recognizes; fields appended at the tail
// in future versions stay backward compatible (the host's sentinel survives
// when an older plugin leaves them unset).
//
// Unknown semantics: every gauge may be -1 ("unknown / not applicable", e.g.
// a driver that does not expose its idle count); counters use -1 the same
// way. 0 means "zero events", which is distinct from unknown.
struct shield_pool_stats {
    uint32_t struct_size;  // == sizeof(shield_pool_stats); MUST be first

    // Capacity & usage (instantaneous; -1 = unknown)
    int32_t max_size;  // configured pool_size cap; 0 = unbounded, -1 = unknown
    int32_t size;      // connections held = idle + in_use; -1 = unknown
    int32_t idle;      // -1 = unknown
    int32_t in_use;    // -1 = unknown

    // Waiting
    int32_t waiters;                // -1 = unknown
    int64_t acquire_timeout_total;  // -1 = not tracked

    // Lifecycle (cumulative, monotonic; -1 = not tracked)
    int64_t acquire_total;
    int64_t create_total;
    int64_t destroy_total;
    int64_t eviction_total;
    int64_t health_check_failures_total;

    int64_t last_error_epoch_ms;  // 0 = none, -1 = unknown
};

// C++ host helper (like database.h): constexpr interface_name + vtable.
// Struct layout is ABI-stable POD; header is C++ (constexpr), not
// C-includeable.
struct shield_pool_stats_v1 {
    static constexpr const char* interface_name = SHIELD_POOL_STATS_INTERFACE;
    uint32_t struct_size;
    // Fill *out (honoring out->struct_size).
    // 0 = ok; 1 = unavailable; 2 = unsupported_state; >2 = treated as
    // unsupported_state; <0 = internal_error.
    int (*get_stats)(struct shield_plugin_instance_v1* self,
                     struct shield_pool_stats* out);
};

#ifdef __cplusplus
}
#endif
