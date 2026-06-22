// [SHIELD_PLUGIN] queue.redis — Redis pub/sub provider for shield.queue.v1.
//
// v1 ABI. The v1 queue interface is intentionally narrow: publish/subscribe/
// unsubscribe only. Legacy shield_redis exposed consumer groups and acks;
// those are NOT in v1 (see docs/plugin-system.md: queue.v1 = pub/sub), and
// the dead code has been removed.
//
// subscribe() semantics: the v1 API expects messages to start flowing to the
// callback after subscribe returns. We spawn a dedicated subscriber thread
// per shield_queue_conn that owns a sw::redis::Subscriber. Unsubscribe stops
// the channel; disconnect stops the thread and joins.

#include "shield/plugin/abi.h"
#include "shield/plugin/queue.h"

#include <sw/redis++/redis++.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

struct SubEntry {
    shield_queue_on_message cb;
    void* user_data;
};

// shield_queue_conn is opaque in queue.h; concrete layout lives here.
// A single Subscriber thread consumes messages and dispatches to the
// per-channel callback registered via subscribe().
struct shield_queue_conn {
    std::shared_ptr<sw::redis::Redis> redis;
    sw::redis::Subscriber subscriber;
    std::unordered_map<std::string, SubEntry> subs;
    std::mutex subs_mu;
    std::thread worker;
    std::atomic<bool> running{false};

    explicit shield_queue_conn(std::shared_ptr<sw::redis::Redis> r)
        : redis(std::move(r)), subscriber(redis->subscriber()) {}
};

void dispatch_message(shield_queue_conn* c, const std::string& channel,
                      const std::string& msg) {
    SubEntry entry;
    {
        std::lock_guard<std::mutex> lock(c->subs_mu);
        auto it = c->subs.find(channel);
        if (it == c->subs.end()) return;
        entry = it->second;
    }
    if (entry.cb) {
        entry.cb(channel.c_str(), msg.c_str(),
                 static_cast<int>(msg.size()), entry.user_data);
    }
}

void subscriber_loop(shield_queue_conn* c) {
    using namespace std::chrono;
    while (c->running.load()) {
        try {
            c->subscriber.consume();
        } catch (const sw::redis::TimeoutError&) {
            // Normal: no messages within the socket timeout; loop.
        } catch (const sw::redis::ClosedError&) {
            break;
        } catch (const std::exception&) {
            // Transient errors — back off briefly and retry.
            std::this_thread::sleep_for(milliseconds(100));
        }
    }
}

// ---------------------------------------------------------------------------
// v1 queue vtable
// ---------------------------------------------------------------------------
const shield_queue_v1& queue_vtable() {
    static const shield_queue_v1 v = {
        sizeof(shield_queue_v1),
        "redis",
        "1.0.0",
        // connect
        [](const shield_queue_config* cfg,
           char* err_buf, int err_buf_size) -> shield_queue_conn* {
            if (!cfg) return nullptr;
            try {
                sw::redis::ConnectionOptions opts;
                opts.host = cfg->host && cfg->host[0] ? cfg->host : "localhost";
                opts.port = cfg->port > 0 ? cfg->port : 6379;
                if (cfg->password && cfg->password[0])
                    opts.password = cfg->password;
                opts.db = cfg->db > 0 ? cfg->db : 0;
                opts.connect_timeout = std::chrono::milliseconds(
                    cfg->connect_timeout_ms > 0 ? cfg->connect_timeout_ms : 5000);
                opts.socket_timeout = std::chrono::milliseconds(
                    cfg->command_timeout_ms > 0 ? cfg->command_timeout_ms : 1000);

                auto redis = std::make_shared<sw::redis::Redis>(opts);

                auto* conn = new shield_queue_conn(redis);
                conn->subscriber.on_message(
                    [conn](const std::string& channel, const std::string& msg) {
                        dispatch_message(conn, channel, msg);
                    });
                conn->running.store(true);
                conn->worker = std::thread(subscriber_loop, conn);
                return conn;
            } catch (const std::exception& e) {
                if (err_buf && err_buf_size > 0) {
                    std::snprintf(err_buf, err_buf_size, "%s", e.what());
                }
                return nullptr;
            }
        },
        // disconnect
        [](shield_queue_conn* c) {
            if (!c) return;
            c->running.store(false);
            // Force the consumer out of consume(): closing the subscriber is
            // the cleanest cross-platform way to interrupt its blocking read.
            try { c->subscriber.close(); } catch (...) {}
            if (c->worker.joinable()) c->worker.join();
            delete c;
        },
        // publish
        [](shield_queue_conn* c, const char* channel,
           const char* data, int data_len) -> int {
            if (!c || !c->redis || !channel || !data) return -1;
            try {
                std::string msg(data, data_len > 0 ? data_len : std::strlen(data));
                return static_cast<int>(
                    c->redis->publish(std::string(channel), msg));
            } catch (...) { return -1; }
        },
        // subscribe
        [](shield_queue_conn* c, const char* channel,
           shield_queue_on_message callback, void* user_data) -> int {
            if (!c || !channel) return -1;
            try {
                {
                    std::lock_guard<std::mutex> lock(c->subs_mu);
                    c->subs[std::string(channel)] = {callback, user_data};
                }
                c->subscriber.subscribe(std::string(channel));
                return 0;
            } catch (...) { return -1; }
        },
        // unsubscribe
        [](shield_queue_conn* c, const char* channel) -> int {
            if (!c || !channel) return -1;
            try {
                c->subscriber.unsubscribe(std::string(channel));
                {
                    std::lock_guard<std::mutex> lock(c->subs_mu);
                    c->subs.erase(std::string(channel));
                }
                return 0;
            } catch (...) { return -1; }
        },
    };
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// v1 ABI entry
// ---------------------------------------------------------------------------
namespace {

struct queue_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
};

int queue_create(const shield_plugin_create_args_v1* args,
                 shield_plugin_instance_v1** out,
                 shield_error_v1* err) {
    (void)err;
    auto* inst = new queue_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1*,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_QUEUE_INTERFACE) == 0)
            return &queue_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) { return 0; };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        delete reinterpret_cast<queue_instance*>(self);
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
        "queue.redis",
        "1.0.0",
        queue_create,
    };
    return &abi;
}
