// LAPI-009: Gateway API tests.
//
// Exercises the M3 typed client-RPC dispatch: the bridge delivers
// ClientControlMessage (bound / disconnected) and ClientIngress to the
// target service's actor; ingress routes through the spawn-time compiled
// handler table as handler(ctx, client, request). Direct manager.call()
// cases use the same (client, request) shape with plain-table clients.

#define BOOST_TEST_MODULE LuaApiGatewayTests
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shield/caf_initializer.hpp"
#include "shield/lua/gateway_actor.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_gateway_bridge.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/session.hpp"
#include "shield/plugin/protocol_codec.h"
#include "shield/transport/protocol.hpp"

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";

// The service's inbound route set: 4097 (0x1001) carries structured /
// JSON-decodable payloads, 4098 (0x1002) raw strings.
nlohmann::json opts_for(const std::string& name,
                        nlohmann::json config = nlohmann::json::object()) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", std::move(config)},
        {"rpc",
         {{"routes", nlohmann::json::array({
                         nlohmann::json{
                             {"id", 4097},
                             {"name", "gw_move"},
                             {"direction", "c2s"},
                             {"binding", "gw_move"},
                             {"requires_auth", false},
                         },
                         nlohmann::json{
                             {"id", 4098},
                             {"name", "gw_raw_echo"},
                             {"direction", "c2s"},
                             {"binding", "gw_raw_echo"},
                             {"requires_auth", false},
                             {"request_codec", "raw"},
                         },
                     })}}},
    };
}

SpawnResult spawn_gateway(LuaServiceManager& manager, const std::string& name,
                          nlohmann::json config = nlohmann::json::object()) {
    return manager.spawn(TEST_SCRIPTS_DIR + "gateway_service.lua",
                         opts_for(name, std::move(config)).dump());
}

// Table-form client identity for direct manager.call() cases (the bridge
// path materializes a real ClientContext userdata instead; both are keyed by
// session_id inside the script).
nlohmann::json client_table(uint64_t session_id,
                            const std::string& gateway = "") {
    return {{"session_id", session_id},
            {"session_epoch", 0},
            {"player_id", ""},
            {"gateway_service", gateway}};
}

class MockSession final : public shield::net::Session {
public:
    MockSession(shield::net::SessionId id, shield::net::RemoteAddress remote,
                bool protocol_enabled = false,
                std::string protocol_codec = "json")
        : id_(id),
          remote_(std::move(remote)),
          protocol_enabled_(protocol_enabled),
          protocol_codec_(std::move(protocol_codec)) {}

    shield::net::SessionId id() const override { return id_; }
    shield::net::RemoteAddress remote_addr() const override { return remote_; }
    bool send(const std::vector<uint8_t>& data,
              std::string* error = nullptr) override {
        if (!alive_) {
            if (error) {
                *error = "session is closed";
            }
            return false;
        }
        sent_.push_back(data);
        return true;
    }
    void close(std::string reason) override {
        alive_ = false;
        close_reason_ = std::move(reason);
    }
    bool is_alive() const override { return alive_; }
    std::string error_code() const override {
        return alive_ ? "" : "session_closed";
    }
    bool has_protocol_pipeline() const override { return protocol_enabled_; }
    std::string_view protocol_codec_name() const override {
        return protocol_enabled_ ? std::string_view(protocol_codec_)
                                 : std::string_view{};
    }
    bool send_message(const shield::transport::DecodedBody& message,
                      std::string* error) override {
        if (!alive_) {
            if (error) {
                *error = "session is closed";
            }
            return false;
        }
        sent_messages_.push_back(message);
        return true;
    }
    void set_user_data(std::string key, std::string value) override {
        user_data_[std::move(key)] = std::move(value);
    }
    std::string get_user_data(std::string_view key) const override {
        auto it = user_data_.find(std::string(key));
        return it == user_data_.end() ? "" : it->second;
    }

    shield::net::SessionBinding binding() const override { return binding_; }
    void reset_binding(shield::net::SessionBinding initial) override {
        binding_ = std::move(initial);
    }
    bool apply_binding(std::string target_service, std::string player_id,
                       uint32_t expected_epoch,
                       shield::net::SessionBinding* out) override {
        if (expected_epoch != shield::net::kAnyEpoch &&
            expected_epoch != binding_.epoch) {
            return false;
        }
        binding_.target_service = std::move(target_service);
        binding_.player_id = std::move(player_id);
        ++binding_.epoch;
        if (out != nullptr) {
            *out = binding_;
        }
        return true;
    }

    const std::vector<std::vector<uint8_t>>& sent() const { return sent_; }
    const std::vector<shield::transport::DecodedBody>& sent_messages() const {
        return sent_messages_;
    }

private:
    shield::net::SessionId id_;
    shield::net::RemoteAddress remote_;
    bool protocol_enabled_ = false;
    std::string protocol_codec_;
    bool alive_ = true;
    std::string close_reason_;
    std::vector<std::vector<uint8_t>> sent_;
    std::vector<shield::transport::DecodedBody> sent_messages_;
    std::unordered_map<std::string, std::string> user_data_;
    shield::net::SessionBinding binding_;
};

char* dup_protocol_json(std::string_view value) {
    auto* out = static_cast<char*>(std::malloc(value.size() + 1));
    if (out == nullptr) return nullptr;
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return out;
}

std::uint8_t* dup_protocol_payload(std::string_view value) {
    if (value.empty()) return nullptr;
    auto* out = static_cast<std::uint8_t*>(std::malloc(value.size()));
    if (out == nullptr) return nullptr;
    std::memcpy(out, value.data(), value.size());
    return out;
}

struct FakeProtocolCodecState {
    std::string decoded_json = R"json({"uid":7,"name":"alice"})json";
    std::string encoded_payload;
    std::string last_input_json;
};

int fake_protocol_decode(const shield_protocol_codec_v1* self,
                         const shield_protocol_decode_args_v1* args,
                         shield_protocol_decode_result_v1* out,
                         shield_error_v1*) {
    if (self == nullptr || args == nullptr || out == nullptr ||
        self->user_data == nullptr) {
        return -1;
    }
    auto* state = static_cast<FakeProtocolCodecState*>(self->user_data);
    out->message_json = dup_protocol_json(state->decoded_json);
    out->message_json_size = state->decoded_json.size();
    return out->message_json == nullptr ? -1 : 0;
}

int fake_protocol_encode(const shield_protocol_codec_v1* self,
                         const shield_protocol_encode_args_v1* args,
                         shield_protocol_encode_result_v1* out,
                         shield_error_v1*) {
    if (self == nullptr || args == nullptr || out == nullptr ||
        self->user_data == nullptr) {
        return -1;
    }
    auto* state = static_cast<FakeProtocolCodecState*>(self->user_data);
    if (args->message_json != nullptr && args->message_json_size > 0) {
        state->last_input_json.assign(
            args->message_json, args->message_json + args->message_json_size);
    } else {
        state->last_input_json = "{}";
    }
    state->encoded_payload = "pb:" + state->last_input_json;
    out->payload = dup_protocol_payload(state->encoded_payload);
    out->payload_size = state->encoded_payload.size();
    return out->payload == nullptr ? -1 : 0;
}

void fake_free_decode_result(const shield_protocol_codec_v1*,
                             shield_protocol_decode_result_v1* result) {
    if (result == nullptr) return;
    std::free(const_cast<char*>(result->message_json));
    result->message_json = nullptr;
    result->message_json_size = 0;
}

void fake_free_encode_result(const shield_protocol_codec_v1*,
                             shield_protocol_encode_result_v1* result) {
    if (result == nullptr) return;
    std::free(const_cast<std::uint8_t*>(result->payload));
    result->payload = nullptr;
    result->payload_size = 0;
}

shield_protocol_codec_v1 make_fake_protocol_codec(
    FakeProtocolCodecState& state) {
    shield_protocol_codec_v1 codec{};
    codec.struct_size = sizeof(shield_protocol_codec_v1);
    codec.codec_name = "protobuf";
    codec.version = "test";
    codec.user_data = &state;
    codec.decode = fake_protocol_decode;
    codec.encode = fake_protocol_encode;
    codec.free_decode_result = fake_free_decode_result;
    codec.free_encode_result = fake_free_encode_result;
    return codec;
}

std::unique_ptr<shield::transport::ProtocolPipeline>
make_fake_protobuf_pipeline(const shield_protocol_codec_v1* codec,
                            std::string* error = nullptr) {
    // Inline protocol.routes was folded into the RPC descriptor set; the
    // transport-level route this codec test needs is upserted directly.
    const auto config = R"json(
{
  "name": "lua.protobuf",
  "envelope": {
    "type": "idlen",
    "route_id_bytes": 2,
    "length_bytes": 2
  },
  "body": {
    "codec": "protobuf",
    "provider": "protocol.protobuf"
  }
}
)json";
    shield::transport::ProtocolBuildOptions options;
    options.external_codec_resolver =
        [codec](std::string_view provider, std::string_view codec_name,
                std::string*) -> const shield_protocol_codec_v1* {
        if (provider != "protocol.protobuf" || codec_name != "protobuf") {
            return nullptr;
        }
        return codec;
    };
    auto pipeline = shield::transport::build_protocol_pipeline_from_json(
        config, options, error);
    if (pipeline != nullptr) {
        shield::transport::RouteEntry entry;
        entry.route_id = 4097;
        entry.debug_name = "shield.test.Login";
        entry.schema_id = 42;
        entry.direction = shield::transport::RouteDirection::ClientToServer;
        entry.requires_auth = false;
        entry.policy.lazy_decode = false;
        pipeline->routes().upsert(entry);
    }
    return pipeline;
}

bool wait_until(std::function<bool()> predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

CallResult get_sessions(LuaServiceManager& manager,
                        const std::string& service_id) {
    return manager.call(service_id, "get_sessions", nlohmann::json::array());
}

}  // namespace

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

BOOST_AUTO_TEST_SUITE(Lapi009GatewayApi)

// ---------------------------------------------------------------------------
// LAPI_009_01: the bound handler registers a simulated client.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LAPI_009_01_ClientBoundHandler) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_gateway(manager, "gw_connect");
    BOOST_REQUIRE(result.success);

    CallResult cr = manager.call(result.service_id, "on_client_bound",
                                 nlohmann::json::array({client_table(1)}));
    BOOST_CHECK(cr.success);

    CallResult sessions = get_sessions(manager, result.service_id);
    BOOST_REQUIRE(sessions.success);
    BOOST_REQUIRE(sessions.values.is_array());
    BOOST_REQUIRE(!sessions.values.empty());
    BOOST_CHECK(sessions.values[0].contains("1"));
    BOOST_CHECK(sessions.values[0]["1"]["connected"].get<bool>());
}

// ---------------------------------------------------------------------------
// LAPI_009_02: ingress request forms — decoded table, JSON-decodable body,
// and raw string all reach the compiled handler.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LAPI_009_02_IngressRequestForms) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_gateway(manager, "gw_message");
    BOOST_REQUIRE(result.success);

    manager.call(result.service_id, "on_client_bound",
                 nlohmann::json::array({client_table(2)}));

    // Structured request (what the bridge passes when a pipeline codec
    // already decoded the body).
    CallResult table_form = manager.call(
        result.service_id, "gw_move",
        nlohmann::json::array(
            {client_table(2), nlohmann::json::object({{"uid", 7}})}));
    BOOST_CHECK(table_form.success);

    // Raw string request (raw codec route).
    CallResult raw_form =
        manager.call(result.service_id, "gw_raw_echo",
                     nlohmann::json::array({client_table(2), "opaque-bytes"}));
    BOOST_CHECK(raw_form.success);

    CallResult sessions = get_sessions(manager, result.service_id);
    BOOST_REQUIRE(sessions.success);
    const auto& state = sessions.values[0]["2"];
    // The raw echo ran last, so it owns last_message: string request on the
    // raw route (4098), untouched by the structured call before it.
    BOOST_CHECK_EQUAL(state["last_message"]["route_id"].get<uint32_t>(), 4098u);
    BOOST_CHECK(state["last_message"]["request"].is_string());
    BOOST_CHECK_EQUAL(state["last_message"]["request"].get<std::string>(),
                      "opaque-bytes");
}

// ---------------------------------------------------------------------------
// LAPI_009_03: Disconnect handler.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LAPI_009_03_DisconnectHandler) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_gateway(manager, "gw_disconnect");
    BOOST_REQUIRE(result.success);

    manager.call(result.service_id, "on_client_bound",
                 nlohmann::json::array({client_table(3)}));

    CallResult cr =
        manager.call(result.service_id, "on_disconnect",
                     nlohmann::json::array({client_table(3), "client_closed"}));
    BOOST_CHECK(cr.success);

    CallResult sessions = get_sessions(manager, result.service_id);
    BOOST_REQUIRE(sessions.success);
    const auto& state = sessions.values[0]["3"];
    BOOST_CHECK(!state["connected"].get<bool>());
    BOOST_CHECK_EQUAL(state["disconnect_reason"].get<std::string>(),
                      "client_closed");
}

// ---------------------------------------------------------------------------
// LAPI_009_04: the kick path — on_client_unbound records the reason.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LAPI_009_04_UnboundHandler) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_gateway(manager, "gw_unbound");
    BOOST_REQUIRE(result.success);

    manager.call(result.service_id, "on_client_bound",
                 nlohmann::json::array({client_table(4)}));

    CallResult cr =
        manager.call(result.service_id, "on_client_unbound",
                     nlohmann::json::array({client_table(4), "kicked"}));
    BOOST_CHECK(cr.success);

    CallResult sessions = get_sessions(manager, result.service_id);
    BOOST_REQUIRE(sessions.success);
    const auto& state = sessions.values[0]["4"];
    BOOST_CHECK(!state["connected"].get<bool>());
    BOOST_CHECK_EQUAL(state["unbound_reason"].get<std::string>(), "kicked");
}

// ---------------------------------------------------------------------------
// LAPI_009_05: an unknown client (no session record) is handled gracefully.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LAPI_009_05_UnknownClientHandled) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_gateway(manager, "gw_stale");
    BOOST_REQUIRE(result.success);

    // Message for a client that never bound: recorded as a no-op, no crash.
    CallResult cr = manager.call(
        result.service_id, "gw_move",
        nlohmann::json::array(
            {client_table(5), nlohmann::json::object({{"late", true}})}));
    BOOST_CHECK(cr.success);

    CallResult sessions = get_sessions(manager, result.service_id);
    BOOST_REQUIRE(sessions.success);
    BOOST_CHECK(sessions.values[0].empty());
}

// ---------------------------------------------------------------------------
// Real bridge path: on_connect installs the binding and delivers
// ClientControlMessage::Bound; on_packet routes typed ingress through the
// compiled handler table.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LuaGatewayBridgeQueuesTypedClientMessages) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_gateway(manager, "gw_bridge");
    BOOST_REQUIRE(result.success);

    LuaGatewayBridge bridge(
        manager, result.service_id,
        std::make_shared<shield::lua::GatewaySessionRegistry>());
    auto session = std::make_shared<MockSession>(
        42, shield::net::RemoteAddress{"127.0.0.1", 34567});

    bridge.on_connect(session);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult sessions = get_sessions(manager, result.service_id);
            return sessions.success && sessions.values.is_array() &&
                   sessions.values.size() == 1u &&
                   sessions.values[0].is_object() &&
                   sessions.values[0].contains("42");
        },
        std::chrono::seconds(1)));

    // A decoded-local packet with a JSON-decodable body: the handler must
    // receive the parsed request table.
    shield::transport::DispatchResult dispatch;
    dispatch.action = shield::transport::RouteAction::DecodeLocal;
    dispatch.packet.route_id = 4097;
    dispatch.packet.body =
        std::vector<std::uint8_t>{'{', '"', 'u', 'i', 'd', '"', ':', '7', '}'};

    shield::transport::RouteEntry route;
    route.route_id = 4097;
    route.direction = shield::transport::RouteDirection::ClientToServer;
    route.requires_auth = false;
    dispatch.route = &route;
    dispatch.decoded_body = shield::transport::DecodedBody{
        .bytes = std::vector<std::uint8_t>{'{', '"', 'u', 'i', 'd', '"', ':',
                                           '7', '}'},
    };

    bridge.on_packet(session, dispatch);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult sessions = get_sessions(manager, result.service_id);
            if (!sessions.success || !sessions.values.is_array() ||
                sessions.values.size() < 1u) {
                return false;
            }
            if (!sessions.values[0].contains("42")) {
                return false;
            }
            if (!sessions.values[0]["42"].contains("last_message")) {
                return false;
            }
            const auto& last = sessions.values[0]["42"]["last_message"];
            return last["route_id"].get<uint32_t>() == 4097u &&
                   last["request"].is_object() &&
                   last["request"]["uid"].get<int>() == 7;
        },
        std::chrono::seconds(1)));

    // Disconnect goes out as ClientControlMessage::Disconnected to the live
    // target.
    bridge.on_disconnect(session, "bye");

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult sessions = get_sessions(manager, result.service_id);
            if (!sessions.success || !sessions.values.is_array() ||
                sessions.values.size() < 1u ||
                !sessions.values[0].contains("42")) {
                return false;
            }
            const auto& state = sessions.values[0]["42"];
            return !state["connected"].get<bool>() &&
                   state["disconnect_reason"].get<std::string>() == "bye";
        },
        std::chrono::seconds(1)));
}

// ---------------------------------------------------------------------------
// A body that does not parse as JSON on a JSON-codec route arrives as a raw
// string (the descriptor contract's fallback form).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LuaGatewayBridgeDeliversNonJsonBodyAsString) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_gateway(manager, "gw_string_body");
    BOOST_REQUIRE(result.success);

    LuaGatewayBridge bridge(
        manager, result.service_id,
        std::make_shared<shield::lua::GatewaySessionRegistry>());
    auto session = std::make_shared<MockSession>(
        44, shield::net::RemoteAddress{"127.0.0.1", 34569});

    bridge.on_connect(session);
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult sessions = get_sessions(manager, result.service_id);
            return sessions.success && sessions.values.is_array() &&
                   sessions.values.size() >= 1u &&
                   sessions.values[0].contains("44");
        },
        std::chrono::seconds(1)));

    shield::transport::DispatchResult dispatch;
    dispatch.action = shield::transport::RouteAction::DecodeLocal;
    dispatch.packet.route_id = 4097;
    dispatch.packet.body =
        std::vector<std::uint8_t>{'r', 'a', 'w', '_', 't', 'e', 'x', 't'};
    shield::transport::RouteEntry route;
    route.route_id = 4097;
    route.direction = shield::transport::RouteDirection::ClientToServer;
    route.requires_auth = false;
    dispatch.route = &route;
    dispatch.decoded_body = shield::transport::DecodedBody{
        .bytes =
            std::vector<std::uint8_t>{'r', 'a', 'w', '_', 't', 'e', 'x', 't'},
    };

    bridge.on_packet(session, dispatch);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult sessions = get_sessions(manager, result.service_id);
            if (!sessions.success || !sessions.values.is_array() ||
                sessions.values.size() < 1u ||
                !sessions.values[0].contains("44")) {
                return false;
            }
            const auto& state = sessions.values[0]["44"];
            if (!state.contains("last_message")) {
                return false;
            }
            const auto& last = state["last_message"];
            return last["request"].is_string() &&
                   last["request"].get<std::string>() == "raw_text";
        },
        std::chrono::seconds(1)));
}

// ---------------------------------------------------------------------------
// A raw-codec route (request_codec "raw") always delivers the bytes as a
// string, never a decoded table.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LuaGatewayBridgeRoutesRawDecodeLocalAsStrings) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_gateway(manager, "gw_raw_packet_bridge");
    BOOST_REQUIRE(result.success);

    LuaGatewayBridge bridge(
        manager, result.service_id,
        std::make_shared<shield::lua::GatewaySessionRegistry>());
    auto session = std::make_shared<MockSession>(
        78, shield::net::RemoteAddress{"127.0.0.1", 45679});

    bridge.on_connect(session);
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult sessions = get_sessions(manager, result.service_id);
            return sessions.success && sessions.values.is_array() &&
                   sessions.values.size() >= 1u &&
                   sessions.values[0].contains("78");
        },
        std::chrono::seconds(1)));

    shield::transport::DispatchResult dispatch;
    dispatch.action = shield::transport::RouteAction::DecodeLocal;
    dispatch.packet.route_id = 4098;
    dispatch.packet.body = std::vector<std::uint8_t>{'r', 'a', 'w'};

    shield::transport::RouteEntry route;
    route.route_id = 4098;
    route.direction = shield::transport::RouteDirection::ClientToServer;
    route.requires_auth = false;
    route.codec_id = 1;
    route.schema_id = 0;
    route.debug_name = "raw.echo";
    dispatch.route = &route;
    dispatch.decoded_body = shield::transport::DecodedBody{
        .codec_id = 1,
        .schema_id = 0,
        .route_name = "raw.echo",
        .bytes = std::vector<std::uint8_t>{'r', 'a', 'w'},
    };

    bridge.on_packet(session, dispatch);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult sessions = get_sessions(manager, result.service_id);
            if (!sessions.success || !sessions.values.is_array() ||
                sessions.values.size() < 1u ||
                !sessions.values[0].contains("78")) {
                return false;
            }
            const auto& state = sessions.values[0]["78"];
            if (!state.contains("last_message")) {
                return false;
            }
            const auto& last = state["last_message"];
            return last["route_id"].get<uint32_t>() == 4098u &&
                   last["request"].is_string() &&
                   last["request"].get<std::string>() == "raw";
        },
        std::chrono::seconds(1)));
}

// ---------------------------------------------------------------------------
// A pipeline codec's canonical JSON message rides along as the request table.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(
    LuaGatewayBridgeRoutesFakeProtobufPipelinePacketsWithDecodedTable) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_gateway(manager, "gw_protobuf_pipeline");
    BOOST_REQUIRE(result.success);

    FakeProtocolCodecState codec_state;
    auto codec = make_fake_protocol_codec(codec_state);
    std::string protocol_error;
    auto pipeline = make_fake_protobuf_pipeline(&codec, &protocol_error);
    BOOST_REQUIRE_MESSAGE(pipeline != nullptr, protocol_error);

    LuaGatewayBridge bridge(
        manager, result.service_id,
        std::make_shared<shield::lua::GatewaySessionRegistry>());
    auto session = std::make_shared<MockSession>(
        79, shield::net::RemoteAddress{"127.0.0.1", 45680}, true, "protobuf");

    bridge.on_connect(session);
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult sessions = get_sessions(manager, result.service_id);
            return sessions.success && sessions.values.is_array() &&
                   sessions.values.size() >= 1u &&
                   sessions.values[0].contains("79");
        },
        std::chrono::seconds(1)));

    shield::transport::Packet packet;
    packet.route_id = 4097;
    packet.body = std::vector<std::uint8_t>{'p', 'b'};
    const auto frame = pipeline->encode(packet.ref());
    BOOST_REQUIRE_MESSAGE(pipeline->error().empty(), pipeline->error());

    auto dispatches = pipeline->feed(frame.data(), frame.size());
    BOOST_REQUIRE_EQUAL(dispatches.size(), 1u);
    BOOST_REQUIRE(dispatches[0].ok());
    BOOST_REQUIRE(dispatches[0].decoded());
    BOOST_REQUIRE(dispatches[0].decoded_body->has_message());

    bridge.on_packet(session, dispatches[0]);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult sessions = get_sessions(manager, result.service_id);
            if (!sessions.success || !sessions.values.is_array() ||
                sessions.values.size() < 1u ||
                !sessions.values[0].contains("79")) {
                return false;
            }
            const auto& state = sessions.values[0]["79"];
            if (!state.contains("last_message")) {
                return false;
            }
            // The pipeline's fake codec decoded the payload into the
            // canonical JSON message {"uid":7,"name":"alice"}; the handler
            // must receive it as the request table.
            const auto& last = state["last_message"];
            return last["route_id"].get<uint32_t>() == 4097u &&
                   last["request"].is_object() &&
                   last["request"]["uid"].get<int>() == 7 &&
                   last["request"]["name"].get<std::string>() == "alice";
        },
        std::chrono::seconds(1)));
}

// ---------------------------------------------------------------------------
// Forward-raw packets are transport-relayed, never exposed to Lua.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(
    LuaGatewayBridgeDoesNotExposeForwardRawProtocolPacketsToLua) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_gateway(manager, "gw_forward_raw_drop");
    BOOST_REQUIRE(result.success);

    LuaGatewayBridge bridge(
        manager, result.service_id,
        std::make_shared<shield::lua::GatewaySessionRegistry>());
    auto session = std::make_shared<MockSession>(
        88, shield::net::RemoteAddress{"127.0.0.1", 56789});

    bridge.on_connect(session);
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult sessions = get_sessions(manager, result.service_id);
            return sessions.success && sessions.values.is_array() &&
                   sessions.values.size() >= 1u &&
                   sessions.values[0].contains("88");
        },
        std::chrono::seconds(1)));

    shield::transport::DispatchResult dispatch;
    dispatch.action = shield::transport::RouteAction::ForwardRaw;
    dispatch.packet.route_id = 0x2002;
    dispatch.packet.body = std::vector<std::uint8_t>{'r', 'a', 'w'};
    dispatch.packet.raw_frame =
        std::vector<std::uint8_t>{'f', 'r', 'a', 'm', 'e'};

    bridge.on_packet(session, dispatch);

    // Wait briefly for any potential message to be processed.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CallResult sessions = get_sessions(manager, result.service_id);
    BOOST_REQUIRE(sessions.success);
    BOOST_REQUIRE(sessions.values.is_array());
    BOOST_REQUIRE_EQUAL(sessions.values.size(), 1u);
    BOOST_REQUIRE(sessions.values[0].contains("88"));

    const auto& state = sessions.values[0]["88"];
    BOOST_CHECK(!state.contains("last_message"));
    BOOST_CHECK(!state.contains("last_packet"));
}

BOOST_AUTO_TEST_SUITE_END()
