// [SHIELD_PLUGIN] protocol.json - shield.protocol.codec.v1 provider.
//
// Drop-in validating replacement for the built-in json body codec: bound
// only when a listener sets network.protocol.body.provider. Routes without
// a configured schema decode exactly like the built-in codec (parse, unwrap
// the optional top-level {"payload": ...} envelope); routes with a schema
// are validated against it before delivery.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/protocol_codec.h"
#include "shield_payload_schema.hpp"

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
    if (err == nullptr) return;
    err->code = code;
    err->message = message;
    err->phase = phase;
}

struct json_instance {
    shield_plugin_instance_v1 shell{};
    shield_protocol_codec_v1 codec{};
    std::string instance_id;
    shield::plugins::payload_schema_settings settings;
    const shield_host_api_v1* host_api = nullptr;
    // Borrowed-pointer scratch for shield_error_v1::message: the host copies
    // it synchronously on a non-zero return, but it must point at storage
    // that outlives the call.
    std::string last_error;
};

// Shared optional-schema enforcement for decode/encode. Returns 0 to
// proceed (pass, no schema, or warn-and-pass), -1 with err filled on a
// reject. fail_code is protocol.decode_failed or protocol.encode_failed.
int enforce_payload_schema(json_instance* inst, const char* route_name,
                           const nlohmann::json& message, const char* fail_code,
                           shield_error_v1* err) {
    const auto* schema =
        shield::plugins::find_payload_schema(inst->settings, route_name);
    if (schema == nullptr) {
        if (inst->settings.require_schema) {
            inst->last_error =
                std::string("no payload schema configured for route '") +
                (route_name != nullptr ? route_name : "") + "'";
            fill_error(err, "protocol.schema_not_found",
                       inst->last_error.c_str());
            return -1;
        }
        return 0;
    }
    std::string violation;
    if (!shield::plugins::validate_payload_schema(*schema, message,
                                                  &violation)) {
        if (inst->settings.on_violation == "warn") {
            if (inst->host_api != nullptr && inst->host_api->log != nullptr) {
                inst->host_api->log(SHIELD_LOG_WARN, "protocol.json",
                                    inst->instance_id.c_str(),
                                    ("schema violation: " + violation).c_str());
            }
            return 0;
        }
        inst->last_error = "payload failed schema validation: " + violation;
        fill_error(err, fail_code, inst->last_error.c_str());
        return -1;
    }
    return 0;
}

// The business message behind an optional top-level {"payload": ...}
// envelope — same unwrap the built-in json codec applies via
// decode_structured_body, so the validating provider is a drop-in
// replacement. Schemas validate this business message, not the envelope.
nlohmann::json unwrap_payload(const nlohmann::json& message) {
    if (message.is_object() && message.contains("payload")) {
        return message["payload"];
    }
    return message;
}

int json_decode(const shield_protocol_codec_v1* self,
                const shield_protocol_decode_args_v1* args,
                shield_protocol_decode_result_v1* out, shield_error_v1* err) {
    if (self == nullptr || args == nullptr || out == nullptr ||
        self->user_data == nullptr) {
        fill_error(err, "protocol.decode_failed", "invalid json decode args");
        return -1;
    }
    if (args->payload == nullptr && args->payload_size > 0) {
        fill_error(err, "protocol.decode_failed", "json payload is null");
        return -1;
    }
    if (args->payload_size >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        fill_error(err, "protocol.decode_failed", "json payload is too large");
        return -1;
    }

    auto* inst = static_cast<json_instance*>(self->user_data);
    try {
        const auto size = static_cast<std::size_t>(args->payload_size);
        // An empty range throws, matching the built-in codec: unparseable
        // or missing bytes fail the decode.
        const auto* begin = reinterpret_cast<const char*>(args->payload);
        const auto message = nlohmann::json::parse(begin, begin + size);
        const auto business = unwrap_payload(message);
        if (enforce_payload_schema(inst, args->route_name, business,
                                   "protocol.decode_failed", err) != 0) {
            return -1;
        }
        const auto json = business.dump();
        out->message_json = dup_string(json);
        out->message_json_size = json.size();
        return out->message_json == nullptr ? -1 : 0;
    } catch (const std::exception& ex) {
        // Same contract as the built-in codec: unparseable bytes fail the
        // decode (which closes the session upstream).
        inst->last_error = ex.what();
        fill_error(err, "protocol.decode_failed", inst->last_error.c_str());
        return -1;
    }
}

int json_encode(const shield_protocol_codec_v1* self,
                const shield_protocol_encode_args_v1* args,
                shield_protocol_encode_result_v1* out, shield_error_v1* err) {
    if (self == nullptr || args == nullptr || out == nullptr ||
        self->user_data == nullptr) {
        fill_error(err, "protocol.encode_failed", "invalid json encode args");
        return -1;
    }
    if (args->message_json == nullptr && args->message_json_size > 0) {
        fill_error(err, "protocol.encode_failed", "json JSON input is null");
        return -1;
    }
    if (args->message_json_size >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        fill_error(err, "protocol.encode_failed",
                   "json JSON input is too large");
        return -1;
    }

    auto* inst = static_cast<json_instance*>(self->user_data);
    try {
        nlohmann::json message = nlohmann::json::object();
        const auto size = static_cast<std::size_t>(args->message_json_size);
        if (args->message_json != nullptr && size > 0) {
            message = nlohmann::json::parse(args->message_json,
                                            args->message_json + size);
        }
        if (enforce_payload_schema(inst, args->route_name, message,
                                   "protocol.encode_failed", err) != 0) {
            return -1;
        }
        const auto payload = message.dump();
        std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
        out->payload = dup_bytes(bytes);
        out->payload_size = bytes.size();
        return bytes.empty() || out->payload != nullptr ? 0 : -1;
    } catch (const std::exception& ex) {
        inst->last_error = ex.what();
        fill_error(err, "protocol.encode_failed", inst->last_error.c_str());
        return -1;
    }
}

void free_decode_result(const shield_protocol_codec_v1*,
                        shield_protocol_decode_result_v1* result) {
    if (result == nullptr) return;
    std::free(const_cast<char*>(result->message_json));
    result->message_json = nullptr;
    result->message_json_size = 0;
}

void free_encode_result(const shield_protocol_codec_v1*,
                        shield_protocol_encode_result_v1* result) {
    if (result == nullptr) return;
    std::free(const_cast<std::uint8_t*>(result->payload));
    result->payload = nullptr;
    result->payload_size = 0;
}

int json_create(const shield_plugin_create_args_v1* args,
                shield_plugin_instance_v1** out, shield_error_v1* err) {
    if (out == nullptr) {
        fill_error(err, "plugin.create.invalid",
                   "json create output pointer is null", "create");
        return -1;
    }

    auto* inst = new json_instance;
    inst->instance_id = (args && args->instance_id) ? args->instance_id : "";
    if (args != nullptr) {
        inst->host_api = args->host_api;
        // Optional payload-schema settings: shape violations fail create.
        // The manifest config_schema checks key types for host-driven
        // resolves; these guards are defense in depth. Detail goes to the
        // host log — err->message carries only a static literal because the
        // host reads it after create returns.
        std::string settings_error;
        nlohmann::json config = nlohmann::json::object();
        if (args->config_json != nullptr && args->config_json[0] != '\0') {
            config = nlohmann::json::parse(args->config_json, nullptr, false);
            if (config.is_discarded()) {
                fill_error(err, "plugin.create.failed",
                           "json config is not valid JSON", "create");
                delete inst;
                return -1;
            }
        }
        if (!shield::plugins::load_payload_schema_settings(
                inst->settings, config, &settings_error)) {
            if (inst->host_api != nullptr && inst->host_api->log != nullptr) {
                inst->host_api->log(SHIELD_LOG_ERROR, "protocol.json",
                                    inst->instance_id.c_str(),
                                    settings_error.c_str());
            }
            fill_error(err, "plugin.create.failed",
                       "invalid json payload schema settings", "create");
            delete inst;
            return -1;
        }
    }

    inst->codec.struct_size = sizeof(shield_protocol_codec_v1);
    inst->codec.codec_name = "json";
    inst->codec.version = "1.0.0";
    inst->codec.user_data = inst;
    inst->codec.decode = json_decode;
    inst->codec.encode = json_encode;
    inst->codec.free_decode_result = free_decode_result;
    inst->codec.free_encode_result = free_encode_result;

    inst->shell.struct_size = sizeof(json_instance);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1* self,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        auto* inst = reinterpret_cast<json_instance*>(self);
        if (iface != nullptr &&
            std::strcmp(iface, SHIELD_PROTOCOL_CODEC_INTERFACE) == 0) {
            return &inst->codec;
        }
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) {
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        delete reinterpret_cast<json_instance*>(self);
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
        "protocol.json",
        "1.0.0",
        json_create,
    };
    return &abi;
}
