// [SHIELD_PLUGIN] shield.metrics.v1 interface.
//
// Metrics exporter provider. connect() initializes the exporter (open HTTP
// endpoint, connect to collector) and returns a session handle.
#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_METRICS_INTERFACE "shield.metrics.v1"

struct shield_metrics_session;  // opaque, plugin-defined

enum shield_metric_type {
    SHIELD_METRIC_COUNTER   = 0,
    SHIELD_METRIC_GAUGE     = 1,
    SHIELD_METRIC_HISTOGRAM = 2,
    SHIELD_METRIC_TIMER     = 3,
};

struct shield_metric_point {
    const char* name;
    enum shield_metric_type type;
    double value;
    const char* const* label_keys;
    const char* const* label_values;
    int label_count;
    int64_t timestamp_ms;  // 0 = now
};

struct shield_metrics_config {
    const char* bind_address;    // HTTP endpoint (prometheus), NULL = none
    int port;
    const char* path;            // "/metrics"
    int push_interval_seconds;   // 0 = pull only
    const char* extra_json;
};

struct shield_metrics_v1 {
    static constexpr const char* interface_name = SHIELD_METRICS_INTERFACE;

    uint32_t struct_size;
    const char* name;            // "prometheus" | "statsd" | ...
    const char* version;

    struct shield_metrics_session* (*connect)(
        const struct shield_metrics_config* cfg,
        char* err_buf, int err_buf_size);
    void (*disconnect)(struct shield_metrics_session* session);

    int (*record)(struct shield_metrics_session* session,
                  const struct shield_metric_point* point);
    int (*record_batch)(struct shield_metrics_session* session,
                        const struct shield_metric_point* points, int count);
    int (*counter_inc)(struct shield_metrics_session* session,
                       const char* name, double value,
                       const char* const* label_keys,
                       const char* const* label_values, int label_count);
    int (*gauge_set)(struct shield_metrics_session* session,
                     const char* name, double value,
                     const char* const* label_keys,
                     const char* const* label_values, int label_count);
    int (*histogram_observe)(struct shield_metrics_session* session,
                             const char* name, double value,
                             const char* const* label_keys,
                             const char* const* label_values, int label_count);
    int (*flush)(struct shield_metrics_session* session);
};

#ifdef __cplusplus
}
#endif
