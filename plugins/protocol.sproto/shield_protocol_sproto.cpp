// [SHIELD_PLUGIN] protocol.sproto — shield.protocol.codec.v1 provider.

extern "C" {
#include <sproto.h>
}

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

struct sproto_instance {
    shield_plugin_instance_v1 shell{};
    shield_protocol_codec_v1 codec{};
    std::string instance_id;
    std::string schema_path;
    std::string schema_string;
    bool enable_pack = false;

    struct sproto* sp = nullptr;
    std::unordered_map<std::uint16_t, std::string> schema_names;
    std::unordered_map<std::uint32_t, std::string> route_names;
};

// Callback for sproto encode - reads from JSON
struct encode_context {
    const nlohmann::json* data;
    std::string error;
};

static int encode_callback(const struct sproto_arg* args) {
    auto* ctx = static_cast<encode_context*>(args->ud);
    const auto& data = *ctx->data;

    std::string tagname(args->tagname);
    if (!data.contains(tagname)) {
        return 0;  // Skip nil fields
    }

    const auto& value = data[tagname];

    switch (args->type) {
        case SPROTO_TINTEGER: {
            if (!value.is_number_integer()) {
                ctx->error = "field '" + tagname + "' expected integer";
                return -1;
            }
            auto v = value.get<std::int64_t>();
            *static_cast<std::int64_t*>(args->value) = v;
            return 0;
        }
        case SPROTO_TBOOLEAN: {
            if (!value.is_boolean()) {
                ctx->error = "field '" + tagname + "' expected boolean";
                return -1;
            }
            *static_cast<int*>(args->value) = value.get<bool>() ? 1 : 0;
            return 0;
        }
        case SPROTO_TSTRING: {
            if (!value.is_string()) {
                ctx->error = "field '" + tagname + "' expected string";
                return -1;
            }
            auto str = value.get<std::string>();
            if (args->length < static_cast<int>(str.size())) {
                ctx->error = "field '" + tagname + "' string too long";
                return -1;
            }
            std::memcpy(args->value, str.data(), str.size());
            return static_cast<int>(str.size());
        }
        case SPROTO_TSTRUCT: {
            if (!value.is_object()) {
                ctx->error = "field '" + tagname + "' expected object";
                return -1;
            }
            // Nested struct - encode recursively
            auto* sub_ctx = new encode_context{&value, ""};
            int result =
                sproto_encode(args->subtype, args->value, args->length,
                              (sproto_callback)encode_callback, sub_ctx);
            if (result < 0) {
                ctx->error = sub_ctx->error;
            }
            delete sub_ctx;
            return result;
        }
        default:
            ctx->error = "unsupported type for field '" + tagname + "'";
            return -1;
    }
}

// Callback for sproto decode - writes to JSON
struct decode_context {
    nlohmann::json* data;
    std::string error;
};

static int decode_callback(const struct sproto_arg* args) {
    auto* ctx = static_cast<decode_context*>(args->ud);
    auto& data = *ctx->data;

    std::string tagname(args->tagname);

    switch (args->type) {
        case SPROTO_TINTEGER: {
            auto v = *static_cast<std::int64_t*>(args->value);
            data[tagname] = v;
            return 0;
        }
        case SPROTO_TBOOLEAN: {
            auto v = *static_cast<int*>(args->value);
            data[tagname] = (v != 0);
            return 0;
        }
        case SPROTO_TSTRING: {
            auto* str = static_cast<char*>(args->value);
            int len = args->length;
            if (len > 0) {
                data[tagname] = std::string(str, len);
            }
            return 0;
        }
        case SPROTO_TSTRUCT: {
            nlohmann::json sub_data = nlohmann::json::object();
            auto* sub_ctx = new decode_context{&sub_data, ""};
            int result =
                sproto_decode(args->subtype, args->value, args->length,
                              (sproto_callback)decode_callback, sub_ctx);
            if (result < 0) {
                ctx->error = sub_ctx->error;
            } else {
                data[tagname] = sub_data;
            }
            delete sub_ctx;
            return result;
        }
        default:
            ctx->error = "unsupported type for field '" + tagname + "'";
            return -1;
    }
}

const struct sproto_type* resolve_type(
    const sproto_instance& inst, const shield_protocol_decode_args_v1& args) {
    if (args.schema_id != 0) {
        const auto by_schema = inst.schema_names.find(args.schema_id);
        if (by_schema != inst.schema_names.end()) {
            return sproto_type(inst.sp, by_schema->second.c_str());
        }
    }
    if (args.route_name != nullptr && args.route_name[0] != '\0') {
        return sproto_type(inst.sp, args.route_name);
    }
    return nullptr;
}

const struct sproto_type* resolve_type(
    const sproto_instance& inst, const shield_protocol_encode_args_v1& args) {
    shield_protocol_decode_args_v1 decode_args{};
    decode_args.route_id = args.route_id;
    decode_args.schema_id = args.schema_id;
    decode_args.route_name = args.route_name;
    return resolve_type(inst, decode_args);
}

bool load_config(sproto_instance* inst, const char* config_json,
                 std::string* error) {
    nlohmann::json config = nlohmann::json::object();
    if (config_json && config_json[0] != '\0') {
        config = nlohmann::json::parse(config_json, nullptr, false);
        if (!config.is_object()) {
            if (error) *error = "sproto plugin config must be an object";
            return false;
        }
    }

    if (config.contains("schema") && config["schema"].is_string()) {
        inst->schema_path = config["schema"].get<std::string>();
    }

    if (config.contains("schema_string") &&
        config["schema_string"].is_string()) {
        inst->schema_string = config["schema_string"].get<std::string>();
    }

    if (inst->schema_path.empty() && inst->schema_string.empty()) {
        if (error) *error = "schema or schema_string is required";
        return false;
    }

    if (config.contains("pack") && config["pack"].is_boolean()) {
        inst->enable_pack = config["pack"].get<bool>();
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

bool load_schema(sproto_instance* inst, std::string* error) {
    std::string schema_data;

    if (!inst->schema_path.empty()) {
        auto data = read_file(inst->schema_path);
        if (!data) {
            if (error)
                *error = "failed to open schema file: " + inst->schema_path;
            return false;
        }
        schema_data = *data;
    } else {
        schema_data = inst->schema_string;
    }

    // Parse sproto schema
    struct sproto* sp =
        sproto_create(schema_data.data(), static_cast<int>(schema_data.size()));
    if (!sp) {
        if (error) *error = "failed to parse sproto schema";
        return false;
    }

    inst->sp = sp;
    return true;
}

int sproto_decode_func(const shield_protocol_codec_v1* self,
                       const shield_protocol_decode_args_v1* args,
                       shield_protocol_decode_result_v1* out,
                       shield_error_v1* err) {
    if (!self || !args || !out || !self->user_data) {
        fill_error(err, "protocol.decode_failed", "invalid sproto decode args");
        return -1;
    }
    auto* inst = static_cast<sproto_instance*>(self->user_data);
    const auto* type = resolve_type(*inst, *args);
    if (!type) {
        fill_error(err, "protocol.schema_not_found",
                   "sproto type was not found");
        return -1;
    }

    if (args->payload == nullptr && args->payload_size > 0) {
        fill_error(err, "protocol.decode_failed", "sproto payload is null");
        return -1;
    }
    if (args->payload_size >
        static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        fill_error(err, "protocol.decode_failed",
                   "sproto payload is too large");
        return -1;
    }

    const char empty_payload[] = "";
    const auto* payload = args->payload_size == 0
                              ? empty_payload
                              : reinterpret_cast<const char*>(args->payload);
    auto payload_size = static_cast<int>(args->payload_size);

    // Unpack if enabled
    std::vector<char> unpacked;
    if (inst->enable_pack) {
        int unpack_size = sproto_unpack(payload, payload_size, nullptr, 0);
        if (unpack_size < 0) {
            fill_error(err, "protocol.decode_failed", "sproto unpack failed");
            return -1;
        }
        unpacked.resize(unpack_size);
        sproto_unpack(payload, payload_size, unpacked.data(), unpack_size);
        payload = unpacked.data();
        payload_size = unpack_size;
    }

    // Decode
    nlohmann::json data = nlohmann::json::object();
    decode_context ctx{&data, ""};

    int result = sproto_decode(type, payload, payload_size,
                               (sproto_callback)decode_callback, &ctx);
    if (result < 0) {
        fill_error(
            err, "protocol.decode_failed",
            ctx.error.empty() ? "sproto decode failed" : ctx.error.c_str());
        return -1;
    }

    auto json = data.dump();
    out->message_json = dup_string(json);
    out->message_json_size = json.size();
    return out->message_json ? 0 : -1;
}

int sproto_encode_func(const shield_protocol_codec_v1* self,
                       const shield_protocol_encode_args_v1* args,
                       shield_protocol_encode_result_v1* out,
                       shield_error_v1* err) {
    if (!self || !args || !out || !self->user_data) {
        fill_error(err, "protocol.encode_failed", "invalid sproto encode args");
        return -1;
    }
    auto* inst = static_cast<sproto_instance*>(self->user_data);
    const auto* type = resolve_type(*inst, *args);
    if (!type) {
        fill_error(err, "protocol.schema_not_found",
                   "sproto type was not found");
        return -1;
    }

    if (args->message_json == nullptr && args->message_json_size > 0) {
        fill_error(err, "protocol.encode_failed", "sproto JSON input is null");
        return -1;
    }

    std::string json;
    if (args->message_json != nullptr && args->message_json_size > 0) {
        json.assign(args->message_json,
                    args->message_json + args->message_json_size);
    }

    nlohmann::json data;
    try {
        data = json.empty() ? nlohmann::json::object()
                            : nlohmann::json::parse(json);
    } catch (const std::exception& ex) {
        fill_error(err, "protocol.encode_failed", "invalid JSON input");
        return -1;
    }

    // Encode
    encode_context ctx{&data, ""};

    // First pass to get size
    int encode_size =
        sproto_encode(type, nullptr, 0, (sproto_callback)encode_callback, &ctx);
    if (encode_size < 0) {
        fill_error(
            err, "protocol.encode_failed",
            ctx.error.empty() ? "sproto encode failed" : ctx.error.c_str());
        return -1;
    }

    std::vector<char> buffer(encode_size);
    int result = sproto_encode(type, buffer.data(), encode_size,
                               (sproto_callback)encode_callback, &ctx);
    if (result < 0) {
        fill_error(
            err, "protocol.encode_failed",
            ctx.error.empty() ? "sproto encode failed" : ctx.error.c_str());
        return -1;
    }

    // Pack if enabled
    if (inst->enable_pack) {
        int pack_size = sproto_pack(buffer.data(), result, nullptr, 0);
        if (pack_size < 0) {
            fill_error(err, "protocol.encode_failed", "sproto pack failed");
            return -1;
        }
        std::vector<char> packed(pack_size);
        sproto_pack(buffer.data(), result, packed.data(), pack_size);
        buffer = std::move(packed);
        result = pack_size;
    }

    out->payload = dup_bytes({buffer.begin(), buffer.begin() + result});
    out->payload_size = result;
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

int sproto_create(const shield_plugin_create_args_v1* args,
                  shield_plugin_instance_v1** out, shield_error_v1* err) {
    if (out == nullptr) {
        fill_error(err, "plugin.create.invalid",
                   "sproto create output pointer is null", "create");
        return -1;
    }
    auto* inst = new sproto_instance;
    inst->instance_id = (args && args->instance_id) ? args->instance_id : "";

    std::string config_error;
    if (!load_config(inst, args ? args->config_json : nullptr, &config_error)) {
        fill_error(err, "plugin.config.invalid", "invalid sproto plugin config",
                   "create");
        delete inst;
        return -1;
    }
    if (!load_schema(inst, &config_error)) {
        fill_error(err, "plugin.config.invalid", "failed to load sproto schema",
                   "create");
        delete inst;
        return -1;
    }

    inst->codec.struct_size = sizeof(shield_protocol_codec_v1);
    inst->codec.codec_name = "sproto";
    inst->codec.version = "1.0.0";
    inst->codec.user_data = inst;
    inst->codec.decode = sproto_decode_func;
    inst->codec.encode = sproto_encode_func;
    inst->codec.free_decode_result = free_decode_result;
    inst->codec.free_encode_result = free_encode_result;

    inst->shell.struct_size = sizeof(sproto_instance);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1* self,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        auto* inst = reinterpret_cast<sproto_instance*>(self);
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
        auto* inst = reinterpret_cast<sproto_instance*>(self);
        if (inst->sp) {
            sproto_release(inst->sp);
        }
        delete inst;
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
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "protocol.sproto",
        "1.0.0",
        sproto_create,
    };
    return &abi;
}
