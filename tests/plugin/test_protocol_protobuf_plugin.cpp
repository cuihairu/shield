#define BOOST_TEST_MODULE ProtocolProtobufPluginTests
#include <boost/test/unit_test.hpp>

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/protocol_codec.h"

#include <google/protobuf/descriptor.pb.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" const shield_plugin_abi_v1* shield_plugin_get_v1(void);

namespace {

std::filesystem::path write_login_descriptor_set() {
    google::protobuf::FileDescriptorProto file;
    file.set_name("shield_test_login.proto");
    file.set_package("shield.test");
    file.set_syntax("proto3");

    auto* message = file.add_message_type();
    message->set_name("Login");

    auto* uid = message->add_field();
    uid->set_name("uid");
    uid->set_number(1);
    uid->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
    uid->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT32);

    auto* name = message->add_field();
    name->set_name("name");
    name->set_number(2);
    name->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
    name->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);

    google::protobuf::FileDescriptorSet set;
    *set.add_file() = file;

    const auto path = std::filesystem::temp_directory_path() /
                      "shield_test_login_descriptor.pb";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    BOOST_REQUIRE(out.is_open());
    BOOST_REQUIRE(set.SerializeToOstream(&out));
    return path;
}

nlohmann::json parse_json_result(const char* data, std::uint64_t size) {
    BOOST_REQUIRE(data != nullptr);
    BOOST_REQUIRE_GT(size, 0u);
    return nlohmann::json::parse(data, data + static_cast<std::size_t>(size));
}

}  // namespace

BOOST_AUTO_TEST_SUITE(ProtocolProtobufPlugin)

BOOST_AUTO_TEST_CASE(CodecRoundTripsFileDescriptorSetMessage) {
    const auto descriptor_path = write_login_descriptor_set();
    const auto config = nlohmann::json{
        {"descriptor_set", descriptor_path.string()},
        {"messages",
         nlohmann::json::array({nlohmann::json{
             {"schema_id", 42},
             {"route_id", 4097},
             {"name", "shield.test.Login"},
         }})},
    }.dump();

    shield_plugin_create_args_v1 args{};
    args.instance_id = "protocol.protobuf.test";
    args.config_json = config.c_str();

    const auto* abi = shield_plugin_get_v1();
    BOOST_REQUIRE(abi != nullptr);
    BOOST_CHECK_EQUAL(abi->package_id, "protocol.protobuf");
    BOOST_REQUIRE(abi->create != nullptr);

    shield_plugin_instance_v1* instance = nullptr;
    shield_error_v1 create_error{};
    BOOST_REQUIRE_EQUAL(abi->create(&args, &instance, &create_error), 0);
    BOOST_REQUIRE(instance != nullptr);
    BOOST_REQUIRE(instance->get_interface != nullptr);

    const auto* codec = static_cast<const shield_protocol_codec_v1*>(
        instance->get_interface(instance, SHIELD_PROTOCOL_CODEC_INTERFACE,
                                nullptr));
    BOOST_REQUIRE(codec != nullptr);
    BOOST_CHECK_EQUAL(codec->codec_name, "protobuf");
    BOOST_REQUIRE(codec->encode != nullptr);
    BOOST_REQUIRE(codec->decode != nullptr);

    const std::string message_json = R"json({"uid":7,"name":"alice"})json";
    shield_protocol_encode_args_v1 encode_args{};
    encode_args.route_id = 4097;
    encode_args.schema_id = 42;
    encode_args.route_name = "shield.test.Login";
    encode_args.message_json = message_json.data();
    encode_args.message_json_size = message_json.size();

    shield_protocol_encode_result_v1 encoded{};
    shield_error_v1 encode_error{};
    BOOST_REQUIRE_EQUAL(codec->encode(codec, &encode_args, &encoded,
                                      &encode_error),
                        0);
    BOOST_REQUIRE(encoded.payload != nullptr);
    BOOST_REQUIRE_GT(encoded.payload_size, 0u);
    const std::vector<std::uint8_t> payload(
        encoded.payload, encoded.payload + encoded.payload_size);
    codec->free_encode_result(codec, &encoded);

    shield_protocol_decode_args_v1 decode_args{};
    decode_args.route_id = 4097;
    decode_args.schema_id = 42;
    decode_args.route_name = "shield.test.Login";
    decode_args.payload = payload.data();
    decode_args.payload_size = payload.size();

    shield_protocol_decode_result_v1 decoded{};
    shield_error_v1 decode_error{};
    BOOST_REQUIRE_EQUAL(codec->decode(codec, &decode_args, &decoded,
                                      &decode_error),
                        0);
    const auto decoded_json =
        parse_json_result(decoded.message_json, decoded.message_json_size);
    BOOST_CHECK_EQUAL(decoded_json["uid"].get<int>(), 7);
    BOOST_CHECK_EQUAL(decoded_json["name"].get<std::string>(), "alice");
    codec->free_decode_result(codec, &decoded);

    instance->shutdown(instance);
}

BOOST_AUTO_TEST_CASE(CodecReportsMissingSchema) {
    const auto descriptor_path = write_login_descriptor_set();
    const auto config = nlohmann::json{
        {"descriptor_set", descriptor_path.string()},
        {"messages",
         nlohmann::json::array({nlohmann::json{
             {"schema_id", 42},
             {"name", "shield.test.Login"},
         }})},
    }.dump();

    shield_plugin_create_args_v1 args{};
    args.instance_id = "protocol.protobuf.test";
    args.config_json = config.c_str();

    shield_plugin_instance_v1* instance = nullptr;
    shield_error_v1 create_error{};
    BOOST_REQUIRE_EQUAL(shield_plugin_get_v1()->create(&args, &instance,
                                                       &create_error),
                        0);
    BOOST_REQUIRE(instance != nullptr);

    const auto* codec = static_cast<const shield_protocol_codec_v1*>(
        instance->get_interface(instance, SHIELD_PROTOCOL_CODEC_INTERFACE,
                                nullptr));
    BOOST_REQUIRE(codec != nullptr);

    const std::string message_json = R"json({"uid":7})json";
    shield_protocol_encode_args_v1 encode_args{};
    encode_args.schema_id = 77;
    encode_args.message_json = message_json.data();
    encode_args.message_json_size = message_json.size();

    shield_protocol_encode_result_v1 encoded{};
    shield_error_v1 encode_error{};
    BOOST_CHECK_NE(codec->encode(codec, &encode_args, &encoded,
                                 &encode_error),
                   0);
    BOOST_REQUIRE(encode_error.code != nullptr);
    BOOST_CHECK_EQUAL(encode_error.code, "protocol.schema_not_found");

    instance->shutdown(instance);
}

BOOST_AUTO_TEST_SUITE_END()
