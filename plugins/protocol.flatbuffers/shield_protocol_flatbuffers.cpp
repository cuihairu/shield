// [SHIELD_PLUGIN] protocol.flatbuffers — shield.protocol.codec.v1 provider.

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/idl.h>
#include <flatbuffers/reflection.h>
#include <flatbuffers/util.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/protocol_codec.h"

namespace {

char* dup_string(const std::string& value) {
    auto* out = static_cast<char*>(std::malloc(value.size() + 1));
    if (out == nullptr) return nullptr;
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return out;
}

std::uint8_t* dup_bytes(const std::vector<std::uint8_t>& value) {
    if (value.empty()) return nullptr;
    auto* out = static_cast<std::uint8_t*>(std::malloc(value.size()));
    if (out == nullptr) return nullptr;
    std::memcpy(out, value.data(), value.size());
    return out;
}

void fill_error(shield_error_v1* err, const char* code, const char* message,
                const char* phase = "runtime") {
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

struct flatbuffers_instance {
    shield_plugin_instance_v1 shell{};
    shield_protocol_codec_v1 codec{};
    std::string instance_id;
    std::string schema_path;
    std::string schema_dir;

    // Loaded schema data
    std::string schema_data;
    const reflection::Schema* schema = nullptr;

    // Parser for JSON conversion
    flatbuffers::Parser parser;

    // Message mappings
    std::unordered_map<std::uint16_t, std::string> schema_names;
    std::unordered_map<std::uint32_t, std::string> route_names;
};

const reflection::Object* resolve_object(
    const flatbuffers_instance& inst,
    const shield_protocol_decode_args_v1& args) {
    if (!inst.schema) return nullptr;

    std::string name;
    if (args.schema_id != 0) {
        const auto it = inst.schema_names.find(args.schema_id);
        if (it != inst.schema_names.end()) {
            name = it->second;
        }
    }
    if (name.empty() && args.route_name != nullptr &&
        args.route_name[0] != '\0') {
        name = args.route_name;
    }
    if (name.empty()) return nullptr;

    return inst.schema->objects()->LookupByKey(name);
}

const reflection::Object* resolve_object(
    const flatbuffers_instance& inst,
    const shield_protocol_encode_args_v1& args) {
    shield_protocol_decode_args_v1 decode_args{};
    decode_args.route_id = args.route_id;
    decode_args.schema_id = args.schema_id;
    decode_args.route_name = args.route_name;
    return resolve_object(inst, decode_args);
}

bool load_config(flatbuffers_instance* inst, const char* config_json,
                 std::string* error) {
    nlohmann::json config = nlohmann::json::object();
    if (config_json && config_json[0] != '\0') {
        config = nlohmann::json::parse(config_json, nullptr, false);
        if (!config.is_object()) {
            if (error) *error = "flatbuffers plugin config must be an object";
            return false;
        }
    }

    if (config.contains("schema") && config["schema"].is_string()) {
        inst->schema_path = config["schema"].get<std::string>();
    }

    if (config.contains("schema_dir") && config["schema_dir"].is_string()) {
        inst->schema_dir = config["schema_dir"].get<std::string>();
    }

    if (inst->schema_path.empty() && inst->schema_dir.empty()) {
        if (error) *error = "schema or schema_dir is required";
        return false;
    }

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
                inst->route_names[item["route_id"].get<std::uint32_t>()] = name;
            }
        }
    }
    return true;
}

bool load_schema(flatbuffers_instance* inst, std::string* error) {
    // Load binary schema (.bfbs)
    if (!inst->schema_path.empty()) {
        auto data = read_file(inst->schema_path);
        if (!data) {
            if (error)
                *error = "failed to open schema file: " + inst->schema_path;
            return false;
        }
        inst->schema_data = *data;
    } else {
        // Try to find .bfbs files in schema_dir
        std::filesystem::path dir(inst->schema_dir);
        if (!std::filesystem::exists(dir)) {
            if (error)
                *error = "schema_dir does not exist: " + inst->schema_dir;
            return false;
        }

        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() == ".bfbs") {
                auto data = read_file(entry.path());
                if (data) {
                    inst->schema_data = *data;
                    break;
                }
            }
        }

        if (inst->schema_data.empty()) {
            if (error) *error = "no .bfbs files found in schema_dir";
            return false;
        }
    }

    // Parse schema
    inst->schema = reflection::GetSchema(inst->schema_data.data());
    if (!inst->schema) {
        if (error) *error = "failed to parse FlatBuffers schema";
        return false;
    }

    return true;
}

int flatbuffers_decode(const shield_protocol_codec_v1* self,
                       const shield_protocol_decode_args_v1* args,
                       shield_protocol_decode_result_v1* out,
                       shield_error_v1* err) {
    if (!self || !args || !out || !self->user_data) {
        fill_error(err, "protocol.decode_failed",
                   "invalid flatbuffers decode args");
        return -1;
    }
    auto* inst = static_cast<flatbuffers_instance*>(self->user_data);
    const auto* obj = resolve_object(*inst, *args);
    if (!obj) {
        fill_error(err, "protocol.schema_not_found",
                   "FlatBuffers object type was not found");
        return -1;
    }

    if (args->payload == nullptr && args->payload_size > 0) {
        fill_error(err, "protocol.decode_failed",
                   "flatbuffers payload is null");
        return -1;
    }
    if (args->payload_size >
        static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        fill_error(err, "protocol.decode_failed",
                   "flatbuffers payload is too large");
        return -1;
    }

    const uint8_t* payload =
        args->payload_size == 0
            ? nullptr
            : reinterpret_cast<const uint8_t*>(args->payload);

    // Verify buffer
    if (payload) {
        flatbuffers::Verifier verifier(payload, args->payload_size);
        if (!verifier.VerifyBuffer<uoffset_t>()) {
            fill_error(err, "protocol.decode_failed",
                       "flatbuffers verification failed");
            return -1;
        }
    }

    // Convert to JSON using FlatBuffers parser
    std::string json;
    if (payload && args->payload_size > 0) {
        // Create parser and deserialize schema
        flatbuffers::Parser parser;
        if (!parser.Deserialize(
                reinterpret_cast<const uint8_t*>(inst->schema_data.data()),
                inst->schema_data.size())) {
            fill_error(err, "protocol.decode_failed",
                       "failed to deserialize FlatBuffers schema");
            return -1;
        }

        // Generate JSON from binary
        if (!GenerateText(parser, payload, &json)) {
            fill_error(err, "protocol.decode_failed",
                       "failed to convert FlatBuffers to JSON");
            return -1;
        }
    } else {
        json = "{}";
    }

    out->message_json = dup_string(json);
    out->message_json_size = json.size();
    return out->message_json ? 0 : -1;
}

int flatbuffers_encode(const shield_protocol_codec_v1* self,
                       const shield_protocol_encode_args_v1* args,
                       shield_protocol_encode_result_v1* out,
                       shield_error_v1* err) {
    if (!self || !args || !out || !self->user_data) {
        fill_error(err, "protocol.encode_failed",
                   "invalid flatbuffers encode args");
        return -1;
    }
    auto* inst = static_cast<flatbuffers_instance*>(self->user_data);
    const auto* obj = resolve_object(*inst, *args);
    if (!obj) {
        fill_error(err, "protocol.schema_not_found",
                   "FlatBuffers object type was not found");
        return -1;
    }

    if (args->message_json == nullptr && args->message_json_size > 0) {
        fill_error(err, "protocol.encode_failed",
                   "flatbuffers JSON input is null");
        return -1;
    }

    std::string json;
    if (args->message_json != nullptr && args->message_json_size > 0) {
        json.assign(args->message_json,
                    args->message_json + args->message_json_size);
    }

    // Create parser and deserialize schema
    flatbuffers::Parser parser;
    if (!parser.Deserialize(
            reinterpret_cast<const uint8_t*>(inst->schema_data.data()),
            inst->schema_data.size())) {
        fill_error(err, "protocol.encode_failed",
                   "failed to deserialize FlatBuffers schema");
        return -1;
    }

    // Parse JSON and create FlatBuffer
    if (!parser.Parse(json.c_str())) {
        fill_error(err, "protocol.encode_failed",
                   "failed to parse JSON for FlatBuffers");
        return -1;
    }

    auto buf_ptr = parser.builder_.GetBufferPointer();
    auto buf_size = parser.builder_.GetSize();

    std::vector<uint8_t> buffer(buf_ptr, buf_ptr + buf_size);

    out->payload = dup_bytes(buffer);
    out->payload_size = buffer.size();
    return out->payload ? 0 : -1;
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

int flatbuffers_create(const shield_plugin_create_args_v1* args,
                       shield_plugin_instance_v1** out, shield_error_v1* err) {
    if (out == nullptr) {
        fill_error(err, "plugin.create.invalid",
                   "flatbuffers create output pointer is null", "create");
        return -1;
    }
    auto* inst = new flatbuffers_instance;
    inst->instance_id = (args && args->instance_id) ? args->instance_id : "";

    std::string config_error;
    if (!load_config(inst, args ? args->config_json : nullptr, &config_error)) {
        fill_error(err, "plugin.config.invalid",
                   "invalid flatbuffers plugin config", "create");
        delete inst;
        return -1;
    }
    if (!load_schema(inst, &config_error)) {
        fill_error(err, "plugin.config.invalid",
                   "failed to load FlatBuffers schema", "create");
        delete inst;
        return -1;
    }

    inst->codec.struct_size = sizeof(shield_protocol_codec_v1);
    inst->codec.codec_name = "flatbuffers";
    inst->codec.version = "1.0.0";
    inst->codec.user_data = inst;
    inst->codec.decode = flatbuffers_decode;
    inst->codec.encode = flatbuffers_encode;
    inst->codec.free_decode_result = free_decode_result;
    inst->codec.free_encode_result = free_encode_result;

    inst->shell.struct_size = sizeof(flatbuffers_instance);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1* self,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        auto* inst = reinterpret_cast<flatbuffers_instance*>(self);
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
        delete reinterpret_cast<flatbuffers_instance*>(self);
    };
    inst->shell.register_lua = [](shield_plugin_instance_v1*, lua_State*,
                                  shield_error_v1*) { return 0; };

    *out = &inst->shell;
    return 0;
}

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT const shield_plugin_abi_v1*
shield_plugin_get_v1(void) {
    static const shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION, sizeof(shield_plugin_abi_v1),
        "protocol.flatbuffers",    "1.0.0",
        flatbuffers_create,
    };
    return &abi;
}
