// Test for protocol.flatbuffers plugin

#define BOOST_TEST_MODULE ProtocolFlatbuffersPlugin
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "shield/plugin/abi.h"
#include "shield/plugin/protocol_codec.h"

namespace {

// Helper to create a test FlatBuffers schema
std::string create_test_schema() {
    return R"(
namespace Test;

table LoginRequest {
    username:string;
    password:string;
}

table LoginResponse {
    code:int;
    message:string;
    token:string;
}

table Person {
    name:string;
    age:int;
    email:string;
}

root_type LoginRequest;
)";
}

// Helper to write schema to temp file
std::string write_temp_schema(const std::string& schema) {
    auto temp_dir = std::filesystem::temp_directory_path();
    auto temp_file = temp_dir / "test.fbs";
    std::ofstream out(temp_file);
    out << schema;
    out.close();
    return temp_file.string();
}

// Build a live plugin instance against a schema file and return its codec
// vtable. Verifies every ABI hop a host would go through.
const shield_protocol_codec_v1* make_codec(
    const shield_plugin_abi_v1** abi_out = nullptr) {
    const auto schema_path = write_temp_schema(create_test_schema());
    const auto config = nlohmann::json{{"schema", schema_path}}.dump();

    // One instance per call; the test process exits before any leak matters.
    shield_plugin_instance_v1* instance = nullptr;
    shield_plugin_create_args_v1 args{};
    args.instance_id = "protocol.flatbuffers.test";
    args.config_json = config.c_str();

    const auto* abi = shield_plugin_get_v1();
    BOOST_REQUIRE(abi != nullptr);
    BOOST_REQUIRE(abi->create != nullptr);
    shield_error_v1 create_error{};
    BOOST_REQUIRE_EQUAL(abi->create(&args, &instance, &create_error), 0);
    BOOST_REQUIRE(instance != nullptr);
    BOOST_REQUIRE(instance->get_interface != nullptr);
    if (abi_out) *abi_out = abi;
    return static_cast<const shield_protocol_codec_v1*>(instance->get_interface(
        instance, SHIELD_PROTOCOL_CODEC_INTERFACE, nullptr));
}

// Encode a LoginRequest JSON and return the binary payload.
std::vector<std::uint8_t> encode_login(const shield_protocol_codec_v1* codec,
                                       const char* json) {
    BOOST_REQUIRE(codec != nullptr);
    BOOST_REQUIRE(codec->encode != nullptr);
    shield_protocol_encode_args_v1 encode_args{};
    encode_args.route_name = "LoginRequest";
    encode_args.message_json = json;
    encode_args.message_json_size = std::strlen(json);
    shield_protocol_encode_result_v1 encoded{};
    shield_error_v1 encode_error{};
    BOOST_REQUIRE_EQUAL(
        codec->encode(codec, &encode_args, &encoded, &encode_error), 0);
    BOOST_REQUIRE(encoded.payload != nullptr);
    BOOST_REQUIRE_GT(encoded.payload_size, 0u);
    std::vector<std::uint8_t> payload(encoded.payload,
                                      encoded.payload + encoded.payload_size);
    codec->free_encode_result(codec, &encoded);
    return payload;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(protocol_flatbuffers_plugin)

BOOST_AUTO_TEST_CASE(load_plugin_abi) {
    shield_plugin_abi_v1 abi{};
    abi.abi_version = SHIELD_PLUGIN_ABI_VERSION;
    abi.struct_size = sizeof(shield_plugin_abi_v1);
    abi.package_id = "protocol.flatbuffers";
    abi.create = nullptr;

    BOOST_CHECK_EQUAL(abi.abi_version, SHIELD_PLUGIN_ABI_VERSION);
    BOOST_CHECK_EQUAL(std::strcmp(abi.package_id, "protocol.flatbuffers"), 0);
}

BOOST_AUTO_TEST_CASE(flatbuffers_schema_parsing) {
    // Test that FlatBuffers schema can be written
    auto schema = create_test_schema();
    auto schema_path = write_temp_schema(schema);

    BOOST_CHECK(std::filesystem::exists(schema_path));

    // Clean up
    std::filesystem::remove(schema_path);
}

BOOST_AUTO_TEST_CASE(codec_interface_structure) {
    // Test the codec interface structure
    shield_protocol_codec_v1 codec{};
    codec.struct_size = sizeof(shield_protocol_codec_v1);
    codec.codec_name = "flatbuffers";
    codec.version = "1.0.0";
    codec.user_data = nullptr;
    codec.decode = nullptr;
    codec.encode = nullptr;
    codec.free_decode_result = nullptr;
    codec.free_encode_result = nullptr;

    BOOST_CHECK_EQUAL(codec.struct_size, sizeof(shield_protocol_codec_v1));
    BOOST_CHECK_EQUAL(std::strcmp(codec.codec_name, "flatbuffers"), 0);
    BOOST_CHECK_EQUAL(std::strcmp(codec.version, "1.0.0"), 0);
}

BOOST_AUTO_TEST_CASE(decode_args_structure) {
    // Only the host-resolved schema type name travels with the payload.
    shield_protocol_decode_args_v1 args{};
    args.route_name = "LoginRequest";
    args.payload = nullptr;
    args.payload_size = 0;

    BOOST_CHECK_EQUAL(std::strcmp(args.route_name, "LoginRequest"), 0);
    BOOST_CHECK_EQUAL(args.payload_size, 0u);
}

BOOST_AUTO_TEST_CASE(encode_args_structure) {
    // Only the host-resolved schema type name travels with the message.
    shield_protocol_encode_args_v1 args{};
    args.route_name = "LoginResponse";
    args.message_json = R"({"code": 200, "message": "success"})";
    args.message_json_size = std::strlen(args.message_json);

    BOOST_CHECK_EQUAL(std::strcmp(args.route_name, "LoginResponse"), 0);
    BOOST_CHECK(args.message_json_size > 0);
}

BOOST_AUTO_TEST_CASE(decode_result_structure) {
    // Test decode result structure
    shield_protocol_decode_result_v1 result{};
    result.message_json = nullptr;
    result.message_json_size = 0;

    BOOST_CHECK(result.message_json == nullptr);
    BOOST_CHECK_EQUAL(result.message_json_size, 0);
}

BOOST_AUTO_TEST_CASE(encode_result_structure) {
    // Test encode result structure
    shield_protocol_encode_result_v1 result{};
    result.payload = nullptr;
    result.payload_size = 0;

    BOOST_CHECK(result.payload == nullptr);
    BOOST_CHECK_EQUAL(result.payload_size, 0);
}

BOOST_AUTO_TEST_CASE(error_structure) {
    // Test error structure
    shield_error_v1 err{};
    err.code = "test.error";
    err.message = "test error message";
    err.phase = "test";

    BOOST_CHECK_EQUAL(std::strcmp(err.code, "test.error"), 0);
    BOOST_CHECK_EQUAL(std::strcmp(err.message, "test error message"), 0);
    BOOST_CHECK_EQUAL(std::strcmp(err.phase, "test"), 0);
}

// A well-formed payload must survive the schema-driven verifier and decode
// to the same fields it was encoded from.
BOOST_AUTO_TEST_CASE(decode_valid_payload_succeeds) {
    const auto* codec = make_codec();
    BOOST_REQUIRE(codec != nullptr);
    BOOST_REQUIRE(codec->decode != nullptr);

    const auto payload =
        encode_login(codec, R"({"username":"alice","password":"pw"})");

    shield_protocol_decode_args_v1 decode_args{};
    decode_args.route_name = "LoginRequest";
    decode_args.payload = payload.data();
    decode_args.payload_size = payload.size();
    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 decode_error{};
    const int rc = codec->decode(codec, &decode_args, &decoded, &decode_error);
    BOOST_CHECK_MESSAGE(
        rc == 0, std::string("decode failed: ") +
                     (decode_error.message ? decode_error.message : "?") +
                     " code=" + (decode_error.code ? decode_error.code : "?"));
    BOOST_REQUIRE(rc == 0);
    BOOST_REQUIRE(decoded.message_json != nullptr);
    // GenerateText emits flatbuffers text form (unquoted field keys, quoted
    // string values), not strict JSON — assert on the rendered text.
    const std::string out(decoded.message_json);
    BOOST_CHECK(out.find("username: \"alice\"") != std::string::npos);
    BOOST_CHECK(out.find("password: \"pw\"") != std::string::npos);
    codec->free_decode_result(codec, &decoded);
}

// A truncated payload must be rejected by the verifier instead of reading
// out of bounds during text generation.
BOOST_AUTO_TEST_CASE(decode_truncated_payload_rejected) {
    const auto* codec = make_codec();
    BOOST_REQUIRE(codec != nullptr);
    BOOST_REQUIRE(codec->decode != nullptr);

    const auto payload =
        encode_login(codec, R"({"username":"alice","password":"pw"})");

    // Keep only the root-offset field: the root table it points at is now
    // outside the buffer, which the verifier must catch.
    shield_protocol_decode_args_v1 decode_args{};
    decode_args.route_name = "LoginRequest";
    decode_args.payload = payload.data();
    decode_args.payload_size = 4;
    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 decode_error{};
    BOOST_CHECK_EQUAL(
        codec->decode(codec, &decode_args, &decoded, &decode_error), -1);
    BOOST_CHECK(decode_error.code != nullptr);
    BOOST_CHECK_EQUAL(std::strcmp(decode_error.code, "protocol.decode_failed"),
                      0);
}

// A buffer whose root offset is zero is structurally invalid and must be
// rejected (an empty / zeroed frame is the classic hostile-input case).
BOOST_AUTO_TEST_CASE(decode_zeroed_payload_rejected) {
    const auto* codec = make_codec();
    BOOST_REQUIRE(codec != nullptr);
    BOOST_REQUIRE(codec->decode != nullptr);

    const std::vector<std::uint8_t> zeroed(8, 0);
    shield_protocol_decode_args_v1 decode_args{};
    decode_args.route_name = "LoginRequest";
    decode_args.payload = zeroed.data();
    decode_args.payload_size = zeroed.size();
    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 decode_error{};
    BOOST_CHECK_EQUAL(
        codec->decode(codec, &decode_args, &decoded, &decode_error), -1);
    BOOST_CHECK(decode_error.code != nullptr);
    BOOST_CHECK_EQUAL(std::strcmp(decode_error.code, "protocol.decode_failed"),
                      0);
}

BOOST_AUTO_TEST_SUITE_END()
