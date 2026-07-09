// [SHIELD_PLUGIN] protocol.msgpack - shield.protocol.codec.v1 provider.

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/protocol_codec.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

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

void fill_error(shield_error_v1* err, const char* code,
                const char* message, const char* phase = "runtime") {
    if (err == nullptr) return;
    err->code = code;
    err->message = message;
    err->phase = phase;
}

struct msgpack_instance {
    shield_plugin_instance_v1 shell{};
    shield_protocol_codec_v1 codec{};
    std::string instance_id;
};

int msgpack_decode(const shield_protocol_codec_v1* self,
                   const shield_protocol_decode_args_v1* args,
                   shield_protocol_decode_result_v1* out,
                   shield_error_v1* err) {
    if (self == nullptr || args == nullptr || out == nullptr ||
        self->user_data == nullptr) {
        fill_error(err, "protocol.decode_failed", "invalid msgpack decode args");
        return -1;
    }
    if (args->payload == nullptr && args->payload_size > 0) {
        fill_error(err, "protocol.decode_failed", "msgpack payload is null");
        return -1;
    }
    if (args->payload_size >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        fill_error(err, "protocol.decode_failed", "msgpack payload is too large");
        return -1;
    }

    try {
        const auto size = static_cast<std::size_t>(args->payload_size);
        const std::uint8_t empty_payload[] = {0};
        const auto* begin = size == 0 ? empty_payload : args->payload;
        const auto* end = begin + size;
        const auto message = nlohmann::json::from_msgpack(begin, end);
        const auto json = message.dump();
        out->message_json = dup_string(json);
        out->message_json_size = json.size();
        return out->message_json == nullptr ? -1 : 0;
    } catch (const std::exception& ex) {
        fill_error(err, "protocol.decode_failed", ex.what());
        return -1;
    }
}

int msgpack_encode(const shield_protocol_codec_v1* self,
                   const shield_protocol_encode_args_v1* args,
                   shield_protocol_encode_result_v1* out,
                   shield_error_v1* err) {
    if (self == nullptr || args == nullptr || out == nullptr ||
        self->user_data == nullptr) {
        fill_error(err, "protocol.encode_failed", "invalid msgpack encode args");
        return -1;
    }
    if (args->message_json == nullptr && args->message_json_size > 0) {
        fill_error(err, "protocol.encode_failed", "msgpack JSON input is null");
        return -1;
    }
    if (args->message_json_size >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        fill_error(err, "protocol.encode_failed", "msgpack JSON input is too large");
        return -1;
    }

    try {
        nlohmann::json message = nlohmann::json::object();
        const auto size = static_cast<std::size_t>(args->message_json_size);
        if (args->message_json != nullptr && size > 0) {
            message = nlohmann::json::parse(
                args->message_json,
                args->message_json + size);
        }
        const auto payload = nlohmann::json::to_msgpack(message);
        out->payload = dup_bytes(payload);
        out->payload_size = payload.size();
        return payload.empty() || out->payload != nullptr ? 0 : -1;
    } catch (const std::exception& ex) {
        fill_error(err, "protocol.encode_failed", ex.what());
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

int msgpack_create(const shield_plugin_create_args_v1* args,
                   shield_plugin_instance_v1** out,
                   shield_error_v1* err) {
    if (out == nullptr) {
        fill_error(err, "plugin.create.invalid",
                   "msgpack create output pointer is null", "create");
        return -1;
    }

    auto* inst = new msgpack_instance;
    inst->instance_id = (args && args->instance_id) ? args->instance_id : "";

    inst->codec.struct_size = sizeof(shield_protocol_codec_v1);
    inst->codec.codec_name = "msgpack";
    inst->codec.version = "1.0.0";
    inst->codec.user_data = inst;
    inst->codec.decode = msgpack_decode;
    inst->codec.encode = msgpack_encode;
    inst->codec.free_decode_result = free_decode_result;
    inst->codec.free_encode_result = free_encode_result;

    inst->shell.struct_size = sizeof(msgpack_instance);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1* self,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        auto* inst = reinterpret_cast<msgpack_instance*>(self);
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
        delete reinterpret_cast<msgpack_instance*>(self);
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
        "protocol.msgpack",
        "1.0.0",
        msgpack_create,
    };
    return &abi;
}
