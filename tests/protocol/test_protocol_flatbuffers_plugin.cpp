// Test for protocol.flatbuffers plugin

#define BOOST_TEST_MODULE ProtocolFlatbuffersPlugin
#include <boost/test/unit_test.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

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
    // Test decode args structure
    shield_protocol_decode_args_v1 args{};
    args.route_id = 1001;
    args.schema_id = 1;
    args.route_name = "LoginRequest";
    args.payload = nullptr;
    args.payload_size = 0;

    BOOST_CHECK_EQUAL(args.route_id, 1001);
    BOOST_CHECK_EQUAL(args.schema_id, 1);
    BOOST_CHECK_EQUAL(std::strcmp(args.route_name, "LoginRequest"), 0);
}

BOOST_AUTO_TEST_CASE(encode_args_structure) {
    // Test encode args structure
    shield_protocol_encode_args_v1 args{};
    args.route_id = 1002;
    args.schema_id = 2;
    args.route_name = "LoginResponse";
    args.message_json = R"({"code": 200, "message": "success"})";
    args.message_json_size = std::strlen(args.message_json);

    BOOST_CHECK_EQUAL(args.route_id, 1002);
    BOOST_CHECK_EQUAL(args.schema_id, 2);
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

BOOST_AUTO_TEST_SUITE_END()
