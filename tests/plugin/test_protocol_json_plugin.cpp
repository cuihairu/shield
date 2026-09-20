// protocol.json plugin tests: optional payload-schema validation over the
// codec ABI, plus a pipeline-level integration test proving that a schema
// violation on decode surfaces as the session-closing "body decode failed"
// error.
#define BOOST_TEST_MODULE ProtocolJsonPluginTests
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/protocol_codec.h"
#include "shield/transport/protocol.hpp"

extern "C" const shield_plugin_abi_v1* shield_plugin_get_v1(void);

using shield::transport::build_protocol_pipeline_from_json;
using shield::transport::ProtocolBuildOptions;
using shield::transport::RouteDirection;
using shield::transport::RouteEntry;

namespace {

nlohmann::json parse_json_result(const char* data, std::uint64_t size) {
    BOOST_REQUIRE(data != nullptr);
    BOOST_REQUIRE_GT(size, 0u);
    return nlohmann::json::parse(data, data + static_cast<std::size_t>(size));
}

// Per-test RAII wrapper: creates the plugin instance from a config string and
// resolves its codec interface.
struct JsonCodec {
    shield_plugin_instance_v1* instance = nullptr;
    const shield_protocol_codec_v1* codec = nullptr;

    JsonCodec(const std::string& config_json,
              const char* instance_id = "protocol.json.test") {
        shield_plugin_create_args_v1 args{};
        args.instance_id = instance_id;
        args.config_json = config_json.c_str();
        shield_error_v1 err{};
        BOOST_REQUIRE_EQUAL(
            shield_plugin_get_v1()->create(&args, &instance, &err), 0);
        BOOST_REQUIRE(instance != nullptr);
        BOOST_REQUIRE(instance->get_interface != nullptr);
        codec = static_cast<const shield_protocol_codec_v1*>(
            instance->get_interface(instance, SHIELD_PROTOCOL_CODEC_INTERFACE,
                                    nullptr));
        BOOST_REQUIRE(codec != nullptr);
        BOOST_CHECK_EQUAL(codec->codec_name, "json");
    }
    ~JsonCodec() {
        if (instance != nullptr) instance->shutdown(instance);
    }
};

// Schema keyed by the route's schema_name: requires a string "userid" and
// rejects unknown keys — the motivating typo catch.
const char* kLoginSchemas = R"json({
  "schemas": {
    "login": {
      "type": "object",
      "required": ["userid"],
      "properties": {"userid": {"type": "string", "minLength": 1}},
      "additionalProperties": false
    }
  }
})json";

void decode_bytes(const shield_protocol_codec_v1* codec,
                  const std::vector<std::uint8_t>& payload,
                  const char* route_name, int& rc,
                  shield_protocol_decode_result_v1& out, shield_error_v1& err) {
    shield_protocol_decode_args_v1 args{};
    args.payload = payload.data();
    args.payload_size = payload.size();
    args.route_name = route_name;
    std::memset(&out, 0, sizeof(out));
    std::memset(&err, 0, sizeof(err));
    rc = codec->decode(codec, &args, &out, &err);
}

std::vector<std::uint8_t> to_bytes(const std::string& value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

}  // namespace

BOOST_AUTO_TEST_SUITE(ProtocolJsonPlugin)

// 1. Round-trip with no schema configured: decode/encode behave like the
// built-in json codec.
BOOST_AUTO_TEST_CASE(RoundTripsWithoutSchema) {
    JsonCodec fix("{}");

    const std::string message_json = R"json({"uid":7,"name":"alice"})json";
    shield_protocol_encode_args_v1 encode_args{};
    encode_args.message_json = message_json.data();
    encode_args.message_json_size = message_json.size();

    shield_protocol_encode_result_v1 encoded{};
    shield_error_v1 encode_error{};
    BOOST_REQUIRE_EQUAL(
        fix.codec->encode(fix.codec, &encode_args, &encoded, &encode_error), 0);
    const std::vector<std::uint8_t> payload(
        encoded.payload, encoded.payload + encoded.payload_size);
    fix.codec->free_encode_result(fix.codec, &encoded);

    int rc = 0;
    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 decode_error{};
    decode_bytes(fix.codec, payload, nullptr, rc, decoded, decode_error);
    BOOST_CHECK_EQUAL(rc, 0);
    const auto message =
        parse_json_result(decoded.message_json, decoded.message_json_size);
    BOOST_CHECK_EQUAL(message["uid"].get<int>(), 7);
    BOOST_CHECK_EQUAL(message["name"].get<std::string>(), "alice");
    fix.codec->free_decode_result(fix.codec, &decoded);
}

// 2. Payload matching the inline schema passes.
BOOST_AUTO_TEST_CASE(ValidPayloadAgainstInlineSchemaPasses) {
    JsonCodec fix(kLoginSchemas);

    int rc = 0;
    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 err{};
    decode_bytes(fix.codec, to_bytes(R"({"userid":"123"})"), "login", rc,
                 decoded, err);
    BOOST_CHECK_EQUAL(rc, 0);
    const auto message =
        parse_json_result(decoded.message_json, decoded.message_json_size);
    BOOST_CHECK_EQUAL(message["userid"].get<std::string>(), "123");
    fix.codec->free_decode_result(fix.codec, &decoded);
}

// 3. The motivating typo case through the full ABI: "user_id" instead of
// "userid" is rejected at decode with a violation naming the stray key.
BOOST_AUTO_TEST_CASE(TypoPayloadRejectedAtDecode) {
    JsonCodec fix(kLoginSchemas);

    // Typo-only payload: "required" fires first with a clear message.
    int rc = 0;
    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 err{};
    decode_bytes(fix.codec, to_bytes(R"({"user_id":123})"), "login", rc,
                 decoded, err);
    BOOST_CHECK_NE(rc, 0);
    BOOST_REQUIRE(err.code != nullptr);
    BOOST_CHECK_EQUAL(err.code, "protocol.decode_failed");
    BOOST_REQUIRE(err.message != nullptr);
    BOOST_CHECK(std::string(err.message).find("userid") != std::string::npos);

    // Required key present plus the typo key: strict mode rejects the stray
    // key with it named in the violation.
    decode_bytes(fix.codec, to_bytes(R"({"userid":"123","user_id":123})"),
                 "login", rc, decoded, err);
    BOOST_CHECK_NE(rc, 0);
    BOOST_REQUIRE(err.code != nullptr);
    BOOST_CHECK_EQUAL(err.code, "protocol.decode_failed");
    BOOST_REQUIRE(err.message != nullptr);
    const std::string message = err.message;
    BOOST_CHECK(message.find("user_id") != std::string::npos);
    BOOST_CHECK(message.find("additional property") != std::string::npos);
}

// 4. Routes without a schema pass through even when schemas are configured
// (require_schema defaults to false).
BOOST_AUTO_TEST_CASE(RouteWithoutSchemaPassesThrough) {
    JsonCodec fix(kLoginSchemas);

    int rc = 0;
    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 err{};
    decode_bytes(fix.codec, to_bytes(R"({"anything":1})"), "other.route", rc,
                 decoded, err);
    BOOST_CHECK_EQUAL(rc, 0);
    const auto message =
        parse_json_result(decoded.message_json, decoded.message_json_size);
    BOOST_CHECK_EQUAL(message["anything"].get<int>(), 1);
    fix.codec->free_decode_result(fix.codec, &decoded);
}

// 5. require_schema=true turns a missing per-route schema into a hard
// protocol.schema_not_found failure.
BOOST_AUTO_TEST_CASE(RequireSchemaMissingRouteFails) {
    JsonCodec fix(R"json({"require_schema": true})json");

    int rc = 0;
    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 err{};
    decode_bytes(fix.codec, to_bytes(R"({"userid":"123"})"), "unknown", rc,
                 decoded, err);
    BOOST_CHECK_NE(rc, 0);
    BOOST_REQUIRE(err.code != nullptr);
    BOOST_CHECK_EQUAL(err.code, "protocol.schema_not_found");
}

// 6. on_violation=warn logs (host_api is null here — the direct call must not
// crash) and passes the message through unchanged.
BOOST_AUTO_TEST_CASE(WarnModePassesViolatingPayload) {
    JsonCodec fix(R"json({"on_violation": "warn"})json");

    const auto payload = to_bytes(R"({"user_id":123})");
    const char* routes[] = {"login", nullptr};
    for (const char* route : routes) {
        shield_protocol_decode_args_v1 args{};
        args.payload = payload.data();
        args.payload_size = payload.size();
        args.route_name = route;
        // Schema lookup with a null route_name finds nothing: warn mode plus
        // the miss means the payload passes either way.
        shield_protocol_decode_result_v1 decoded{};
        shield_error_v1 err{};
        BOOST_CHECK_EQUAL(fix.codec->decode(fix.codec, &args, &decoded, &err),
                          0);
        const auto message =
            parse_json_result(decoded.message_json, decoded.message_json_size);
        BOOST_CHECK_EQUAL(message["user_id"].get<int>(), 123);
        fix.codec->free_decode_result(fix.codec, &decoded);
    }
}

// 7. Encode-side violations fail with protocol.encode_failed (outbound drops
// the message; the session stays up).
BOOST_AUTO_TEST_CASE(EncodeViolationFails) {
    JsonCodec fix(kLoginSchemas);

    const std::string message_json = R"json({"user_id":123})json";
    shield_protocol_encode_args_v1 encode_args{};
    encode_args.message_json = message_json.data();
    encode_args.message_json_size = message_json.size();
    encode_args.route_name = "login";

    shield_protocol_encode_result_v1 encoded{};
    shield_error_v1 err{};
    BOOST_CHECK_NE(fix.codec->encode(fix.codec, &encode_args, &encoded, &err),
                   0);
    BOOST_REQUIRE(err.code != nullptr);
    BOOST_CHECK_EQUAL(err.code, "protocol.encode_failed");
}

// 8. Unparseable bytes fail the decode exactly like the built-in codec
// (which closes the session upstream).
BOOST_AUTO_TEST_CASE(UnparseableBytesFailDecode) {
    JsonCodec fix(kLoginSchemas);

    for (const auto* payload : {"{nope", ""}) {
        int rc = 0;
        shield_protocol_decode_result_v1 decoded{};
        shield_error_v1 err{};
        decode_bytes(fix.codec, to_bytes(payload), "login", rc, decoded, err);
        BOOST_CHECK_NE(rc, 0);
        BOOST_REQUIRE(err.code != nullptr);
        BOOST_CHECK_EQUAL(err.code, "protocol.decode_failed");
    }
}

// 9. The optional top-level {"payload": ...} envelope is unwrapped before
// validation, mirroring the built-in codec: schemas describe the business
// message, and the delivered message is the business message.
BOOST_AUTO_TEST_CASE(PayloadEnvelopeUnwrappedBeforeValidation) {
    JsonCodec fix(kLoginSchemas);

    // Envelope with a valid business message: passes, delivered unwrapped.
    int rc = 0;
    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 err{};
    decode_bytes(fix.codec, to_bytes(R"({"payload":{"userid":"123"}})"),
                 "login", rc, decoded, err);
    BOOST_CHECK_EQUAL(rc, 0);
    const auto message =
        parse_json_result(decoded.message_json, decoded.message_json_size);
    BOOST_CHECK(message.is_object());
    BOOST_CHECK_EQUAL(message["userid"].get<std::string>(), "123");
    BOOST_CHECK(!message.contains("payload"));
    fix.codec->free_decode_result(fix.codec, &decoded);

    // Violation inside the envelope is caught too.
    decode_bytes(fix.codec, to_bytes(R"({"payload":{"user_id":123}})"), "login",
                 rc, decoded, err);
    BOOST_CHECK_NE(rc, 0);
    BOOST_REQUIRE(err.code != nullptr);
    BOOST_CHECK_EQUAL(err.code, "protocol.decode_failed");
}

// 10. Config shape violations fail create with plugin.create.failed.
BOOST_AUTO_TEST_CASE(ConfigErrorsFailCreate) {
    const char* bad_configs[] = {
        "not json at all",
        R"json({"schemas": "notanobject"})json",
        R"json({"schemas": {"login": 5}})json",
        R"json({"schemas": {}, "schemas_file": "x.json"})json",
        R"json({"schemas_file": "/nonexistent/path/schemas.json"})json",
        R"json({"on_violation": "explode"})json",
        R"json({"require_schema": "yes"})json",
    };
    for (const auto* config_json : bad_configs) {
        shield_plugin_create_args_v1 args{};
        args.instance_id = "protocol.json.test";
        args.config_json = config_json;
        shield_plugin_instance_v1* instance = nullptr;
        shield_error_v1 err{};
        BOOST_CHECK_NE(shield_plugin_get_v1()->create(&args, &instance, &err),
                       0);
        BOOST_REQUIRE(err.code != nullptr);
        BOOST_CHECK_EQUAL(err.code, "plugin.create.failed");
    }
}

// 11. schemas_file loads schemas from disk at create time.
BOOST_AUTO_TEST_CASE(SchemasFileLoading) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "shield_json_plugin_test";
    fs::create_directories(dir);
    const auto file = dir / "schemas.json";
    {
        std::ofstream out(file, std::ios::binary);
        out << R"json({"login": {
            "type": "object",
            "required": ["userid"],
            "properties": {"userid": {"type": "string"}},
            "additionalProperties": false
        }})json";
    }

    const auto config = "{\"schemas_file\": \"" + file.string() + "\"}";
    JsonCodec fix(config);

    int rc = 0;
    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 err{};
    decode_bytes(fix.codec, to_bytes(R"({"userid":"123"})"), "login", rc,
                 decoded, err);
    BOOST_CHECK_EQUAL(rc, 0);
    fix.codec->free_decode_result(fix.codec, &decoded);

    decode_bytes(fix.codec, to_bytes(R"({"user_id":123})"), "login", rc,
                 decoded, err);
    BOOST_CHECK_NE(rc, 0);
    std::error_code ignored;
    fs::remove(file, ignored);
}

// 12. Pipeline integration: the plugin is wired through
// body.provider=protocol.json and a schema violation surfaces as the
// session-closing "body decode failed" error with the offending key.
BOOST_AUTO_TEST_CASE(PipelineIntegrationDecodeViolationFailsFrame) {
    // Schema keyed by the route's schema_name ("game.Login").
    auto schemas = nlohmann::json::parse(kLoginSchemas);
    schemas["schemas"]["game.Login"] = schemas["schemas"]["login"];
    JsonCodec fix(schemas.dump());

    const auto config = R"json(
{
  "name": "game.json",
  "envelope": {
    "type": "idlen",
    "route_id_bytes": 2,
    "length_bytes": 2
  },
  "body": {
    "codec": "json",
    "provider": "protocol.json"
  }
}
)json";

    ProtocolBuildOptions options;
    options.external_codec_resolver =
        [&](std::string_view provider, std::string_view codec_name,
            std::string*) -> const shield_protocol_codec_v1* {
        BOOST_CHECK(provider == "protocol.json");
        BOOST_CHECK(codec_name == "json");
        return fix.codec;
    };

    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, options, &error);
    BOOST_REQUIRE_MESSAGE(pipeline != nullptr, error);
    {
        RouteEntry entry;
        entry.route_id = 4097;
        entry.debug_name = "game.Login";
        entry.direction = RouteDirection::ClientToServer;
        entry.requires_auth = false;
        entry.schema_name = "game.Login";
        entry.policy.lazy_decode = false;
        pipeline->routes().upsert(entry);
    }

    // Bad frame: a valid payload plus the typo key -> strict mode rejects the
    // stray key -> decode fails -> the pipeline reports the frame as failed
    // with "body decode failed" plus the violation (this is what the session
    // layer turns into a close).
    shield::transport::Packet packet;
    packet.route_id = 4097;
    packet.body = to_bytes(R"({"userid":"123","user_id":123})");
    const auto encoded_bad = pipeline->encode(packet.ref());
    BOOST_REQUIRE_MESSAGE(pipeline->error().empty(), pipeline->error());
    auto results = pipeline->feed(encoded_bad.data(), encoded_bad.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(!results[0].ok());
    BOOST_CHECK(results[0].error.find("body decode failed") !=
                std::string::npos);
    BOOST_CHECK(results[0].error.find("user_id") != std::string::npos);

    // Good frame: decodes through the plugin.
    packet.body = to_bytes(R"({"userid":"123"})");
    const auto encoded_good = pipeline->encode(packet.ref());
    results = pipeline->feed(encoded_good.data(), encoded_good.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(results[0].ok());
    BOOST_REQUIRE(results[0].decoded());
    BOOST_REQUIRE(results[0].decoded_body->has_message());
    BOOST_CHECK_EQUAL(
        (*results[0].decoded_body->message)["userid"].get<std::string>(),
        "123");
}

BOOST_AUTO_TEST_SUITE_END()
