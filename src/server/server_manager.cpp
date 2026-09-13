// [SHIELD_SERVER] Process-wide server state machine (P0).
// See server_manager.hpp and docs/runtime-server.md.
#include "shield/server/server_manager.hpp"

#include <algorithm>
#include <utility>

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"

namespace shield::server {
namespace {

ServerManager* g_server_manager = nullptr;

std::uint64_t wall_now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

bool info_length_ok(const std::string& value, const char* key,
                    std::string* error) {
    if (value.size() <= 64) {
        return true;
    }
    if (error) {
        *error = std::string(key) + " must be at most 64 characters";
    }
    return false;
}

}  // namespace

const char* server_state_name(ServerState state) {
    switch (state) {
        case ServerState::kStarting:
            return "starting";
        case ServerState::kRunning:
            return "running";
        case ServerState::kMaintenance:
            return "maintenance";
        case ServerState::kShutdown:
            return "shutdown";
    }
    return "unknown";  // GCOVR_EXCL_LINE (every enum value is listed above)
}

bool parse_server_state(std::string_view name, ServerState* out) {
    if (name == "starting") {
        *out = ServerState::kStarting;
    } else if (name == "running") {
        *out = ServerState::kRunning;
    } else if (name == "maintenance") {
        *out = ServerState::kMaintenance;
    } else if (name == "shutdown") {
        *out = ServerState::kShutdown;
    } else {
        return false;
    }
    return true;
}

bool ServerConfig::from_global_config(ServerConfig* out, std::string* error) {
    auto& cfg = shield::config::global_config();
    out->name = cfg.get_string("server_manager.name", "server_manager");
    out->info_name = cfg.get_string("server_manager.info.name", "");
    out->info_version = cfg.get_string("server_manager.info.version", "");
    out->info_region = cfg.get_string("server_manager.info.region", "");
    (void)error;
    return true;
}

bool validate_server_config(const ServerConfig& config, std::string* error) {
    if (config.name.empty()) {
        if (error) *error = "server_manager.name must not be empty";
        return false;
    }
    return info_length_ok(config.name, "server_manager.name", error) &&
           info_length_ok(config.info_name, "server_manager.info.name",
                          error) &&
           info_length_ok(config.info_version, "server_manager.info.version",
                          error) &&
           info_length_ok(config.info_region, "server_manager.info.region",
                          error);
}

ServerManager::ServerManager(ServerConfig config)
    : config_(std::move(config)) {}

ServerManager::~ServerManager() { stop(); }

ServerManager* ServerManager::global() { return g_server_manager; }

void ServerManager::set_global(ServerManager* manager) {
    g_server_manager = manager;
}

void ServerManager::set_locality(std::string node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    node_id_ = std::move(node_id);
}

void ServerManager::set_version_fallback(std::string fallback) {
    std::lock_guard<std::mutex> lock(mutex_);
    version_fallback_ = std::move(fallback);
}

void ServerManager::set_notify_fn(NotifyFn fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    notify_fn_ = std::move(fn);
}

void ServerManager::set_stop_request_fn(StopRequestFn fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_request_fn_ = std::move(fn);
}

bool ServerManager::transition_allowed(ServerState from, ServerState to) {
    // Same-value set: idempotent success for every state (including
    // starting->starting and the terminal shutdown->shutdown).
    if (from == to) return true;
    // `starting` is initial-only; nothing may migrate back into it.
    if (to == ServerState::kStarting) return false;
    // `shutdown` is terminal.
    if (from == ServerState::kShutdown) return false;
    switch (from) {
        case ServerState::kStarting:
            return to == ServerState::kRunning || to == ServerState::kShutdown;
        case ServerState::kRunning:
        case ServerState::kMaintenance:
            return to == ServerState::kRunning ||
                   to == ServerState::kMaintenance ||
                   to == ServerState::kShutdown;
        case ServerState::kShutdown:
            return false;  // GCOVR_EXCL_LINE (terminal state handled above)
    }
    return false;  // GCOVR_EXCL_LINE (every enum value is handled above)
}

void ServerManager::mark_ready() {
    set_state(ServerState::kRunning, nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_at_ms_ == 0) {
        started_at_ms_ = wall_now_ms();
        ready_steady_ = std::chrono::steady_clock::now();
    }
}

ServerState ServerManager::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool ServerManager::set_state(ServerState to, std::string* error) {
    std::vector<WatchEntry> to_notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!transition_allowed(state_, to)) {
            if (error) {
                *error = std::string("invalid state transition: ") +
                         server_state_name(state_) + " -> " +
                         server_state_name(to);
            }
            return false;
        }
        if (state_ != to) {
            state_ = to;
            // Copy the watcher snapshot out: delivery runs without the lock
            // (watcher callbacks may re-enter watch/unwatch/state()).
            to_notify = watchers_;
        }
    }
    notify_watchers(to_notify, to);
    return true;
}

void ServerManager::notify_watchers(const std::vector<WatchEntry>& watchers,
                                    ServerState state) {
    if (watchers.empty()) {
        return;
    }
    NotifyFn fn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fn = notify_fn_;
    }
    if (!fn) {
        return;
    }
    const std::string state_name = server_state_name(state);
    std::vector<std::uint64_t> gone;
    for (const auto& watcher : watchers) {
        if (fn(watcher.service_id, state_name) == Delivery::kGone) {
            gone.push_back(watcher.id);
        }
    }
    if (gone.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto id : gone) {
        std::erase_if(watchers_,
                      [id](const WatchEntry& entry) { return entry.id == id; });
    }
}

std::uint64_t ServerManager::watch(const std::string& service_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : watchers_) {
        if (entry.service_id == service_id) {
            return entry.id;
        }
    }
    watchers_.push_back({next_watch_id_, service_id});
    return next_watch_id_++;
}

void ServerManager::unwatch(std::uint64_t watch_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Idempotent by contract: an unknown id is a silent success.
    std::erase_if(watchers_, [watch_id](const WatchEntry& entry) {
        return entry.id == watch_id;
    });
}

std::size_t ServerManager::watcher_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return watchers_.size();
}

double ServerManager::uptime_seconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_at_ms_ == 0) {
        return 0.0;
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         ready_steady_)
        .count();
}

std::string ServerManager::version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.info_version.empty()) {
        return config_.info_version;
    }
    return version_fallback_;
}

std::string ServerManager::node_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return node_id_;
}

std::uint64_t ServerManager::started_at_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_at_ms_;
}

bool ServerManager::schedule_shutdown(std::uint64_t delay_ms,
                                      std::string* error) {
    std::vector<WatchEntry> to_notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A shutdown scheduled before — or a `shutdown` state reached by any
        // other path — means the handover already happened.
        if (shutdown_scheduled_ || state_ == ServerState::kShutdown) {
            if (error) *error = "shutdown already scheduled";
            return false;
        }
        if (!transition_allowed(state_, ServerState::kShutdown)) {
            // Unreachable: every state other than shutdown itself may move
            // to shutdown (guarded for symmetry with set_state).
            if (error) {  // GCOVR_EXCL_LINE (unreachable twin of set_state)
                *error =  // GCOVR_EXCL_LINE
                    std::string("invalid state transition: ") +  //
                    server_state_name(state_) + " -> shutdown";
            }
            return false;  // GCOVR_EXCL_LINE
        }
        state_ = ServerState::kShutdown;
        shutdown_scheduled_ = true;
        to_notify = watchers_;
    }
    // Migrate + notify inline instead of via set_state(): schedule_shutdown
    // owns the shutdown_scheduled_ decision under one lock pass, so a
    // concurrent caller cannot slip past the already-scheduled check.
    notify_watchers(to_notify, ServerState::kShutdown);

    auto& log = shield::log::get_logger("server");
    if (delay_ms == 0) {
        StopRequestFn fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fn = stop_request_fn_;
        }
        if (fn) {
            fn();
        }
        SHIELD_LOG_INFO(log, "Server shutdown requested (immediate)");
        return true;
    }
    // Interruptible wait: stop() aborts the remaining delay so teardown is
    // never held up by a long shutdown timer (condition_variable_any takes
    // the stop_token directly; the never-true predicate makes the wait
    // return on stop or timeout only).
    shutdown_timer_ = std::jthread([this, delay_ms](std::stop_token stop) {
        std::mutex wake_mutex;
        std::condition_variable_any wake_cv;
        std::unique_lock<std::mutex> wake_lock(wake_mutex);
        wake_cv.wait_for(wake_lock, stop, std::chrono::milliseconds(delay_ms),
                         [] { return false; });
        if (stop.stop_requested()) {
            return;
        }
        StopRequestFn fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fn = stop_request_fn_;
        }
        if (fn) {
            fn();
        }
    });
    SHIELD_LOG_INFO(
        log, "Server shutdown scheduled in " + std::to_string(delay_ms) + "ms");
    return true;
}

bool ServerManager::shutdown_scheduled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutdown_scheduled_;
}

void ServerManager::stop() {
    // Join the timer WITHOUT holding mutex_: its body locks mutex_ on the
    // way out, so joining under the lock would deadlock.
    if (shutdown_timer_.joinable()) {
        shutdown_timer_.request_stop();
        shutdown_timer_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    notify_fn_ = nullptr;
    stop_request_fn_ = nullptr;
}

}  // namespace shield::server
