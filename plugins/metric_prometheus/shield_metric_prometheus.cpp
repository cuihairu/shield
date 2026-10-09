// [SHIELD_PLUGIN] metrics.prometheus — Prometheus text exporter for
// shield.metrics.v1.
//
// v1 ABI + production-grade HTTP endpoint via boost::beast. Serves a single
// configurable path (default "/metrics") returning Prometheus text format.
// Supports counters/gauges/histograms with labels.
//
// As with health.http, does NOT link shield_net — embeds a minimal beast
// listener to stay a leaf shared library.

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/metrics.h"
#include "shield_lua_plugin_binding.hpp"

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
        if (j[p] == '\\' && p + 1 < j.size()) {
            out += j[p + 1];
            p += 2;
        } else {
            out += j[p++];
        }
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
    if (p < j.size() && j[p] == '-') {
        neg = true;
        ++p;
    }
    int v = 0;
    bool any = false;
    while (p < j.size() && j[p] >= '0' && j[p] <= '9') {
        v = v * 10 + (j[p] - '0');
        ++p;
        any = true;
    }
    return any ? (neg ? -v : v) : def;
}

metrics_config parse_config(const char* config_json) {
    metrics_config c;
    if (!config_json) return c;
    std::string s(config_json);
    c.bind_address = json_get_string(s, "bind_address");
    if (c.bind_address.empty()) c.bind_address = "0.0.0.0";
    c.port = json_get_int(s, "port", c.port);
    c.path = json_get_string(s, "path");
    if (c.path.empty()) c.path = "/metrics";
    return c;
}

// ---------------------------------------------------------------------------
// Label key — sorted vector of (k,v) pairs, so two points with the same labels
// collapse to the same series regardless of insertion order.
// ---------------------------------------------------------------------------
using LabelVec = std::vector<std::pair<std::string, std::string>>;

LabelVec normalize_labels(const char* const* keys, const char* const* vals,
                          int n) {
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
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* ctx = nullptr;

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
        case SHIELD_METRIC_COUNTER:
            return "counter";
        case SHIELD_METRIC_GAUGE:
            return "gauge";
        case SHIELD_METRIC_HISTOGRAM:
            return "histogram";
        case SHIELD_METRIC_TIMER:
            return "histogram";  // timers render as histograms
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
        os << k.name << "_count" << format_labels(k.labels) << " " << count
           << "\n";
    }
}

void handle_session(metric_instance* inst, tcp::socket socket) {
    beast::flat_buffer buf;
    http::request<http::string_body> req;
    try {
        http::read(socket, buf, req);
    } catch (...) {
        return;
    }

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
    try {
        http::write(socket, res);
    } catch (...) {
    }
    beast::error_code ec;
    socket.shutdown(tcp::socket::shutdown_both, ec);
}

// A synchronous accept() blocked in the io thread is not interrupted by
// closing the acceptor from another thread; poke one loopback connection so
// accept returns and the loop can observe running == false.
void poke_listener(const std::string& bind_address, unsigned short port,
                   net::io_context& ioc) {
    boost::system::error_code ec;
    tcp::socket poke(ioc);
    const std::string addr = (bind_address == "0.0.0.0" || bind_address == "::")
                                 ? "127.0.0.1"
                                 : bind_address;
    poke.connect({net::ip::make_address(addr), port}, ec);
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
        // The shutdown poke below also lands here as a bare connection that
        // never sends a request; handling it would block http::read, so
        // drop it instead.
        if (!inst->running.load()) break;
        handle_session(inst, std::move(socket));
    }
}

// ---------------------------------------------------------------------------
// Process-wide instance registry (for Lua proxy resolution)
// ---------------------------------------------------------------------------
std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, metric_instance*>& instances_map() {
    static std::map<std::string, metric_instance*> m;
    return m;
}

void register_instance(metric_instance* inst) {
    std::lock_guard<std::mutex> lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}
void unregister_instance(const std::string& id) {
    std::lock_guard<std::mutex> lk(instances_mu());
    instances_map().erase(id);
}
metric_instance* find_instance(const std::string& id) {
    std::lock_guard<std::mutex> lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// v1 metrics vtable
// ---------------------------------------------------------------------------
const shield_metrics_v1& metric_vtable() {
    static const shield_metrics_v1 v = {
        sizeof(shield_metrics_v1),
        SHIELD_METRICS_INTERFACE,
        "prometheus",
        "1.0.0",
        // connect — no-op (listener started on instance->start())
        [](const struct shield_metrics_config*, char* err_buf,
           int err_buf_size) -> struct shield_metrics_session* {
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
                        normalize_labels(point->label_keys, point->label_values,
                                         point->label_count)};
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
                // Reuse record lambda logic (inline to avoid dispatch
                // overhead).
                const auto& point = points[i];
                if (!point.name) continue;
                std::lock_guard<std::mutex> lock(inst->mu);
                SeriesKey k{point.name, normalize_labels(point.label_keys,
                                                         point.label_values,
                                                         point.label_count)};
                switch (point.type) {
                    case SHIELD_METRIC_COUNTER:
                        inst->counters[k] += point.value;
                        break;
                    case SHIELD_METRIC_GAUGE:
                        inst->gauges[k] = point.value;
                        break;
                    case SHIELD_METRIC_HISTOGRAM:
                    case SHIELD_METRIC_TIMER: {
                        auto& e = inst->histograms[k];
                        e.first += point.value;
                        e.second += 1;
                        break;
                    }
                }
            }
            return 0;
        },
        // counter_inc
        [](struct shield_metrics_session* session, const char* name,
           double value, const char* const* lk, const char* const* lv,
           int lc) -> int {
            auto* inst = reinterpret_cast<metric_instance*>(session);
            if (!inst || !name) return -1;
            std::lock_guard<std::mutex> lock(inst->mu);
            SeriesKey k{name, normalize_labels(lk, lv, lc)};
            inst->counters[k] += value;
            return 0;
        },
        // gauge_set
        [](struct shield_metrics_session* session, const char* name,
           double value, const char* const* lk, const char* const* lv,
           int lc) -> int {
            auto* inst = reinterpret_cast<metric_instance*>(session);
            if (!inst || !name) return -1;
            std::lock_guard<std::mutex> lock(inst->mu);
            SeriesKey k{name, normalize_labels(lk, lv, lc)};
            inst->gauges[k] = value;
            return 0;
        },
        // histogram_observe
        [](struct shield_metrics_session* session, const char* name,
           double value, const char* const* lk, const char* const* lv,
           int lc) -> int {
            auto* inst = reinterpret_cast<metric_instance*>(session);
            if (!inst || !name) return -1;
            std::lock_guard<std::mutex> lock(inst->mu);
            SeriesKey k{name, normalize_labels(lk, lv, lc)};
            auto& e = inst->histograms[k];
            e.first += value;
            e.second += 1;
            return 0;
        },
        // flush — no-op (pull model)
        [](struct shield_metrics_session*) -> int { return 0; },
    };
    return v;
}

// ---------------------------------------------------------------------------
// Lua surface: shield.metrics(binding) -> per-instance proxy
// ---------------------------------------------------------------------------

// Flatten a labels table {route="echo"} into parallel key/value arrays.
bool lua_labels_to_arrays(const shd::table& labels,
                          std::vector<std::string>& keys,
                          std::vector<std::string>& vals) {
    for (const auto& kv : labels) {
        if (!kv.first.is<std::string>() || !kv.second.is<std::string>()) {
            return false;
        }
        keys.push_back(kv.first.as<std::string>());
        vals.push_back(kv.second.as<std::string>());
    }
    return true;
}

// Build the per-instance Lua proxy: the three typed record methods
// (counter / gauge / histogram), each (name, value[, labels]) with labels
// as a string-keyed table. The leading optional self slot makes colon and
// dot call shapes equivalent (proxy:counter("x", 1) == proxy.counter("x", 1),
// same documented contract as the db facades). Argument-shape violations
// raise a Lua error (the shared binding-layer discipline); a failing vtable
// record returns false + error table.
shd::table make_instance_proxy(shd::state_view lua, metric_instance* inst) {
    auto proxy = lua.create_table();
    const shield_metrics_v1& v = metric_vtable();
    auto* session = reinterpret_cast<struct shield_metrics_session*>(inst);

    // Shared body for the (name, value[, labels]) typed record methods.
    auto make_record_fn = [&](int (*fn)(struct shield_metrics_session*,
                                        const char*, double, const char* const*,
                                        const char* const*, int)) {
        return [session, fn](
                   shd::this_state s, std::optional<shd::table> self,
                   std::string name, double value,
                   std::optional<shd::table> labels) -> shd::variadic_results {
            (void)self;  // colon-call receiver; ignored
            shd::state_view lua(s);
            shd::variadic_results results;
            std::vector<std::string> keys, vals;
            if (labels.has_value() &&
                !lua_labels_to_arrays(*labels, keys, vals)) {
                results.push_back(shd::make_object(lua, false));
                shd::table err = lua.create_table();
                err["code"] = "invalid_labels";
                err["message"] = "labels must be a string-keyed table";
                results.push_back(shd::make_object(lua, err));
                return results;
            }
            std::vector<const char*> kc, vc;
            for (size_t i = 0; i < keys.size(); ++i) {
                kc.push_back(keys[i].c_str());
                vc.push_back(vals[i].c_str());
            }
            int rc = fn(
                session, name.c_str(), value, kc.empty() ? nullptr : kc.data(),
                vc.empty() ? nullptr : vc.data(), static_cast<int>(kc.size()));
            results.push_back(shd::make_object(lua, rc == 0));
            return results;
        };
    };

    proxy.set_function("counter", make_record_fn(v.counter_inc));
    proxy.set_function("gauge", make_record_fn(v.gauge_set));
    proxy.set_function("histogram", make_record_fn(v.histogram_observe));
    return proxy;
}

// register_lua: install the callable namespace shield.metrics.
int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err) {
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "metrics.prometheus: lua_State is null";
        }
        return 1;
    }
    auto* current = reinterpret_cast<metric_instance*>(self);
    if (!current || !current->host_api ||
        !current->host_api->binding_instance_id) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "metrics.prometheus: host binding resolver is null";
        }
        return 1;
    }
    shd::state_view lua(L);

    // Build the callable namespace shield.metrics.
    shd::table shield =
        shield::plugins::get_or_create_global_subtable(lua, "shield");

    shd::object existing = shield["metrics"];
    if (!existing.is<shd::table>()) {
        auto ns = lua.create_table();
        auto mt = lua.create_table();
        const shield_host_api_v1* host_api = current->host_api;
        shield_plugin_context_v1* ctx = current->ctx;
        mt.set_function(
            "__call",
            [host_api, ctx](
                shd::this_state s, shd::table /*self*/,
                std::optional<std::string> binding) -> shd::variadic_results {
                shd::state_view lua(s);
                shd::variadic_results results;
                std::string logical = binding.value_or("");
                auto* inst = shield::plugins::resolve_lua_binding(
                    host_api, ctx, logical, find_instance);
                if (!inst) {
                    shield::plugins::push_module_unavailable(results, lua,
                                                             logical);
                    return results;
                }
                results.push_back(
                    shd::make_object(lua, make_instance_proxy(lua, inst)));
                return results;
            });
        shield::plugins::set_metatable(ns, mt);
        shield["metrics"] = ns;
    }

    return 0;
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
        if (err) {
            err->code = "plugin.create.failed";
            err->message = "metrics.prometheus: oom";
        }
        return 1;
    }
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->cfg = parse_config(args->config_json);
    inst->host_api = args->host_api;
    inst->ctx = args->ctx;
    register_instance(inst);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](struct shield_plugin_instance_v1* self,
                                   const char* iface,
                                   struct shield_error_v1*) -> const void* {
        if (!self || !iface) return nullptr;
        if (std::strcmp(iface, SHIELD_METRICS_INTERFACE) == 0)
            return &metric_vtable();
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
            if (e) {
                e->code = "plugin.init.failed";
                e->message = ex.what();
            }
            return 1;
        }
        return 0;
    };
    // Lua autonomy: register_lua installs the callable namespace
    // shield.metrics(binding) with the typed record methods.
    inst->shell.register_lua = &register_lua_impl;
    inst->shell.shutdown = [](struct shield_plugin_instance_v1* self) {
        auto* inst = reinterpret_cast<metric_instance*>(self);
        if (!inst) return;
        unregister_instance(inst->instance_id);
        inst->running.store(false);
        boost::system::error_code ec;
        if (inst->acceptor) inst->acceptor->close(ec);
        // Without the poke, a blocked accept() never returns and shutdown
        // joins forever (observed as a hung SIGTERM'd process).
        poke_listener(inst->cfg.bind_address,
                      static_cast<unsigned short>(inst->cfg.port), inst->ioc);
        if (inst->io_thread.joinable()) inst->io_thread.join();
        delete inst;
    };
    *out = &inst->shell;
    return 0;
}

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT const struct shield_plugin_abi_v1*
shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "metrics.prometheus",
        "1.0.0",
        metric_create,
    };
    return &abi;
}
