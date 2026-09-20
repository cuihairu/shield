#define BOOST_TEST_MODULE ProtocolMsgpackPluginTests
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/protocol_codec.h"

extern "C" const shield_plugin_abi_v1* shield_plugin_get_v1(void);

namespace {

nlohmann::json parse_json_result(const char* data, std::uint64_t size) {
    BOOST_REQUIRE(data != nullptr);
    BOOST_REQUIRE_GT(size, 0u);
    return nlohmann::json::parse(data, data + static_cast<std::size_t>(size));
}

}  // namespace

BOOST_AUTO_TEST_SUITE(ProtocolMsgpackPlugin)

BOOST_AUTO_TEST_CASE(CodecRoundTripsCanonicalJsonMessage) {
    shield_plugin_create_args_v1 args{};
    args.instance_id = "protocol.msgpack.test";
    args.config_json = "{}";

    const auto* abi = shield_plugin_get_v1();
    BOOST_REQUIRE(abi != nullptr);
    BOOST_CHECK_EQUAL(abi->package_id, "protocol.msgpack");
    BOOST_REQUIRE(abi->create != nullptr);

    shield_plugin_instance_v1* instance = nullptr;
    shield_error_v1 create_error{};
    BOOST_REQUIRE_EQUAL(abi->create(&args, &instance, &create_error), 0);
    BOOST_REQUIRE(instance != nullptr);
    BOOST_REQUIRE(instance->get_interface != nullptr);

    const auto* codec =
        static_cast<const shield_protocol_codec_v1*>(instance->get_interface(
            instance, SHIELD_PROTOCOL_CODEC_INTERFACE, nullptr));
    BOOST_REQUIRE(codec != nullptr);
    BOOST_CHECK_EQUAL(codec->codec_name, "msgpack");
    BOOST_REQUIRE(codec->encode != nullptr);
    BOOST_REQUIRE(codec->decode != nullptr);

    const std::string message_json =
        R"json({"uid":7,"name":"alice","tags":["a","b"]})json";
    shield_protocol_encode_args_v1 encode_args{};
    encode_args.message_json = message_json.data();
    encode_args.message_json_size = message_json.size();

    shield_protocol_encode_result_v1 encoded{};
    shield_error_v1 encode_error{};
    BOOST_REQUIRE_EQUAL(
        codec->encode(codec, &encode_args, &encoded, &encode_error), 0);
    BOOST_REQUIRE(encoded.payload != nullptr);
    BOOST_REQUIRE_GT(encoded.payload_size, 0u);
    const std::vector<std::uint8_t> payload(
        encoded.payload, encoded.payload + encoded.payload_size);
    codec->free_encode_result(codec, &encoded);

    shield_protocol_decode_args_v1 decode_args{};
    decode_args.payload = payload.data();
    decode_args.payload_size = payload.size();

    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 decode_error{};
    BOOST_REQUIRE_EQUAL(
        codec->decode(codec, &decode_args, &decoded, &decode_error), 0);
    const auto decoded_json =
        parse_json_result(decoded.message_json, decoded.message_json_size);
    BOOST_CHECK_EQUAL(decoded_json["uid"].get<int>(), 7);
    BOOST_CHECK_EQUAL(decoded_json["name"].get<std::string>(), "alice");
    BOOST_REQUIRE(decoded_json["tags"].is_array());
    BOOST_CHECK_EQUAL(decoded_json["tags"][0].get<std::string>(), "a");
    BOOST_CHECK_EQUAL(decoded_json["tags"][1].get<std::string>(), "b");
    codec->free_decode_result(codec, &decoded);

    instance->shutdown(instance);
}

BOOST_AUTO_TEST_CASE(CodecReportsInvalidPayload) {
    shield_plugin_create_args_v1 args{};
    args.instance_id = "protocol.msgpack.test";
    args.config_json = "{}";

    shield_plugin_instance_v1* instance = nullptr;
    shield_error_v1 create_error{};
    BOOST_REQUIRE_EQUAL(
        shield_plugin_get_v1()->create(&args, &instance, &create_error), 0);
    BOOST_REQUIRE(instance != nullptr);

    const auto* codec =
        static_cast<const shield_protocol_codec_v1*>(instance->get_interface(
            instance, SHIELD_PROTOCOL_CODEC_INTERFACE, nullptr));
    BOOST_REQUIRE(codec != nullptr);

    const std::vector<std::uint8_t> invalid = {0xc1};
    shield_protocol_decode_args_v1 decode_args{};
    decode_args.payload = invalid.data();
    decode_args.payload_size = invalid.size();

    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 decode_error{};
    BOOST_CHECK_NE(codec->decode(codec, &decode_args, &decoded, &decode_error),
                   0);
    BOOST_REQUIRE(decode_error.code != nullptr);
    BOOST_CHECK_EQUAL(decode_error.code, "protocol.decode_failed");

    instance->shutdown(instance);
}

// ---------------------------------------------------------------------------
// Optional payload-schema validation (shared config/glue with protocol.json).
// ---------------------------------------------------------------------------

namespace {

struct MsgpackCodec {
    shield_plugin_instance_v1* instance = nullptr;
    const shield_protocol_codec_v1* codec = nullptr;

    explicit MsgpackCodec(const std::string& config_json) {
        shield_plugin_create_args_v1 args{};
        args.instance_id = "protocol.msgpack.test";
        args.config_json = config_json.c_str();
        shield_error_v1 err{};
        BOOST_REQUIRE_EQUAL(
            shield_plugin_get_v1()->create(&args, &instance, &err), 0);
        BOOST_REQUIRE(instance != nullptr);
        codec = static_cast<const shield_protocol_codec_v1*>(
            instance->get_interface(instance, SHIELD_PROTOCOL_CODEC_INTERFACE,
                                    nullptr));
        BOOST_REQUIRE(codec != nullptr);
    }
    ~MsgpackCodec() {
        if (instance != nullptr) instance->shutdown(instance);
    }
};

const char* kLoginSchemas = R"json({
  "schemas": {
    "login": {
      "type": "object",
      "required": ["userid"],
      "properties": {"userid": {"type": "integer", "minimum": 1}},
      "additionalProperties": false
    }
  }
})json";

}  // namespace

BOOST_AUTO_TEST_CASE(SchemaValidPayloadPassesDecodeAndEncode) {
    MsgpackCodec fix(kLoginSchemas);

    const std::string message_json = R"json({"userid":123})json";
    shield_protocol_encode_args_v1 encode_args{};
    encode_args.message_json = message_json.data();
    encode_args.message_json_size = message_json.size();
    encode_args.route_name = "login";

    shield_protocol_encode_result_v1 encoded{};
    shield_error_v1 encode_error{};
    BOOST_REQUIRE_EQUAL(
        fix.codec->encode(fix.codec, &encode_args, &encoded, &encode_error), 0);
    const std::vector<std::uint8_t> payload(
        encoded.payload, encoded.payload + encoded.payload_size);
    fix.codec->free_encode_result(fix.codec, &encoded);

    shield_protocol_decode_args_v1 decode_args{};
    decode_args.payload = payload.data();
    decode_args.payload_size = payload.size();
    decode_args.route_name = "login";

    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 decode_error{};
    BOOST_REQUIRE_EQUAL(
        fix.codec->decode(fix.codec, &decode_args, &decoded, &decode_error), 0);
    const auto decoded_json =
        parse_json_result(decoded.message_json, decoded.message_json_size);
    BOOST_CHECK_EQUAL(decoded_json["userid"].get<int>(), 123);
    fix.codec->free_decode_result(fix.codec, &decoded);
}

BOOST_AUTO_TEST_CASE(SchemaTypeMismatchFailsDecode) {
    MsgpackCodec fix(kLoginSchemas);

    // "123" as a string violates the integer type; the typo key violates
    // additionalProperties. Both reject at decode.
    for (const auto* message_json :
         {R"json({"userid":"123"})json", R"json({"user_id":123})json"}) {
        const auto encoded =
            nlohmann::json::to_msgpack(nlohmann::json::parse(message_json));
        shield_protocol_decode_args_v1 decode_args{};
        decode_args.payload = encoded.data();
        decode_args.payload_size = encoded.size();
        decode_args.route_name = "login";

        shield_protocol_decode_result_v1 decoded{};
        shield_error_v1 decode_error{};
        BOOST_CHECK_NE(
            fix.codec->decode(fix.codec, &decode_args, &decoded, &decode_error),
            0);
        BOOST_REQUIRE(decode_error.code != nullptr);
        BOOST_CHECK_EQUAL(decode_error.code, "protocol.decode_failed");
    }
}

BOOST_AUTO_TEST_CASE(SchemaWarnModePassesViolatingPayload) {
    MsgpackCodec fix(R"json({"on_violation": "warn"})json");

    const auto encoded =
        nlohmann::json::to_msgpack(nlohmann::json::parse(R"({"user_id":123})"));
    shield_protocol_decode_args_v1 decode_args{};
    decode_args.payload = encoded.data();
    decode_args.payload_size = encoded.size();
    decode_args.route_name = "login";

    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 decode_error{};
    BOOST_CHECK_EQUAL(
        fix.codec->decode(fix.codec, &decode_args, &decoded, &decode_error), 0);
    const auto decoded_json =
        parse_json_result(decoded.message_json, decoded.message_json_size);
    BOOST_CHECK_EQUAL(decoded_json["user_id"].get<int>(), 123);
    fix.codec->free_decode_result(fix.codec, &decoded);
}

BOOST_AUTO_TEST_CASE(SchemaRequireSchemaMissingRouteFails) {
    MsgpackCodec fix(R"json({"require_schema": true})json");

    const auto encoded =
        nlohmann::json::to_msgpack(nlohmann::json::parse(R"({"userid":123})"));
    shield_protocol_decode_args_v1 decode_args{};
    decode_args.payload = encoded.data();
    decode_args.payload_size = encoded.size();
    decode_args.route_name = "unknown";

    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 decode_error{};
    BOOST_CHECK_NE(
        fix.codec->decode(fix.codec, &decode_args, &decoded, &decode_error), 0);
    BOOST_REQUIRE(decode_error.code != nullptr);
    BOOST_CHECK_EQUAL(decode_error.code, "protocol.schema_not_found");
}

BOOST_AUTO_TEST_CASE(SchemaEncodeViolationFails) {
    MsgpackCodec fix(kLoginSchemas);

    const std::string message_json = R"json({"userid":"123"})json";
    shield_protocol_encode_args_v1 encode_args{};
    encode_args.message_json = message_json.data();
    encode_args.message_json_size = message_json.size();
    encode_args.route_name = "login";

    shield_protocol_encode_result_v1 encoded{};
    shield_error_v1 encode_error{};
    BOOST_CHECK_NE(
        fix.codec->encode(fix.codec, &encode_args, &encoded, &encode_error), 0);
    BOOST_REQUIRE(encode_error.code != nullptr);
    BOOST_CHECK_EQUAL(encode_error.code, "protocol.encode_failed");
}

BOOST_AUTO_TEST_SUITE_END()
