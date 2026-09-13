// [SHIELD_PLAYER] Process-wide player session index (P0-b C++ primitive)
#include "shield/player/player_manager.hpp"

#include <algorithm>

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"

namespace shield::player {

namespace {
PlayerManager* g_player_manager = nullptr;
}

const char* session_state_name(SessionState state) {
    switch (state) {
        case SessionState::kConnecting:
            return "connecting";
        case SessionState::kAuthenticating:
            return "authenticating";
        case SessionState::kOnline:
            return "online";
        case SessionState::kReady:
            return "ready";
        case SessionState::kDisconnected:
            return "disconnected";
        case SessionState::kAnonymous:
            return "anonymous";
        case SessionState::kSpectator:
            return "spectator";
    }
    return "unknown";  // GCOVR_EXCL_LINE (every enum value is listed above)
}

bool PlayerConfig::from_global_config(PlayerConfig* out, std::string* error) {
    auto& cfg = shield::config::global_config();
    // Module presence is gated upstream (bootstrap validates the YAML
    // section against the compiled-in module and constructs the manager
    // only then); a parsed config therefore describes an enabled player
    // subsystem. The flattened key store never contains a bare "player"
    // entry for a map section, so there is nothing to sniff here.
    out->enabled = true;

    auto multi = cfg.get_string("player.multi_device", "single");
    if (multi == "single") {
        out->multi_device = MultiDevicePolicy::kSingle;
    } else if (multi == "kick_old") {
        out->multi_device = MultiDevicePolicy::kKickOld;
    } else if (multi == "multi") {
        out->multi_device = MultiDevicePolicy::kMulti;
    } else {
        if (error) {
            *error =
                "player.multi_device must be single|kick_old|multi, got '" +
                multi + "'";
        }
        return false;
    }

    out->max_devices =
        static_cast<std::uint32_t>(cfg.get_int("player.max_devices", 2));
    out->anonymous_enabled = cfg.get_bool("player.anonymous", false);
    out->spectator_enabled = cfg.get_bool("player.spectator", false);
    out->reconnect_window_ms = static_cast<std::uint64_t>(
        cfg.get_int("player.reconnect_window_ms", 30000));
    out->message_queue_limit = static_cast<std::uint32_t>(
        cfg.get_int("player.message_queue_limit", 64));
    out->persistence_binding = cfg.get_string("player.persistence.binding", "");
    out->persistence_fields = cfg.get_string_array("player.persistence.fields");
    auto on_error = cfg.get_string("player.persistence.on_save_error", "log");
    if (on_error != "log" && on_error != "panic") {
        if (error) {
            *error =
                "player.persistence.on_save_error must be log|panic, got '" +
                on_error + "'";
        }
        return false;
    }
    out->persistence_panic_on_error = (on_error == "panic");
    out->save_interval_ms = static_cast<std::uint64_t>(
        cfg.get_int("player.persistence.save_interval_ms", 60000));
    return true;
}

bool validate_player_config(const PlayerConfig& config, std::string* error) {
    // max_devices only meaningfully applies to the `multi` policy; under
    // single/kick_old it is ignored by design (a single live session).
    if (config.multi_device == MultiDevicePolicy::kMulti &&
        config.max_devices == 0) {
        if (error) *error = "player.max_devices must be >= 1";
        return false;
    }
    return true;
}

PlayerManager::PlayerManager(PlayerConfig config) : config_(config) {}

PlayerManager* PlayerManager::global() { return g_player_manager; }

void PlayerManager::set_global(PlayerManager* manager) {
    g_player_manager = manager;
}

std::uint64_t PlayerManager::node_epoch() const { return node_epoch_; }

std::string PlayerManager::node_id() const { return node_id_; }

void PlayerManager::set_locality(const std::string& node_id,
                                 std::uint64_t epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    node_id_ = node_id;
    node_epoch_ = epoch;
}

AdmissionDecision PlayerManager::admit(const std::string& uid,
                                       const std::string& device_id,
                                       std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    AdmissionDecision decision;
    auto it = sessions_.find(uid);
    if (it == sessions_.end()) return decision;  // fresh login

    Entry& entry = it->second;
    const bool live = (entry.disconnected_ms == 0);
    const bool in_window =
        !live && now_ms >= entry.disconnected_ms &&
        now_ms - entry.disconnected_ms <= config_.reconnect_window_ms;

    if (in_window) {
        decision.kind = AdmissionDecision::Kind::kRestore;
        return decision;
    }

    switch (config_.multi_device) {
        case MultiDevicePolicy::kSingle:
            if (live) {
                decision.kind = AdmissionDecision::Kind::kReject;
                decision.code = "already_online";
            }
            break;
        case MultiDevicePolicy::kKickOld:
            if (live) {
                decision.kind = AdmissionDecision::Kind::kKickOld;
                decision.code = "replaced";
                decision.kicked_service_id = entry.ref.service_id;
            }
            break;
        case MultiDevicePolicy::kMulti:
            if (live) {
                auto dev_it = devices_.find(uid);
                const std::size_t count =
                    dev_it == devices_.end() ? 0 : dev_it->second.size();
                const bool known =
                    dev_it != devices_.end() &&
                    std::find(dev_it->second.begin(), dev_it->second.end(),
                              device_id) != dev_it->second.end();
                // A returning device re-occupies its slot; only brand-new
                // devices consume quota.
                if (!known && count >= config_.max_devices) {
                    decision.kind = AdmissionDecision::Kind::kReject;
                    decision.code = "too_many_devices";
                }
            }
            break;
    }
    return decision;
}

void PlayerManager::register_session(const PlayerRef& ref,
                                     const std::string& device_id,
                                     SessionState state,
                                     std::uint64_t /*now_ms*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = sessions_[ref.uid];
    entry.ref = ref;
    entry.state = state;
    entry.device_id = device_id;
    entry.disconnected_ms = 0;
    auto& devs = devices_[ref.uid];
    if (std::find(devs.begin(), devs.end(), device_id) == devs.end()) {
        devs.push_back(device_id);
    }
}

bool PlayerManager::mark_disconnected(const std::string& uid,
                                      std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(uid);
    if (it == sessions_.end() || it->second.disconnected_ms != 0) return false;
    it->second.disconnected_ms = now_ms;
    it->second.state = SessionState::kDisconnected;
    return true;
}

bool PlayerManager::mark_reconnected(const std::string& uid,
                                     std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(uid);
    if (it == sessions_.end() || it->second.disconnected_ms == 0) return false;
    if (now_ms < it->second.disconnected_ms ||
        now_ms - it->second.disconnected_ms > config_.reconnect_window_ms) {
        return false;  // window already expired; caller re-authenticates
    }
    it->second.disconnected_ms = 0;
    it->second.state = SessionState::kOnline;
    return true;
}

bool PlayerManager::set_state(const std::string& uid, SessionState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(uid);
    if (it == sessions_.end()) return false;
    it->second.state = state;
    return true;
}

std::optional<std::string> PlayerManager::unregister(const std::string& uid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(uid);
    if (it == sessions_.end()) return std::nullopt;
    std::string service_id = it->second.ref.service_id;
    sessions_.erase(it);
    devices_.erase(uid);
    return service_id;
}

std::optional<SessionInfo> PlayerManager::get(const std::string& uid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(uid);
    if (it == sessions_.end()) return std::nullopt;
    SessionInfo info;
    info.ref = it->second.ref;
    info.state = it->second.state;
    info.device_id = it->second.device_id;
    return info;
}

std::vector<std::string> PlayerManager::get_devices(
    const std::string& uid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(uid);
    if (it == devices_.end()) return {};
    return it->second;
}

std::optional<SessionInfo> PlayerManager::resolve(const PlayerRef& ref) const {
    if (!ref.node_id.empty() && ref.node_id != node_id_) {
        return std::nullopt;  // remote ref: P0 resolve is local-only
    }
    return get(ref.uid);
}

bool PlayerManager::in_reconnect_window(const std::string& uid,
                                        std::uint64_t now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(uid);
    if (it == sessions_.end() || it->second.disconnected_ms == 0) return false;
    return now_ms >= it->second.disconnected_ms &&
           now_ms - it->second.disconnected_ms <= config_.reconnect_window_ms;
}

std::size_t PlayerManager::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

}  // namespace shield::player
