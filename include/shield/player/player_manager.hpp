#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace shield::player {

/// Cross-service player reference (OD-010): a plain value, never an actor
/// target. `epoch` mirrors the cluster node epoch and stays 0 on a single
/// node.
struct PlayerRef {
    std::string uid;
    std::string node_id;
    std::string service_id;
    std::uint64_t epoch = 0;

    bool operator==(const PlayerRef&) const = default;
};

/// Player session state names exposed to Lua (`player.state`).
/// connecting/authenticating/online are transient; ready is the only state
/// from which client messages are dispatched to business handlers.
enum class SessionState {
    kConnecting,
    kAuthenticating,
    kOnline,
    kReady,
    kDisconnected,
    kAnonymous,
    kSpectator,
};

const char* session_state_name(SessionState state);

/// Multi-device / duplicate-login policy (OD-013).
enum class MultiDevicePolicy { kSingle, kKickOld, kMulti };

/// Module-owned configuration (the `player` config section). Parsed by the
/// module itself from the global config; core config never grows player
/// fields.
struct PlayerConfig {
    bool enabled = false;
    MultiDevicePolicy multi_device = MultiDevicePolicy::kSingle;
    std::uint32_t max_devices = 2;
    bool anonymous_enabled = false;
    bool spectator_enabled = false;
    std::uint64_t reconnect_window_ms = 30000;
    std::uint32_t message_queue_limit = 64;
    // persistence (OD-009): empty binding = not configured -> default save is
    // a no-op.
    std::string persistence_binding;
    std::vector<std::string> persistence_fields;
    bool persistence_panic_on_error = false;  // on_save_error: log | panic
    std::uint64_t save_interval_ms = 60000;   // 0 = save only on logout

    /// Reads the `player` section from the global config. Returns false with
    /// `error` set when a present section fails validation.
    static bool from_global_config(PlayerConfig* out, std::string* error);
};

/// Decision returned by the admission check that runs between the business
/// `auth` hook and the spawn/bind. These codes surface to Lua verbatim.
struct AdmissionDecision {
    enum class Kind {
        kAllow,    // proceed (fresh spawn or restore)
        kRestore,  // uid is inside its reconnect window: reuse the session
        kKickOld,  // allow, but the old session must logout("replaced")
        kReject,   // deny with `code`
    };
    Kind kind = Kind::kAllow;
    std::string code;               // already_online / too_many_devices / ...
    std::string kicked_service_id;  // kKickOld: the live session's service id
};

/// Immutable snapshot of one registered session, returned through
/// resolve()/get().
struct SessionInfo {
    PlayerRef ref;
    SessionState state = SessionState::kConnecting;
    std::string device_id;
};

/// Process-wide player session index. Shield expects one PlayerService actor
/// per online player (AD-02); this index is the shared truth all of them
/// agree on. Single process, plain mutex — C++ state, no CAF messages.
class PlayerManager {
public:
    explicit PlayerManager(PlayerConfig config);

    static PlayerManager* global();
    static void set_global(PlayerManager* manager);

    const PlayerConfig& config() const { return config_; }

    /// Local epoch for PlayerRef values (cluster node epoch when enabled).
    std::uint64_t node_epoch() const;
    /// Local node id for PlayerRef values (empty on a single node).
    std::string node_id() const;
    /// Inject locality. Called by the Lua setup orchestration after the
    /// cluster module (if enabled) has started; single-node stays at the
    /// ("", 0) defaults. Kept as a setter so shield_player never links
    /// against shield_cluster.
    void set_locality(const std::string& node_id, std::uint64_t epoch);

    /// Admission ruling for a (uid, device) login attempt. Applies the
    /// multi-device policy and detects reconnect-window restores.
    AdmissionDecision admit(const std::string& uid,
                            const std::string& device_id, std::uint64_t now_ms);

    /// Record a committed session after spawn+bind succeeded.
    void register_session(const PlayerRef& ref, const std::string& device_id,
                          SessionState state, std::uint64_t now_ms);

    /// Transition an online session to its reconnect window. Returns false
    /// when the uid is unknown or already disconnected.
    bool mark_disconnected(const std::string& uid, std::uint64_t now_ms);

    /// Restore a disconnected session (reconnect within the window).
    bool mark_reconnected(const std::string& uid, std::uint64_t now_ms);

    /// Update state for an existing session (ready/anonymous/...).
    bool set_state(const std::string& uid, SessionState state);

    /// Drop the session index entry. Returns the service id it pointed at.
    std::optional<std::string> unregister(const std::string& uid);

    std::optional<SessionInfo> get(const std::string& uid) const;

    /// All live device ids for a uid under the `multi` policy.
    std::vector<std::string> get_devices(const std::string& uid) const;

    /// Local resolve (OD-010): requires ref.node_id to match this node and
    /// the uid to be indexed.
    std::optional<SessionInfo> resolve(const PlayerRef& ref) const;

    /// True when uid has a session sitting in the reconnect window.
    bool in_reconnect_window(const std::string& uid,
                             std::uint64_t now_ms) const;

    std::size_t size() const;

private:
    struct Entry {
        PlayerRef ref;
        SessionState state = SessionState::kConnecting;
        std::string device_id;
        std::uint64_t disconnected_ms = 0;  // 0 = connected
    };

    PlayerConfig config_;
    std::string node_id_;
    std::uint64_t node_epoch_ = 0;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> sessions_;  // uid -> entry
    // device index for the `multi` policy: uid -> device ids
    std::unordered_map<std::string, std::vector<std::string>> devices_;
};

/// Parse validation for the optional-module guard: the module accepts the
/// `player_manager` section as reserved (currently no fields); unknown keys
/// in `player` fail fast.
bool validate_player_config(const PlayerConfig& config, std::string* error);

}  // namespace shield::player
