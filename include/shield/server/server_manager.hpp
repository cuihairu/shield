// [SHIELD_SERVER] Process-wide server state machine (P0).
// See docs/runtime-server.md and open-decisions.md OD-016/OD-017: a plain
// C++ singleton (no CAF actor, no mailbox, no Starter). State truth lives
// here; Lua reaches it through the shield.server facade.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace shield::server {

/// Server lifecycle states (runtime-server.md 状态机). `starting` is the
/// bootstrap initial state; `shutdown` is terminal.
enum class ServerState {
    kStarting,
    kRunning,
    kMaintenance,
    kShutdown,
};

const char* server_state_name(ServerState state);

/// Parses a state name (the same strings server_state_name produces).
/// Returns false for unknown names.
bool parse_server_state(std::string_view name, ServerState* out);

/// Module-owned configuration (the `server_manager` config section). Parsed
/// by the module itself from the global config; core config never grows
/// server fields. All fields are optional; see runtime-server.md 配置.
struct ServerConfig {
    std::string name = "server_manager";
    std::string info_name;
    std::string info_version;
    std::string info_region;

    /// Reads the `server_manager` section from the global config. Returns
    /// false with `error` set when a present section fails validation.
    static bool from_global_config(ServerConfig* out, std::string* error);
};

/// Independent validation so bootstrap can fail fast with a message that
/// names the offending field (OMOD-SV-006/013).
bool validate_server_config(const ServerConfig& config, std::string* error);

/// Result of delivering one state notification to a watcher service.
enum class Delivery {
    kOk,         /// delivered to a live service
    kGone,       /// service no longer exists: drop the watcher registration
    kRetryable,  /// transient failure (e.g. runtime stopping): keep watching
};

/// Invoked without the manager lock held, one call per watcher per
/// transition. Injected by bootstrap to keep shield_server decoupled from
/// shield_lua (OD-017).
using NotifyFn = std::function<Delivery(const std::string& service_id,
                                        const std::string& state_name)>;

/// Invoked without the manager lock held after a scheduled shutdown delay
/// elapses. Bootstrap injects the same stop request the signal handlers use.
using StopRequestFn = std::function<void()>;

/// Process-wide server state singleton. Plain C++ state behind one mutex —
/// no CAF messages. Watchers are keyed by (watch id, service id); the C++
/// registry never holds Lua closures so shield_server does not link sol2
/// (OD-016).
class ServerManager {
public:
    explicit ServerManager(ServerConfig config);
    ~ServerManager();

    ServerManager(const ServerManager&) = delete;
    ServerManager& operator=(const ServerManager&) = delete;

    static ServerManager* global();
    static void set_global(ServerManager* manager);

    const ServerConfig& config() const { return config_; }

    /// Cluster node id for the runtime-info surface; empty on a standalone
    /// node. Kept as a setter so shield_server never links shield_cluster.
    void set_locality(std::string node_id);

    /// app.version fallback for version() (info.version wins). Injected so
    /// version() does not read the global config store.
    void set_version_fallback(std::string fallback);

    void set_notify_fn(NotifyFn fn);
    void set_stop_request_fn(StopRequestFn fn);

    /// Static transition table (runtime-server.md): pure and instance-free
    /// so tests can drive the full 4x4 matrix. Same-value sets are always
    /// allowed (idempotent); `starting` is initial-only; `shutdown` is
    /// terminal.
    static bool transition_allowed(ServerState from, ServerState to);

    /// starting -> running, called by bootstrap when init completes.
    /// Idempotent; also stamps the uptime/started_at origin on the first
    /// ready transition.
    void mark_ready();

    ServerState state() const;

    /// Applies the transition table. Returns false with `error` set on an
    /// illegal transition ("invalid state transition: a -> b"). Same-value
    /// sets return true and notify nobody.
    bool set_state(ServerState state, std::string* error);

    /// Registers `service_id` as a state watcher. Idempotent per service:
    /// re-watching returns the existing id and keeps the original
    /// registration (the per-VM Lua layer decides whether to swap the
    /// callback).
    std::uint64_t watch(const std::string& service_id);

    /// Drops a watcher registration. Unknown/already-removed ids are a
    /// silent success (idempotent).
    void unwatch(std::uint64_t watch_id);

    std::size_t watcher_count() const;

    /// Seconds since mark_ready on the steady clock; 0 while still starting.
    double uptime_seconds() const;

    /// info.version, else the injected fallback (app.version), else "".
    std::string version() const;

    /// Cluster node id; empty on a standalone node. Returned by value:
    /// set_locality() writes it under the lock from bootstrap while console
    /// threads read.
    std::string node_id() const;

    /// Wall-clock ms of the mark_ready moment; 0 before the first ready.
    std::uint64_t started_at_ms() const;

    /// shutdown handover (runtime-server.md shutdown): migrate to `shutdown`
    /// (notifying watchers), then invoke the injected stop request after
    /// `delay_ms`. delay_ms == 0 invokes it synchronously; otherwise a
    /// jthread timer waits (interruptible by stop()) and invokes it. Returns
    /// false with `error` = "shutdown already scheduled" when a shutdown was
    /// scheduled before (or the state is already `shutdown` by any path).
    bool schedule_shutdown(std::uint64_t delay_ms, std::string* error);

    bool shutdown_scheduled() const;

    /// Stops the timer thread and drops the injected callbacks. Called from
    /// bootstrap teardown (and the destructor) BEFORE the objects the
    /// callbacks capture are released.
    void stop();

private:
    struct WatchEntry {
        std::uint64_t id = 0;
        std::string service_id;
    };

    /// Copies the watcher list out and delivers without the lock; watchers
    /// reported kGone are erased back under the lock.
    void notify_watchers(const std::vector<WatchEntry>& watchers,
                         ServerState state);

    ServerConfig config_;
    std::string node_id_;
    std::string version_fallback_;
    ServerState state_ = ServerState::kStarting;
    std::uint64_t started_at_ms_ = 0;
    std::chrono::steady_clock::time_point ready_steady_{};
    bool shutdown_scheduled_ = false;
    NotifyFn notify_fn_;
    StopRequestFn stop_request_fn_;
    mutable std::mutex mutex_;
    std::vector<WatchEntry> watchers_;
    std::uint64_t next_watch_id_ = 1;
    // Timer thread for schedule_shutdown. Assignment and join happen WITHOUT
    // holding mutex_: the timer body locks mutex_ on its way out to read the
    // stop-request callback.
    std::jthread shutdown_timer_;
};

}  // namespace shield::server
