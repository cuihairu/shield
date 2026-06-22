// [SHIELD_PLUGIN] health.http — HTTP health provider for shield.health.v1.
//
// v1 ABI + production-grade HTTP endpoint via boost::beast. Serves configured
// liveness_path (default "/health") and readiness_path (default "/ready")
// with 200/503 + JSON body aggregating all registered checks.
//
// Does NOT link shield_net: plugins are independent shared libraries and
// linking the static shield_net would cause symbol clashes with the host.
// We embed a minimal beast listener here.

#include "shield/plugin/abi.h"
#include "shield/plugin/health.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace {

struct CheckEntry {
    std::string name;
    std::function<int(shield_health_check_result*, void*)> check;
    void* user_data = nullptr;
};

struct health_config {
    std::string bind_address = "0.0.0.0";
    int port = 8086;
    std::string liveness_path = "/health";
    std::string readiness_path = "/ready";
};

// Minimal JSON string escape (for check names / messages).
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    return out;
}

// Tiny JSON string extraction (used for config parsing only).
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

health_config parse_config(const char* config_json) {
    health_config c;
    if (!config_json) return c;
    std::string s(config_json);
    c.bind_address  = json_get_string(s, "bind_address");  if (c.bind_address.empty())  c.bind_address  = "0.0.0.0";
    c.port          = json_get_int(s, "port", c.port);
    c.liveness_path = json_get_string(s, "liveness_path"); if (c.liveness_path.empty()) c.liveness_path = "/health";
    c.readiness_path = json_get_string(s, "readiness_path"); if (c.readiness_path.empty()) c.readiness_path = "/ready";
    return c;
}

char* dup_cstr(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------
struct health_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    health_config cfg;

    std::mutex checks_mu;
    std::vector<std::unique_ptr<CheckEntry>> checks;

    // HTTP runtime (started lazily on connect)
    net::io_context ioc;
    std::unique_ptr<tcp::acceptor> acceptor;
    std::thread io_thread;
    std::atomic<bool> running{false};
    std::mutex state_mu;
    shield_health_status aggregated_status = SHIELD_HEALTH_OK;
};

// Run one HTTP connection synchronously on the io_thread.
void handle_session(health_instance* inst, tcp::socket socket) {
    beast::flat_buffer buf;
    http::request<http::string_body> req;
    try {
        http::read(socket, buf, req);
    } catch (...) {
        return;
    }

    // Build response: only GET is supported. Map liveness_path and
    // readiness_path to the same logic — both aggregate all checks.
    http::response<http::string_body> res;
    res.version(req.version());
    res.set(http::field::server, "shield.health.http");
    res.set(http::field::content_type, "application/json");
    res.keep_alive(false);

    bool known = false;
    if (req.method() == http::verb::get) {
        std::string target = std::string(req.target());
        if (target == inst->cfg.liveness_path || target == inst->cfg.readiness_path) {
            known = true;

            // Snapshot checks.
            std::vector<CheckEntry*> snapshot;
            {
                std::lock_guard<std::mutex> lock(inst->checks_mu);
                snapshot.reserve(inst->checks.size());
                for (auto& c : inst->checks) snapshot.push_back(c.get());
            }

            shield_health_status agg = SHIELD_HEALTH_OK;
            std::ostringstream body;
            body << "{\"status\":\"";
            std::vector<std::string> items;
            for (auto* e : snapshot) {
                shield_health_check_result r{};
                auto t0 = std::chrono::steady_clock::now();
                int rc = e->check ? e->check(&r, e->user_data) : -1;
                auto t1 = std::chrono::steady_clock::now();
                int64_t latency_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

                const char* status_str = "ok";
                if (rc != 0 || r.status == SHIELD_HEALTH_FAIL) {
                    agg = SHIELD_HEALTH_FAIL;
                    status_str = "fail";
                } else if (r.status == SHIELD_HEALTH_DEGRADED && agg != SHIELD_HEALTH_FAIL) {
                    agg = SHIELD_HEALTH_DEGRADED;
                    status_str = "degraded";
                }
                std::ostringstream item;
                item << "{\"name\":\"" << json_escape(e->name) << "\","
                     << "\"status\":\"" << status_str << "\","
                     << "\"latency_ms\":" << latency_ms;
                if (r.message) item << ",\"message\":\"" << json_escape(r.message) << "\"";
                item << "}";
                items.push_back(item.str());

                if (r.check_name) std::free(const_cast<char*>(r.check_name));
                if (r.message)    std::free(const_cast<char*>(r.message));
            }
            const char* agg_str = agg == SHIELD_HEALTH_OK ? "ok"
                               : agg == SHIELD_HEALTH_DEGRADED ? "degraded" : "fail";
            body << agg_str << "\",\"checks:[";
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) body << ",";
                body << items[i];
            }
            body << "]}";

            res.result(agg == SHIELD_HEALTH_FAIL ? http::status::service_unavailable
                                                 : http::status::ok);
            res.body() = body.str();
            {
                std::lock_guard<std::mutex> lock(inst->state_mu);
                inst->aggregated_status = agg;
            }
        }
    }
    if (!known) {
        res.result(http::status::not_found);
        res.body() = "{\"error\":\"not_found\"}";
    }
    res.prepare_payload();
    try {
        http::write(socket, res);
    } catch (...) {}
    beast::error_code ec;
    socket.shutdown(tcp::socket::shutdown_both, ec);
}

// Synchronous accept loop: one accept per iteration, hand off to
// handle_session inline. Simple but serial; sufficient for health checks
// which are low-frequency.
void accept_loop_sync(health_instance* inst) {
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
// v1 health vtable
// ---------------------------------------------------------------------------
const shield_health_v1& health_vtable() {
    static const shield_health_v1 v = {
        sizeof(shield_health_v1),
        "http",
        "1.0.0",
        // connect — starts the HTTP listener. Returns the instance as the
        // session handle.
        [](const struct shield_health_config* cfg,
           char* err_buf, int err_buf_size) -> struct shield_health_session* {
            (void)cfg;  // real config is parsed at instance create() time
            // The connect() in v1 table is invoked at most once per instance;
            // but we can also lazy-start from start() below. For now we don't
            // start here; the host calls instance->start() after create().
            if (err_buf && err_buf_size > 0) err_buf[0] = '\0';
            return nullptr;
        },
        // disconnect — no-op (HTTP server stopped on shutdown).
        [](struct shield_health_session*) {},
        // register_check
        [](struct shield_health_session* session, const char* name,
           int (*check)(struct shield_health_check_result*, void*),
           void* user_data) -> int {
            auto* inst = reinterpret_cast<health_instance*>(session);
            if (!inst || !name || !check) return -1;
            auto entry = std::make_unique<CheckEntry>();
            entry->name = name;
            entry->check = check;
            entry->user_data = user_data;
            std::lock_guard<std::mutex> lock(inst->checks_mu);
            inst->checks.push_back(std::move(entry));
            return 0;
        },
        // check_all
        [](struct shield_health_session* session,
           struct shield_health_check_result* results, int max_results,
           int* out_count) -> int {
            auto* inst = reinterpret_cast<health_instance*>(session);
            if (!inst || !results || !out_count) return -1;
            std::lock_guard<std::mutex> lock(inst->checks_mu);
            int i = 0;
            for (auto& e : inst->checks) {
                if (i >= max_results) break;
                auto& r = results[i];
                r.status = SHIELD_HEALTH_OK;
                r.check_name = dup_cstr(e->name.c_str());
                r.message = dup_cstr("");
                r.latency_ms = 0;
                e->check(&r, e->user_data);
                ++i;
            }
            *out_count = i;
            return 0;
        },
        // get_status
        [](struct shield_health_session* session) -> enum shield_health_status {
            auto* inst = reinterpret_cast<health_instance*>(session);
            if (!inst) return SHIELD_HEALTH_FAIL;
            std::lock_guard<std::mutex> lock(inst->state_mu);
            return inst->aggregated_status;
        },
        // free_result
        [](struct shield_health_check_result* r) {
            if (!r) return;
            if (r->check_name) std::free(const_cast<char*>(r->check_name));
            if (r->message)    std::free(const_cast<char*>(r->message));
            r->check_name = nullptr;
            r->message = nullptr;
        },
    };
    return v;
}

// ---------------------------------------------------------------------------
// v1 ABI entry
// ---------------------------------------------------------------------------
int health_create(const struct shield_plugin_create_args_v1* args,
                  struct shield_plugin_instance_v1** out,
                  struct shield_error_v1* err) {
    if (!args || !out) return 1;
    auto* inst = new (std::nothrow) health_instance;
    if (!inst) {
        if (err) { err->code = "plugin.create.failed"; err->message = "health.http: oom"; }
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
        if (std::strcmp(iface, SHIELD_HEALTH_INTERFACE) == 0) return &health_vtable();
        return nullptr;
    };
    // start — open the listener.
    inst->shell.start = [](struct shield_plugin_instance_v1* self,
                           struct shield_error_v1* e) -> int {
        auto* inst = reinterpret_cast<health_instance*>(self);
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
            inst->io_thread = std::thread(accept_loop_sync, inst);
        } catch (const std::exception& ex) {
            if (e) {
                e->code = "plugin.init.failed";
                e->message = ex.what();
            }
            return 1;
        }
        return 0;
    };
    // shutdown — stop listener, join thread, clear checks.
    inst->shell.shutdown = [](struct shield_plugin_instance_v1* self) {
        auto* inst = reinterpret_cast<health_instance*>(self);
        if (!inst) return;
        inst->running.store(false);
        boost::system::error_code ec;
        if (inst->acceptor) inst->acceptor->close(ec);
        if (inst->io_thread.joinable()) inst->io_thread.join();
        {
            std::lock_guard<std::mutex> lock(inst->checks_mu);
            inst->checks.clear();
        }
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
        "health.http",
        "1.0.0",
        health_create,
    };
    return &abi;
}
