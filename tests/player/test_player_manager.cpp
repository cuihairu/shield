// PlayerManager unit tests (shield_player P0-b).
// Constructed directly from PlayerConfig: no global config, no Lua, no
// bootstrap. Config parsing tests opt in via global_config().set().
#define BOOST_TEST_MODULE PlayerManagerTests
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <string>

#include "shield/config/config.hpp"
#include "shield/player/player_manager.hpp"

using shield::player::AdmissionDecision;
using shield::player::MultiDevicePolicy;
using shield::player::PlayerConfig;
using shield::player::PlayerManager;
using shield::player::PlayerRef;
using shield::player::session_state_name;
using shield::player::SessionInfo;
using shield::player::SessionState;
using shield::player::validate_player_config;

namespace {

PlayerConfig default_config() {
    PlayerConfig config;
    config.enabled = true;
    config.multi_device = MultiDevicePolicy::kSingle;
    config.reconnect_window_ms = 30000;
    return config;
}

PlayerRef make_ref(const std::string& uid) {
    PlayerRef ref;
    ref.uid = uid;
    ref.service_id = "player_" + uid;
    return ref;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(PlayerAdmission)

BOOST_AUTO_TEST_CASE(AdmitFreshUidAllows) {
    PlayerManager mgr(default_config());
    auto d = mgr.admit("u1", "dev-1", 1000);
    BOOST_CHECK(d.kind == AdmissionDecision::Kind::kAllow);
    BOOST_CHECK(d.code.empty());
}

BOOST_AUTO_TEST_CASE(AdmitSinglePolicyRejectsOnlineSession) {
    PlayerManager mgr(default_config());
    mgr.register_session(make_ref("u1"), "dev-1", SessionState::kReady, 1000);
    auto d = mgr.admit("u1", "dev-2", 2000);
    BOOST_CHECK(d.kind == AdmissionDecision::Kind::kReject);
    BOOST_CHECK_EQUAL(d.code, "already_online");
}

BOOST_AUTO_TEST_CASE(AdmitSinglePolicyAllowsAfterWindowExpiry) {
    PlayerManager mgr(default_config());
    mgr.register_session(make_ref("u1"), "dev-1", SessionState::kReady, 1000);
    BOOST_CHECK(mgr.mark_disconnected("u1", 2000));
    // Window (30s) still open: restore wins over a fresh login.
    auto during = mgr.admit("u1", "dev-2", 2000 + 29999);
    BOOST_CHECK(during.kind == AdmissionDecision::Kind::kRestore);
    // One ms past the window: a fresh login is admitted again.
    auto after = mgr.admit("u1", "dev-2", 2000 + 30001);
    BOOST_CHECK(after.kind == AdmissionDecision::Kind::kAllow);
}

BOOST_AUTO_TEST_CASE(AdmitKickOldReportsLiveSessionServiceId) {
    PlayerConfig config = default_config();
    config.multi_device = MultiDevicePolicy::kKickOld;
    PlayerManager mgr(config);
    mgr.register_session(make_ref("u1"), "dev-1", SessionState::kReady, 1000);
    auto d = mgr.admit("u1", "dev-2", 2000);
    BOOST_CHECK(d.kind == AdmissionDecision::Kind::kKickOld);
    BOOST_CHECK_EQUAL(d.code, "replaced");
    BOOST_CHECK_EQUAL(d.kicked_service_id, "player_u1");
    // A disconnected session does not need kicking.
    BOOST_CHECK(mgr.mark_disconnected("u1", 3000));
    auto d2 = mgr.admit("u1", "dev-2", 4000);
    // Inside the window the new login restores instead.
    BOOST_CHECK(d2.kind == AdmissionDecision::Kind::kRestore);
}

BOOST_AUTO_TEST_CASE(AdmitMultiPolicyCountsDevices) {
    PlayerConfig config = default_config();
    config.multi_device = MultiDevicePolicy::kMulti;
    config.max_devices = 2;
    PlayerManager mgr(config);
    mgr.register_session(make_ref("u1"), "dev-1", SessionState::kReady, 1000);
    // A returning device re-occupies its own slot.
    auto again = mgr.admit("u1", "dev-1", 1500);
    BOOST_CHECK(again.kind == AdmissionDecision::Kind::kAllow);
    // Second distinct device fills the quota.
    mgr.register_session(make_ref("u1"), "dev-2", SessionState::kReady, 1600);
    // A third device is rejected.
    auto third = mgr.admit("u1", "dev-3", 2000);
    BOOST_CHECK(third.kind == AdmissionDecision::Kind::kReject);
    BOOST_CHECK_EQUAL(third.code, "too_many_devices");
    // A disconnect puts the uid into the reconnect window; a login on yet
    // another device inside the window restores the live instance (device
    // switch), it does not tear it down.
    BOOST_CHECK(mgr.mark_disconnected("u1", 2100));
    auto rejoin = mgr.admit("u1", "dev-3", 2200);
    BOOST_CHECK(rejoin.kind == AdmissionDecision::Kind::kRestore);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(PlayerSessionLifecycle)

BOOST_AUTO_TEST_CASE(RegisterGetAndStateTransitions) {
    PlayerManager mgr(default_config());
    mgr.register_session(make_ref("u1"), "dev-1", SessionState::kOnline, 1000);
    BOOST_CHECK_EQUAL(mgr.size(), 1u);

    auto info = mgr.get("u1");
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK_EQUAL(info->ref.uid, "u1");
    BOOST_CHECK_EQUAL(info->ref.service_id, "player_u1");
    BOOST_CHECK(info->state == SessionState::kOnline);
    BOOST_CHECK_EQUAL(info->device_id, "dev-1");

    BOOST_CHECK(mgr.set_state("u1", SessionState::kReady));
    BOOST_CHECK(mgr.get("u1")->state == SessionState::kReady);
    BOOST_CHECK(!mgr.set_state("missing", SessionState::kReady));
}

BOOST_AUTO_TEST_CASE(DisconnectedAndReconnectWindow) {
    PlayerManager mgr(default_config());
    BOOST_CHECK(!mgr.mark_disconnected("ghost", 1000));
    mgr.register_session(make_ref("u1"), "dev-1", SessionState::kReady, 1000);
    BOOST_CHECK(!mgr.in_reconnect_window("u1", 1500));
    BOOST_CHECK(mgr.mark_disconnected("u1", 2000));
    BOOST_CHECK(mgr.get("u1")->state == SessionState::kDisconnected);
    BOOST_CHECK(mgr.in_reconnect_window("u1", 2000 + 30000));
    BOOST_CHECK(!mgr.in_reconnect_window("u1", 2000 + 30001));

    BOOST_CHECK(mgr.mark_reconnected("u1", 5000));
    BOOST_CHECK(mgr.get("u1")->state == SessionState::kOnline);
    BOOST_CHECK(!mgr.in_reconnect_window("u1", 6000));

    // Reconnect after the window expired must fail: the caller
    // re-authenticates instead.
    mgr.register_session(make_ref("u2"), "dev-2", SessionState::kReady, 1000);
    BOOST_CHECK(mgr.mark_disconnected("u2", 2000));
    BOOST_CHECK(!mgr.mark_reconnected("u2", 2000 + 30001));
    BOOST_CHECK(!mgr.mark_reconnected("ghost", 100));
}

BOOST_AUTO_TEST_CASE(UnregisterReturnsServiceIdAndClearsDevices) {
    PlayerConfig config = default_config();
    config.multi_device = MultiDevicePolicy::kMulti;
    PlayerManager mgr(config);
    BOOST_CHECK(!mgr.unregister("ghost").has_value());
    mgr.register_session(make_ref("u1"), "dev-1", SessionState::kReady, 1000);
    mgr.register_session(make_ref("u1"), "dev-2", SessionState::kReady, 1000);
    BOOST_CHECK_EQUAL(mgr.get_devices("u1").size(), 2u);
    auto sid = mgr.unregister("u1");
    BOOST_REQUIRE(sid.has_value());
    BOOST_CHECK_EQUAL(*sid, "player_u1");
    BOOST_CHECK_EQUAL(mgr.size(), 0u);
    BOOST_CHECK(mgr.get_devices("u1").empty());
    BOOST_CHECK(!mgr.get("u1").has_value());
}

BOOST_AUTO_TEST_CASE(ResolveLocalOnly) {
    PlayerManager mgr(default_config());
    mgr.set_locality("node-a", 7);
    BOOST_CHECK_EQUAL(mgr.node_id(), "node-a");
    BOOST_CHECK_EQUAL(mgr.node_epoch(), 7u);

    mgr.register_session(make_ref("u1"), "dev-1", SessionState::kReady, 1000);

    PlayerRef local = make_ref("u1");
    local.node_id = "node-a";
    local.epoch = 7;
    auto local_info = mgr.resolve(local);
    BOOST_REQUIRE(local_info.has_value());
    BOOST_CHECK_EQUAL(local_info->ref.uid, "u1");

    // Empty node_id means "this node" for a single-node ref.
    auto bare = mgr.resolve(make_ref("u1"));
    BOOST_CHECK(bare.has_value());

    PlayerRef remote = make_ref("u1");
    remote.node_id = "node-b";
    BOOST_CHECK(!mgr.resolve(remote).has_value());

    BOOST_CHECK(!mgr.resolve(make_ref("ghost")).has_value());
}

BOOST_AUTO_TEST_CASE(GetDevicesEmptyForUnknown) {
    PlayerManager mgr(default_config());
    BOOST_CHECK(mgr.get_devices("ghost").empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(PlayerConfigParsing)

BOOST_AUTO_TEST_CASE(FromGlobalConfigParsesSection) {
    auto& cfg = shield::config::global_config();
    cfg.set("player.multi_device", std::string("kick_old"));
    cfg.set("player.max_devices", std::string("3"));
    cfg.set("player.anonymous", std::string("true"));
    cfg.set("player.spectator", std::string("false"));
    cfg.set("player.reconnect_window_ms", std::string("1500"));
    cfg.set("player.message_queue_limit", std::string("8"));
    cfg.set("player.persistence.binding", std::string("db-main"));
    cfg.set("player.persistence.on_save_error", std::string("panic"));
    cfg.set("player.persistence.save_interval_ms", std::string("0"));

    PlayerConfig config;
    std::string error;
    BOOST_REQUIRE(PlayerConfig::from_global_config(&config, &error));
    BOOST_CHECK(config.enabled);
    BOOST_CHECK(config.multi_device == MultiDevicePolicy::kKickOld);
    BOOST_CHECK_EQUAL(config.max_devices, 3u);
    BOOST_CHECK(config.anonymous_enabled);
    BOOST_CHECK(!config.spectator_enabled);
    BOOST_CHECK_EQUAL(config.reconnect_window_ms, 1500u);
    BOOST_CHECK_EQUAL(config.message_queue_limit, 8u);
    BOOST_CHECK_EQUAL(config.persistence_binding, "db-main");
    BOOST_CHECK(config.persistence_panic_on_error);
    BOOST_CHECK_EQUAL(config.save_interval_ms, 0u);
}

BOOST_AUTO_TEST_CASE(FromGlobalConfigRejectsBadMultiDevice) {
    auto& cfg = shield::config::global_config();
    cfg.set("player.multi_device", std::string("whatever"));
    PlayerConfig config;
    std::string error;
    BOOST_CHECK(!PlayerConfig::from_global_config(&config, &error));
    BOOST_CHECK(error.find("multi_device") != std::string::npos);
    cfg.set("player.multi_device", std::string("single"));
}

BOOST_AUTO_TEST_CASE(FromGlobalConfigRejectsBadOnSaveError) {
    auto& cfg = shield::config::global_config();
    cfg.set("player.persistence.on_save_error", std::string("explode"));
    PlayerConfig config;
    std::string error;
    BOOST_CHECK(!PlayerConfig::from_global_config(&config, &error));
    BOOST_CHECK(error.find("on_save_error") != std::string::npos);
    cfg.set("player.persistence.on_save_error", std::string("log"));
}

BOOST_AUTO_TEST_CASE(ValidateRejectsZeroMaxDevicesForMulti) {
    PlayerConfig config = default_config();
    config.multi_device = MultiDevicePolicy::kMulti;
    config.max_devices = 0;
    std::string error;
    BOOST_CHECK(!validate_player_config(config, &error));
    BOOST_CHECK(error.find("max_devices") != std::string::npos);

    config.max_devices = 1;
    BOOST_CHECK(validate_player_config(config, &error));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(PlayerMisc)

BOOST_AUTO_TEST_CASE(StateNamesCoverAllStates) {
    BOOST_CHECK_EQUAL(session_state_name(SessionState::kConnecting),
                      "connecting");
    BOOST_CHECK_EQUAL(session_state_name(SessionState::kAuthenticating),
                      "authenticating");
    BOOST_CHECK_EQUAL(session_state_name(SessionState::kOnline), "online");
    BOOST_CHECK_EQUAL(session_state_name(SessionState::kReady), "ready");
    BOOST_CHECK_EQUAL(session_state_name(SessionState::kDisconnected),
                      "disconnected");
    BOOST_CHECK_EQUAL(session_state_name(SessionState::kAnonymous),
                      "anonymous");
    BOOST_CHECK_EQUAL(session_state_name(SessionState::kSpectator),
                      "spectator");
}

BOOST_AUTO_TEST_CASE(GlobalAccessorsRoundTrip) {
    BOOST_CHECK(PlayerManager::global() == nullptr);
    PlayerManager mgr(default_config());
    PlayerManager::set_global(&mgr);
    BOOST_CHECK(PlayerManager::global() == &mgr);
    PlayerManager::set_global(nullptr);
    BOOST_CHECK(PlayerManager::global() == nullptr);
}

BOOST_AUTO_TEST_CASE(RefEqualityIsValueBased) {
    PlayerRef a = make_ref("u1");
    PlayerRef b = make_ref("u1");
    b.node_id = "";
    BOOST_CHECK(a == b);
    b.node_id = "node-x";
    BOOST_CHECK(!(a == b));
}

BOOST_AUTO_TEST_SUITE_END()
