// [SHIELD_PLUGIN] protocol.protobuf — shield.protocol.codec.v1 provider.

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/protocol_codec.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace {

char* dup_string(const std::string& value) {
    auto* out = static_cast<char*>(std::malloc(value.size() + 1));
    if (out == nullptr) return nullptr;
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return out;
}

std::uint8_t* dup_bytes(const std::string& value) {
    if (value.empty()) return nullptr;
    auto* out = static_cast<std::uint8_t*>(std::malloc(value.size()));
    if (out == nullptr) return nullptr;
    std::memcpy(out, value.data(), value.size());
    return out;
}

void fill_error(shield_error_v1* err, const char* code,
                const char* message, const char* phase = "runtime") {
    if (!err) return;
    err->code = code;
    err->message = message;
    err->phase = phase;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    return data;
}

struct protobuf_instance {
    shield_plugin_instance_v1 shell{};
    shield_protocol_codec_v1 codec{};
    std::string instance_id;
    std::string descriptor_set_path;
    google::protobuf::FileDescriptorSet descriptor_set;
    google::protobuf::SimpleDescriptorDatabase descriptor_db;
    std::unique_ptr<google::protobuf::DescriptorPool> pool;
    std::unique_ptr<google::protobuf::DynamicMessageFactory> factory;
    std::unordered_map<std::uint16_t, std::string> schema_names;
    std::unordered_map<std::uint32_t, std::string> route_names;
};

const google::protobuf::Descriptor* resolve_descriptor(
    const protobuf_instance& inst,
    const shield_protocol_decode_args_v1& args) {
    if (args.schema_id != 0) {
        const auto by_schema = inst.schema_names.find(args.schema_id);
        if (by_schema != inst.schema_names.end()) {
            return inst.pool->FindMessageTypeByName(by_schema->second);
        }
    }
    if (args.route_id != 0) {
        const auto by_route = inst.route_names.find(args.route_id);
        if (by_route != inst.route_names.end()) {
            return inst.pool->FindMessageTypeByName(by_route->second);
        }
    }
    if (args.route_name != nullptr && args.route_name[0] != '\0') {
        return inst.pool->FindMessageTypeByName(args.route_name);
    }
    return nullptr;
}

const google::protobuf::Descriptor* resolve_descriptor(
    const protobuf_instance& inst,
    const shield_protocol_encode_args_v1& args) {
    shield_protocol_decode_args_v1 decode_args{};
    decode_args.route_id = args.route_id;
    decode_args.schema_id = args.schema_id;
    decode_args.route_name = args.route_name;
    return resolve_descriptor(inst, decode_args);
}

bool load_config(protobuf_instance* inst, const char* config_json,
                 std::string* error) {
    nlohmann::json config = nlohmann::json::object();
    if (config_json && config_json[0] != '\0') {
        config = nlohmann::json::parse(config_json, nullptr, false);
        if (!config.is_object()) {
            if (error) *error = "protobuf plugin config must be an object";
            return false;
        }
    }

    if (!config.contains("descriptor_set") ||
        !config["descriptor_set"].is_string()) {
        if (error) *error = "descriptor_set is required";
        return false;
    }
    inst->descriptor_set_path = config["descriptor_set"].get<std::string>();

    if (config.contains("messages") && config["messages"].is_array()) {
        for (const auto& item : config["messages"]) {
            if (!item.is_object() || !item.contains("name") ||
                !item["name"].is_string()) {
                continue;
            }
            const auto name = item["name"].get<std::string>();
            if (item.contains("schema_id") &&
                item["schema_id"].is_number_unsigned()) {
                const auto schema_id = item["schema_id"].get<std::uint32_t>();
                if (schema_id <= UINT16_MAX) {
                    inst->schema_names[static_cast<std::uint16_t>(schema_id)] =
                        name;
                }
            }
            if (item.contains("route_id") &&
                item["route_id"].is_number_unsigned()) {
                inst->route_names[item["route_id"].get<std::uint32_t>()] =
                    name;
            }
        }
    }
    return true;
}

bool load_descriptors(protobuf_instance* inst, std::string* error) {
    const auto data = read_file(inst->descriptor_set_path);
    if (!data) {
        if (error) *error = "failed to open descriptor_set";
        return false;
    }
    if (!inst->descriptor_set.ParseFromString(*data)) {
        if (error) *error = "failed to parse FileDescriptorSet";
        return false;
    }
    for (const auto& file : inst->descriptor_set.file()) {
        if (!inst->descriptor_db.Add(file)) {
            if (error) {
                *error = "failed to add descriptor file: " + file.name();
            }
            return false;
        }
    }
    inst->pool =
        std::make_unique<google::protobuf::DescriptorPool>(&inst->descriptor_db);
    inst->factory =
        std::make_unique<google::protobuf::DynamicMessageFactory>(inst->pool.get());
    return true;
}

int protobuf_decode(const shield_protocol_codec_v1* self,
                    const shield_protocol_decode_args_v1* args,
                    shield_protocol_decode_result_v1* out,
                    shield_error_v1* err) {
    if (!self || !args || !out || !self->user_data) {
        fill_error(err, "protocol.decode_failed", "invalid protobuf decode args");
        return -1;
    }
    auto* inst = static_cast<protobuf_instance*>(self->user_data);
    const auto* descriptor = resolve_descriptor(*inst, *args);
    if (!descriptor) {
        fill_error(err, "protocol.schema_not_found",
                   "protobuf message descriptor was not found");
        return -1;
    }
    const auto* prototype = inst->factory->GetPrototype(descriptor);
    if (!prototype) {
        fill_error(err, "protocol.schema_not_found",
                   "protobuf message prototype was not found");
        return -1;
    }
    auto message = std::unique_ptr<google::protobuf::Message>(prototype->New());
    if (args->payload_size >
        static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        fill_error(err, "protocol.decode_failed",
                   "protobuf payload is too large");
        return -1;
    }
    if (args->payload == nullptr && args->payload_size > 0) {
        fill_error(err, "protocol.decode_failed",
                   "protobuf payload is null");
        return -1;
    }
    const char empty_payload[] = "";
    const auto* payload =
        args->payload_size == 0
            ? empty_payload
            : reinterpret_cast<const char*>(args->payload);
    if (!message->ParseFromArray(payload, static_cast<int>(args->payload_size))) {
        fill_error(err, "protocol.decode_failed",
                   "failed to parse protobuf payload");
        return -1;
    }

    std::string json;
    auto status = google::protobuf::util::MessageToJsonString(*message, &json);
    if (!status.ok()) {
        fill_error(err, "protocol.decode_failed",
                   "failed to convert protobuf message to JSON");
        return -1;
    }
    out->message_json = dup_string(json);
    out->message_json_size = json.size();
    return out->message_json ? 0 : -1;
}

int protobuf_encode(const shield_protocol_codec_v1* self,
                    const shield_protocol_encode_args_v1* args,
                    shield_protocol_encode_result_v1* out,
                    shield_error_v1* err) {
    if (!self || !args || !out || !self->user_data) {
        fill_error(err, "protocol.encode_failed", "invalid protobuf encode args");
        return -1;
    }
    auto* inst = static_cast<protobuf_instance*>(self->user_data);
    const auto* descriptor = resolve_descriptor(*inst, *args);
    if (!descriptor) {
        fill_error(err, "protocol.schema_not_found",
                   "protobuf message descriptor was not found");
        return -1;
    }
    const auto* prototype = inst->factory->GetPrototype(descriptor);
    if (!prototype) {
        fill_error(err, "protocol.schema_not_found",
                   "protobuf message prototype was not found");
        return -1;
    }
    auto message = std::unique_ptr<google::protobuf::Message>(prototype->New());
    if (args->message_json == nullptr && args->message_json_size > 0) {
        fill_error(err, "protocol.encode_failed",
                   "protobuf encode JSON input is null");
        return -1;
    }
    std::string json;
    if (args->message_json != nullptr && args->message_json_size > 0) {
        json.assign(args->message_json,
                    args->message_json + args->message_json_size);
    }
    auto status = google::protobuf::util::JsonStringToMessage(json, message.get());
    if (!status.ok()) {
        fill_error(err, "protocol.encode_failed",
                   "failed to convert JSON to protobuf message");
        return -1;
    }
    std::string payload;
    if (!message->SerializeToString(&payload)) {
        fill_error(err, "protocol.encode_failed",
                   "failed to serialize protobuf message");
        return -1;
    }
    out->payload = dup_bytes(payload);
    out->payload_size = payload.size();
    return payload.empty() || out->payload ? 0 : -1;
}

void free_decode_result(const shield_protocol_codec_v1*,
                        shield_protocol_decode_result_v1* result) {
    if (!result) return;
    std::free(const_cast<char*>(result->message_json));
    result->message_json = nullptr;
    result->message_json_size = 0;
}

void free_encode_result(const shield_protocol_codec_v1*,
                        shield_protocol_encode_result_v1* result) {
    if (!result) return;
    std::free(const_cast<std::uint8_t*>(result->payload));
    result->payload = nullptr;
    result->payload_size = 0;
}

int protobuf_create(const shield_plugin_create_args_v1* args,
                    shield_plugin_instance_v1** out,
                    shield_error_v1* err) {
    if (out == nullptr) {
        fill_error(err, "plugin.create.invalid",
                   "protobuf create output pointer is null", "create");
        return -1;
    }
    auto* inst = new protobuf_instance;
    inst->instance_id = (args && args->instance_id) ? args->instance_id : "";

    std::string config_error;
    if (!load_config(inst, args ? args->config_json : nullptr, &config_error)) {
        fill_error(err, "plugin.config.invalid",
                   "invalid protobuf plugin config", "create");
        delete inst;
        return -1;
    }
    if (!load_descriptors(inst, &config_error)) {
        fill_error(err, "plugin.config.invalid",
                   "failed to load protobuf descriptors", "create");
        delete inst;
        return -1;
    }

    inst->codec.struct_size = sizeof(shield_protocol_codec_v1);
    inst->codec.codec_name = "protobuf";
    inst->codec.version = "1.0.0";
    inst->codec.user_data = inst;
    inst->codec.decode = protobuf_decode;
    inst->codec.encode = protobuf_encode;
    inst->codec.free_decode_result = free_decode_result;
    inst->codec.free_encode_result = free_encode_result;

    inst->shell.struct_size = sizeof(protobuf_instance);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1* self,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        auto* inst = reinterpret_cast<protobuf_instance*>(self);
        if (iface && std::strcmp(iface, SHIELD_PROTOCOL_CODEC_INTERFACE) == 0) {
            return &inst->codec;
        }
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) {
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        if (self == nullptr) return;
        delete reinterpret_cast<protobuf_instance*>(self);
    };
    inst->shell.register_lua = [](shield_plugin_instance_v1*, lua_State*,
                                  shield_error_v1*) {
        return 0;
    };

    *out = &inst->shell;
    return 0;
}

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT
const shield_plugin_abi_v1* shield_plugin_get_v1(void) {
    static const shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "protocol.protobuf",
        "1.0.0",
        protobuf_create,
    };
    return &abi;
}
