// [SHIELD_PLUGIN] metrics.prometheus — Prometheus text exporter for
// shield.metrics.v1.
//
// v1 ABI + production-grade HTTP endpoint via boost::beast. Serves a single
// configurable path (default "/metrics") returning Prometheus text format.
// Supports counters/gauges/histograms with labels.
//
// As with health.http, does NOT link shield_net — embeds a minimal beast
// listener to stay a leaf shared library.

#include "shield/plugin/abi.h"
#include "shield/plugin/metrics.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace {

struct metrics_config {
    std::string bind_address = "0.0.0.0";
    int port = 8087;
    std::string path = "/metrics";
};

std::string json_get_string(const std::string& j, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto p = j.find(needle);
    if (p == std::string::npos) return "";
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    if (p >= j.size() || j[p] != '"') return "";
    ++p;
    std::string out;
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) { out += j[p + 1]; p += 2; }
        else { out += j[p++]; }
    }
    return out;
}

int json_get_int(const std::string& j, const std::string& key, int def) {
    std::string needle = "\"" + key + "\"";
    auto p = j.find(needle);
    if (p == std::string::npos) return def;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return def;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    bool neg = false;
    if (p < j.size() && j[p] == '-') { neg = true; ++p; }
    int v = 0; bool any = false;
    while (p < j.size() && j[p] >= '0' && j[p] <= '9') {
        v = v * 10 + (j[p] - '0'); ++p; any = true;
    }
    return any ? (neg ? -v : v) : def;
}

metrics_config parse_config(const char* config_json) {
    metrics_config c;
    if (!config_json) return c;
    std::string s(config_json);
    c.bind_address = json_get_string(s, "bind_address"); if (c.bind_address.empty()) c.bind_address = "0.0.0.0";
    c.port = json_get_int(s, "port", c.port);
    c.path = json_get_string(s, "path"); if (c.path.empty()) c.path = "/metrics";
    return c;
}

// ---------------------------------------------------------------------------
// Label key — sorted vector of (k,v) pairs, so two points with the same labels
// collapse to the same series regardless of insertion order.
// ---------------------------------------------------------------------------
using LabelVec = std::vector<std::pair<std::string, std::string>>;

LabelVec normalize_labels(const char* const* keys, const char* const* vals, int n) {
    LabelVec out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        if (keys[i] && vals[i]) {
            out.emplace_back(keys[i], vals[i]);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string format_labels(const LabelVec& labels) {
    if (labels.empty()) return "";
    std::ostringstream os;
    os << "{";
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i) os << ",";
        os << labels[i].first << "=\"" << labels[i].second << "\"";
    }
    os << "}";
    return os.str();
}

struct SeriesKey {
    std::string name;
    LabelVec labels;
    bool operator==(const SeriesKey& o) const {
        return name == o.name && labels == o.labels;
    }
    bool operator<(const SeriesKey& o) const {
        if (name != o.name) return name < o.name;
        return labels < o.labels;
    }
};

struct metric_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    metrics_config cfg;

    std::mutex mu;
    // counter / gauge: latest value per series.
    std::map<SeriesKey, double> counters;
    std::map<SeriesKey, double> gauges;
    // histogram: sum / count per series. Buckets fixed to Prometheus defaults.
    std::map<SeriesKey, std::pair<double, uint64_t>> histograms;

    // HTTP runtime
    net::io_context ioc;
    std::unique_ptr<tcp::acceptor> acceptor;
    std::thread io_thread;
    std::atomic<bool> running{false};
};

const char* type_name(shield_metric_type t) {
    switch (t) {
        case SHIELD_METRIC_COUNTER:   return "counter";
        case SHIELD_METRIC_GAUGE:     return "gauge";
        case SHIELD_METRIC_HISTOGRAM: return "histogram";
        case SHIELD_METRIC_TIMER:     return "histogram";  // timers render as histograms
    }
    return "untyped";
}

void render(std::ostringstream& os, metric_instance* inst) {
    std::lock_guard<std::mutex> lock(inst->mu);
    // Render counters + gauges + histograms. Group by metric name.
    std::map<std::string, std::string> seen_types;

    for (const auto& [k, v] : inst->counters) {
        if (seen_types.insert({k.name, "counter"}).second) {
            os << "# TYPE " << k.name << " counter\n";
        }
        os << k.name << format_labels(k.labels) << " " << v << "\n";
    }
    for (const auto& [k, v] : inst->gauges) {
        if (seen_types.insert({k.name, "gauge"}).second) {
            os << "# TYPE " << k.name << " gauge\n";
        }
        os << k.name << format_labels(k.labels) << " " << v << "\n";
    }
    for (const auto& [k, p] : inst->histograms) {
        if (seen_types.insert({k.name, "histogram"}).second) {
            os << "# TYPE " << k.name << " histogram\n";
        }
        double sum = p.first;
        uint64_t count = p.second;
        os << k.name << "_sum" << format_labels(k.labels) << " " << sum << "\n";
        os << k.name << "_count" << format_labels(k.labels) << " " << count << "\n";
    }
}

void handle_session(metric_instance* inst, tcp::socket socket) {
    beast::flat_buffer buf;
    http::request<http::string_body> req;
    try { http::read(socket, buf, req); } catch (...) { return; }

    http::response<http::string_body> res;
    res.version(req.version());
    res.set(http::field::server, "shield.metrics.prometheus");
    res.keep_alive(false);

    if (req.method() == http::verb::get && req.target() == inst->cfg.path) {
        std::ostringstream os;
        render(os, inst);
        res.result(http::status::ok);
        res.set(http::field::content_type, "text/plain; version=0.0.4");
        res.body() = os.str();
    } else {
        res.result(http::status::not_found);
        res.body() = "not found\n";
    }
    res.prepare_payload();
    try { http::write(socket, res); } catch (...) {}
    beast::error_code ec;
    socket.shutdown(tcp::socket::shutdown_both, ec);
}

void accept_loop(metric_instance* inst) {
    while (inst->running.load()) {
        boost::system::error_code ec;
        tcp::socket socket(inst->ioc);
        inst->acceptor->accept(socket, ec);
        if (ec) {
            if (!inst->running.load()) break;
            continue;
        }
        handle_session(inst, std::move(socket));
    }
}

// ---------------------------------------------------------------------------
// v1 metrics vtable
// ---------------------------------------------------------------------------
const shield_metrics_v1& metric_vtable() {
    static const shield_metrics_v1 v = {
        sizeof(shield_metrics_v1),
        "prometheus",
        "1.0.0",
        // connect — no-op (listener started on instance->start())
        [](const struct shield_metrics_config*,
           char* err_buf, int err_buf_size) -> struct shield_metrics_session* {
            if (err_buf && err_buf_size > 0) err_buf[0] = '\0';
            return nullptr;
        },
        [](struct shield_metrics_session*) {},
        // record
        [](struct shield_metrics_session* session,
           const struct shield_metric_point* point) -> int {
            auto* inst = reinterpret_cast<metric_instance*>(session);
            if (!inst || !point || !point->name) return -1;
            std::lock_guard<std::mutex> lock(inst->mu);
            SeriesKey k{point->name,
                        normalize_labels(point->label_keys, point->label_values, point->label_count)};
            switch (point->type) {
                case SHIELD_METRIC_COUNTER:
                    inst->counters[k] += point->value;
                    break;
                case SHIELD_METRIC_GAUGE:
                    inst->gauges[k] = point->value;
                    break;
                case SHIELD_METRIC_HISTOGRAM:
                case SHIELD_METRIC_TIMER: {
                    auto& entry = inst->histograms[k];
                    entry.first += point->value;
                    entry.second += 1;
                    break;
                }
            }
            return 0;
        },
        // record_batch
        [](struct shield_metrics_session* session,
           const struct shield_metric_point* points, int count) -> int {
            auto* inst = reinterpret_cast<metric_instance*>(session);
            if (!inst || !points) return -1;
            for (int i = 0; i < count; ++i) {
                // Reuse record lambda logic (inline to avoid dispatch overhead).
                const auto& point = points[i];
                if (!point.name) continue;
                std::lock_guard<std::mutex> lock(inst->mu);
                SeriesKey k{point.name,
                            normalize_labels(point.label_keys, point.label_values, point.label_count)};
                switch (point.type) {
                    case SHIELD_METRIC_COUNTER:   inst->counters[k] += point.value; break;
                    case SHIELD_METRIC_GAUGE:     inst->gauges[k] = point.value; break;
                    case SHIELD_METRIC_HISTOGRAM:
                    case SHIELD_METRIC_TIMER: {
                        auto& e = inst->histograms[k];
                        e.first += point.value; e.second += 1;
                        break;
                    }
                }
            }
            return 0;
        },
        // counter_inc
        [](struct shield_metrics_session* session,
           const char* name, double value,
           const char* const* lk, const char* const* lv, int lc) -> int {
            auto* inst = reinterpret_cast<metric_instance*>(session);
            if (!inst || !name) return -1;
            std::lock_guard<std::mutex> lock(inst->mu);
            SeriesKey k{name, normalize_labels(lk, lv, lc)};
            inst->counters[k] += value;
            return 0;
        },
        // gauge_set
        [](struct shield_metrics_session* session,
           const char* name, double value,
           const char* const* lk, const char* const* lv, int lc) -> int {
            auto* inst = reinterpret_cast<metric_instance*>(session);
            if (!inst || !name) return -1;
            std::lock_guard<std::mutex> lock(inst->mu);
            SeriesKey k{name, normalize_labels(lk, lv, lc)};
            inst->gauges[k] = value;
            return 0;
        },
        // histogram_observe
        [](struct shield_metrics_session* session,
           const char* name, double value,
           const char* const* lk, const char* const* lv, int lc) -> int {
            auto* inst = reinterpret_cast<metric_instance*>(session);
            if (!inst || !name) return -1;
            std::lock_guard<std::mutex> lock(inst->mu);
            SeriesKey k{name, normalize_labels(lk, lv, lc)};
            auto& e = inst->histograms[k];
            e.first += value; e.second += 1;
            return 0;
        },
        // flush — no-op (pull model)
        [](struct shield_metrics_session*) -> int { return 0; },
    };
    return v;
}

// ---------------------------------------------------------------------------
// v1 ABI entry
// ---------------------------------------------------------------------------
int metric_create(const struct shield_plugin_create_args_v1* args,
                  struct shield_plugin_instance_v1** out,
                  struct shield_error_v1* err) {
    if (!args || !out) return 1;
    auto* inst = new (std::nothrow) metric_instance;
    if (!inst) {
        if (err) { err->code = "plugin.create.failed"; err->message = "metrics.prometheus: oom"; }
        return 1;
    }
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->cfg = parse_config(args->config_json);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](struct shield_plugin_instance_v1* self,
                                   const char* iface,
                                   struct shield_error_v1*) -> const void* {
        if (!self || !iface) return nullptr;
        if (std::strcmp(iface, SHIELD_METRICS_INTERFACE) == 0) return &metric_vtable();
        return nullptr;
    };
    inst->shell.start = [](struct shield_plugin_instance_v1* self,
                           struct shield_error_v1* e) -> int {
        auto* inst = reinterpret_cast<metric_instance*>(self);
        if (!inst) return 1;
        try {
            tcp::endpoint ep(net::ip::make_address(inst->cfg.bind_address),
                             static_cast<unsigned short>(inst->cfg.port));
            inst->acceptor = std::make_unique<tcp::acceptor>(inst->ioc);
            inst->acceptor->open(ep.protocol());
            inst->acceptor->set_option(net::socket_base::reuse_address(true));
            inst->acceptor->bind(ep);
            inst->acceptor->listen(net::socket_base::max_listen_connections);
            inst->running.store(true);
            inst->io_thread = std::thread(accept_loop, inst);
        } catch (const std::exception& ex) {
            if (e) { e->code = "plugin.init.failed"; e->message = ex.what(); }
            return 1;
        }
        return 0;
    };
    inst->shell.shutdown = [](struct shield_plugin_instance_v1* self) {
        auto* inst = reinterpret_cast<metric_instance*>(self);
        if (!inst) return;
        inst->running.store(false);
        boost::system::error_code ec;
        if (inst->acceptor) inst->acceptor->close(ec);
        if (inst->io_thread.joinable()) inst->io_thread.join();
        delete inst;
    };
    *out = &inst->shell;
    return 0;
}

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT
const struct shield_plugin_abi_v1* shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "metrics.prometheus",
        "1.0.0",
        metric_create,
    };
    return &abi;
}
