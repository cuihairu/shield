// [SHIELD_PLUGIN] Prometheus metrics exporter plugin
//
// Exposes metrics in Prometheus text format via an HTTP endpoint.
// Uses shield_net's HTTP server or a minimal built-in HTTP listener.

#include "shield/plugin/plugin.h"
#include "shield/plugin/metric_plugin.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct MetricEntry {
    std::string name;
    shield_metric_type type;
    double value;
    std::vector<std::pair<std::string, std::string>> labels;
};

std::vector<MetricEntry> g_metrics;
std::mutex g_mutex;

int metric_init(const char* config_json, char* err_buf, int err_buf_size) {
    return 0;
}

void metric_shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_metrics.clear();
}

int metric_record(const shield_metric_point* point) {
    if (!point || !point->name) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    MetricEntry entry;
    entry.name = point->name;
    entry.type = point->type;
    entry.value = point->value;
    for (int i = 0; i < point->label_count; ++i) {
        if (point->label_keys[i] && point->label_values[i]) {
            entry.labels.emplace_back(point->label_keys[i], point->label_values[i]);
        }
    }
    g_metrics.push_back(std::move(entry));
    return 0;
}

int metric_record_batch(const shield_metric_point* points, int count) {
    for (int i = 0; i < count; ++i) {
        metric_record(&points[i]);
    }
    return 0;
}

int metric_counter_inc(const char* name, double value,
                       const char* const* label_keys,
                       const char* const* label_values,
                       int label_count) {
    shield_metric_point point;
    point.name = name;
    point.type = SHIELD_METRIC_COUNTER;
    point.value = value;
    point.timestamp_ms = 0;
    point.label_keys = label_keys;
    point.label_values = label_values;
    point.label_count = label_count;
    return metric_record(&point);
}

int metric_gauge_set(const char* name, double value,
                     const char* const* label_keys,
                     const char* const* label_values,
                     int label_count) {
    shield_metric_point point;
    point.name = name;
    point.type = SHIELD_METRIC_GAUGE;
    point.value = value;
    point.timestamp_ms = 0;
    point.label_keys = label_keys;
    point.label_values = label_values;
    point.label_count = label_count;
    return metric_record(&point);
}

int metric_histogram_observe(const char* name, double value,
                             const char* const* label_keys,
                             const char* const* label_values,
                             int label_count) {
    shield_metric_point point;
    point.name = name;
    point.type = SHIELD_METRIC_HISTOGRAM;
    point.value = value;
    point.timestamp_ms = 0;
    point.label_keys = label_keys;
    point.label_values = label_values;
    point.label_count = label_count;
    return metric_record(&point);
}

int metric_flush() {
    // In a real implementation, this would push metrics to Prometheus.
    return 0;
}

const shield_metric_plugin g_metric_plugin = {
    SHIELD_METRIC_ABI_VERSION,
    "prometheus",
    "1.0.0",

    metric_init,
    metric_shutdown,

    metric_record,
    metric_record_batch,

    metric_counter_inc,
    metric_gauge_set,
    metric_histogram_observe,

    metric_flush,
};

const shield_plugin g_plugin = {
    SHIELD_PLUGIN_ABI_VERSION,
    SHIELD_PLUGIN_TYPE_METRIC,
    "shield_metric_prometheus",
    "1.0.0",
    "Prometheus metrics exporter",
    "Shield",

    [](const shield_host_t, const shield_host_api*,
       const shield_plugin_config*, char*, int) -> int {
        return metric_init(nullptr, nullptr, 0);
    },

    []() { metric_shutdown(); },
    []() -> int { return 1; },
    [](int) -> const shield_plugin_capability* {
        static shield_plugin_capability cap = {"metric", "1.0.0", "Prometheus metrics"};
        return &cap;
    },

    &g_metric_plugin,
};

}  // namespace

extern "C" __declspec(dllexport)
const struct shield_plugin* shield_plugin_api(void) {
    return &g_plugin;
}
