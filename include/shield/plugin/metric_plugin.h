// [SHIELD_PLUGIN] Metrics exporter plugin C ABI
//
// Stable C interface for metrics exporters (Prometheus, StatsD, Datadog,
// New Relic, custom, etc.).
//
// Most game backends don't need metrics. This plugin is only loaded when
// explicitly enabled via config.
//
// Integration with shield_plugin system:
//   type = SHIELD_PLUGIN_TYPE_METRIC, vtable → shield_metric_plugin*

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_METRIC_ABI_VERSION 1

// Metric types.
enum shield_metric_type {
    SHIELD_METRIC_COUNTER   = 0,  // Monotonically increasing value
    SHIELD_METRIC_GAUGE     = 1,  // Value that can go up or down
    SHIELD_METRIC_HISTOGRAM = 2,  // Distribution of values
    SHIELD_METRIC_TIMER     = 3,  // Duration measurements
};

// A single metric data point.
struct shield_metric_point {
    const char* name;              // e.g. "http_requests_total"
    enum shield_metric_type type;
    double value;
    int64_t timestamp_ms;          // 0 = use current time
    // Labels (key-value pairs).
    const char* const* label_keys;
    const char* const* label_values;
    int label_count;
};

struct shield_metric_plugin {
    uint32_t abi_version;
    const char* name;              // "prometheus", "statsd", "datadog", "custom"
    const char* version;

    // Initialize the exporter (connect to collector, start HTTP endpoint, etc.)
    int (*init)(const char* config_json, char* err_buf, int err_buf_size);
    void (*shutdown)(void);

    // Record a single metric point.
    int (*record)(const struct shield_metric_point* point);

    // Record multiple metric points in batch.
    int (*record_batch)(const struct shield_metric_point* points, int count);

    // Increment a counter.
    int (*counter_inc)(const char* name, double value,
                       const char* const* label_keys,
                       const char* const* label_values,
                       int label_count);

    // Set a gauge value.
    int (*gauge_set)(const char* name, double value,
                     const char* const* label_keys,
                     const char* const* label_values,
                     int label_count);

    // Observe a histogram/timer value.
    int (*histogram_observe)(const char* name, double value,
                             const char* const* label_keys,
                             const char* const* label_values,
                             int label_count);

    // Flush pending metrics (for batch exporters).
    int (*flush)(void);
};

// Entry point exported by every metric plugin DLL.
#ifdef _WIN32
#define SHIELD_METRIC_EXPORT __declspec(dllexport)
#else
#define SHIELD_METRIC_EXPORT __attribute__((visibility("default")))
#endif

SHIELD_METRIC_EXPORT
const struct shield_metric_plugin* shield_metric_plugin_api(void);

#ifdef __cplusplus
}
#endif
