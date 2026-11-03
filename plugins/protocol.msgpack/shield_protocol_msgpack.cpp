// [SHIELD_PLUGIN] protocol.msgpack - shield.protocol.codec.v1 provider.

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

// Maximum nesting depth accepted for attacker-controlled msgpack payloads.
// from_msgpack() builds a DOM iteratively, but the later dump() recurses on
// the C++ stack and crashes on deep nesting. Guard at the boundary so a deep
// frame never becomes a deep DOM. Aligns with the JSON codec guard (512).
constexpr std::size_t kMaxMsgpackDepth = 512;

// Read a big-endian unsigned integer of n bytes from p, advancing p.
bool read_uint(const std::uint8_t*& p, const std::uint8_t* end,
               std::uint64_t& value, std::size_t n) {
    if (static_cast<std::size_t>(end - p) < n) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < n; ++i) {
        value = (value << 8) | p[i];
    }
    p += n;
    return true;
}

// Skip n bytes of opaque payload (str/bin/ext body), validating bounds.
bool skip_bytes(const std::uint8_t*& p, const std::uint8_t* end,
                std::uint64_t n) {
    if (n > static_cast<std::uint64_t>(end - p)) {
        return false;
    }
    p += static_cast<std::size_t>(n);
    return true;
}

// Reject counts that would overflow a multiplication or a +1 (map pairs and
// ext type bytes) instead of wrapping to a small value that lets the walker
// under-count a hostile payload.
bool count_within(std::uint64_t value, std::uint64_t multiplier = 1) {
    return value <= std::numeric_limits<std::uint64_t>::max() / multiplier;
}

// Non-recursive walker over raw msgpack bytes. Returns true iff the span is
// structurally a single msgpack value whose array/map nesting never exceeds
// max_depth. Conservative on malformed input: truncated or invalid bytes
// reject rather than guess. Scalars/str/bin are skipped, so the walk is
// linear in the payload size and cannot recurse on the C++ stack.
bool msgpack_nesting_within(const std::uint8_t* begin, const std::uint8_t* end,
                            std::size_t max_depth) {
    // Stack of (end-depth, remaining-container-items). We use a plain
    // vector of frames; depth is bounded by max_depth so the stack can never
    // grow past it.
    struct Frame {
        std::size_t depth;        // nesting depth of this container
        std::uint64_t remaining;  // unread items (0 for scalar/leaf)
    };
    std::vector<Frame> stack;
    const std::uint8_t* p = begin;

    // The first value must be a complete value; the loop drives one frame
    // until the container stack empties.
    std::uint64_t pending = 1;  // items left in the current frame
    std::size_t depth = 0;
    for (;;) {
        if (pending == 0) {
            // Finished the current container: pop the parent frame.
            if (stack.empty()) {
                return p == end;  // top-level done
            }
            const auto parent = stack.back();
            stack.pop_back();
            depth = parent.depth;
            pending = parent.remaining;
            continue;
        }
        if (p >= end) {
            return false;  // truncated
        }
        const std::uint8_t tag = *p++;

        // Positive fixint (0x00-0x7f), negative fixint (0xe0-0xff), nil
        // (0xc0), false/true (0xc2/0xc3) are all leaf scalars.
        if (tag <= 0x7f || tag >= 0xe0) {
            --pending;
            continue;
        }

        switch (tag) {
            case 0xc0:  // nil
            case 0xc2:  // false
            case 0xc3:  // true
                --pending;
                continue;

            case 0xc4: {  // bin 8
                std::uint64_t n = 0;
                if (!read_uint(p, end, n, 1)) return false;
                if (!skip_bytes(p, end, n)) return false;
                --pending;
                continue;
            }
            case 0xc5: {  // bin 16
                std::uint64_t n = 0;
                if (!read_uint(p, end, n, 2)) return false;
                if (!skip_bytes(p, end, n)) return false;
                --pending;
                continue;
            }
            case 0xc6: {  // bin 32
                std::uint64_t n = 0;
                if (!read_uint(p, end, n, 4)) return false;
                if (!skip_bytes(p, end, n)) return false;
                --pending;
                continue;
            }

            case 0xca: {  // float 32
                if (!skip_bytes(p, end, 4)) return false;
                --pending;
                continue;
            }
            case 0xcb: {  // float 64
                if (!skip_bytes(p, end, 8)) return false;
                --pending;
                continue;
            }
            case 0xcc: {  // uint 8
                if (!skip_bytes(p, end, 1)) return false;
                --pending;
                continue;
            }
            case 0xcd: {  // uint 16
                if (!skip_bytes(p, end, 2)) return false;
                --pending;
                continue;
            }
            case 0xce: {  // uint 32
                if (!skip_bytes(p, end, 4)) return false;
                --pending;
                continue;
            }
            case 0xcf: {  // uint 64
                if (!skip_bytes(p, end, 8)) return false;
                --pending;
                continue;
            }
            case 0xd0: {  // int 8
                if (!skip_bytes(p, end, 1)) return false;
                --pending;
                continue;
            }
            case 0xd1: {  // int 16
                if (!skip_bytes(p, end, 2)) return false;
                --pending;
                continue;
            }
            case 0xd2: {  // int 32
                if (!skip_bytes(p, end, 4)) return false;
                --pending;
                continue;
            }
            case 0xd3: {  // int 64
                if (!skip_bytes(p, end, 8)) return false;
                --pending;
                continue;
            }
            case 0xd4: {                                   // fixext 1
                if (!skip_bytes(p, end, 2)) return false;  // type + 1 byte
                --pending;
                continue;
            }
            case 0xd5: {                                   // fixext 2
                if (!skip_bytes(p, end, 3)) return false;  // type + 2 bytes
                --pending;
                continue;
            }
            case 0xd6: {                                   // fixext 4
                if (!skip_bytes(p, end, 5)) return false;  // type + 4 bytes
                --pending;
                continue;
            }
            case 0xd7: {                                   // fixext 8
                if (!skip_bytes(p, end, 9)) return false;  // type + 8 bytes
                --pending;
                continue;
            }
            case 0xd8: {                                    // fixext 16
                if (!skip_bytes(p, end, 17)) return false;  // type + 16 bytes
                --pending;
                continue;
            }
            case 0xd9: {  // str 8
                std::uint64_t n = 0;
                if (!read_uint(p, end, n, 1)) return false;
                if (!skip_bytes(p, end, n)) return false;
                --pending;
                continue;
            }
            case 0xda: {  // str 16
                std::uint64_t n = 0;
                if (!read_uint(p, end, n, 2)) return false;
                if (!skip_bytes(p, end, n)) return false;
                --pending;
                continue;
            }
            case 0xdb: {  // str 32
                std::uint64_t n = 0;
                if (!read_uint(p, end, n, 4)) return false;
                if (!skip_bytes(p, end, n)) return false;
                --pending;
                continue;
            }
            case 0xc7: {  // ext 8
                std::uint64_t n = 0;
                if (!read_uint(p, end, n, 1)) return false;
                if (!count_within(n) || !skip_bytes(p, end, n + 1))
                    return false;  // type + data
                --pending;
                continue;
            }
            case 0xc8: {  // ext 16
                std::uint64_t n = 0;
                if (!read_uint(p, end, n, 2)) return false;
                if (!count_within(n) || !skip_bytes(p, end, n + 1))
                    return false;
                --pending;
                continue;
            }
            case 0xc9: {  // ext 32
                std::uint64_t n = 0;
                if (!read_uint(p, end, n, 4)) return false;
                if (!count_within(n) || !skip_bytes(p, end, n + 1))
                    return false;
                --pending;
                continue;
            }

            case 0xdc: {  // array 16
                std::uint64_t n = 0;
                if (!read_uint(p, end, n, 2)) return false;
                if (n == 0) {
                    --pending;
                    continue;
                }
                if (depth + 1 > max_depth) return false;
                --pending;
                stack.push_back({depth, pending});
                depth = depth + 1;
                pending = n;
                continue;
            }
            case 0xdd: {  // array 32
                std::uint64_t n = 0;
                if (!read_uint(p, end, n, 4)) return false;
                if (n == 0) {
                    --pending;
                    continue;
                }
                if (depth + 1 > max_depth) return false;
                --pending;
                stack.push_back({depth, pending});
                depth = depth + 1;
                pending = n;
                continue;
            }
            case 0xde: {  // map 16
                std::uint64_t n = 0;
                if (!read_uint(p, end, n, 2)) return false;
                if (n == 0) {
                    --pending;
                    continue;
                }
                if (!count_within(n, 2) || depth + 1 > max_depth) return false;
                --pending;
                stack.push_back({depth, pending});
                depth = depth + 1;
                pending = n * 2;  // key + value
                continue;
            }
            case 0xdf: {  // map 32
                std::uint64_t n = 0;
                if (!read_uint(p, end, n, 4)) return false;
                if (n == 0) {
                    --pending;
                    continue;
                }
                if (!count_within(n, 2) || depth + 1 > max_depth) return false;
                --pending;
                stack.push_back({depth, pending});
                depth = depth + 1;
                pending = n * 2;  // key + value
                continue;
            }

            case 0xa0:
            case 0xa1:
            case 0xa2:
            case 0xa3:
            case 0xa4:
            case 0xa5:
            case 0xa6:
            case 0xa7:
            case 0xa8:
            case 0xa9:
            case 0xaa:
            case 0xab:
            case 0xac:
            case 0xad:
            case 0xae:
            case 0xaf:
            case 0xb0:
            case 0xb1:
            case 0xb2:
            case 0xb3:
            case 0xb4:
            case 0xb5:
            case 0xb6:
            case 0xb7:
            case 0xb8:
            case 0xb9:
            case 0xba:
            case 0xbb:
            case 0xbc:
            case 0xbd:
            case 0xbe:
            case 0xbf: {  // fixstr 0x00-0x1f
                const std::uint64_t n = static_cast<std::uint64_t>(tag & 0x1f);
                if (!skip_bytes(p, end, n)) return false;
                --pending;
                continue;
            }
            case 0x90:
            case 0x91:
            case 0x92:
            case 0x93:
            case 0x94:
            case 0x95:
            case 0x96:
            case 0x97:
            case 0x98:
            case 0x99:
            case 0x9a:
            case 0x9b:
            case 0x9c:
            case 0x9d:
            case 0x9e:
            case 0x9f: {  // fixarray 0x00-0x0f
                const std::uint64_t n = static_cast<std::uint64_t>(tag & 0x0f);
                if (n == 0) {
                    --pending;
                    continue;
                }
                if (depth + 1 > max_depth) return false;
                --pending;
                stack.push_back({depth, pending});
                depth = depth + 1;
                pending = n;
                continue;
            }
            case 0x80:
            case 0x81:
            case 0x82:
            case 0x83:
            case 0x84:
            case 0x85:
            case 0x86:
            case 0x87:
            case 0x88:
            case 0x89:
            case 0x8a:
            case 0x8b:
            case 0x8c:
            case 0x8d:
            case 0x8e:
            case 0x8f: {  // fixmap 0x00-0x0f
                const std::uint64_t n = static_cast<std::uint64_t>(tag & 0x0f);
                if (n == 0) {
                    --pending;
                    continue;
                }
                if (!count_within(n, 2) || depth + 1 > max_depth) return false;
                --pending;
                stack.push_back({depth, pending});
                depth = depth + 1;
                pending = n * 2;  // key + value
                continue;
            }

            default:
                return false;  // reserved/invalid tag (0xc1, etc.)
        }
    }
}

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

struct msgpack_instance {
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
int enforce_payload_schema(msgpack_instance* inst, const char* route_name,
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
                inst->host_api->log(SHIELD_LOG_WARN, "protocol.msgpack",
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

int msgpack_decode(const shield_protocol_codec_v1* self,
                   const shield_protocol_decode_args_v1* args,
                   shield_protocol_decode_result_v1* out,
                   shield_error_v1* err) {
    if (self == nullptr || args == nullptr || out == nullptr ||
        self->user_data == nullptr) {
        fill_error(err, "protocol.decode_failed",
                   "invalid msgpack decode args");
        return -1;
    }
    if (args->payload == nullptr && args->payload_size > 0) {
        fill_error(err, "protocol.decode_failed", "msgpack payload is null");
        return -1;
    }
    if (args->payload_size >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        fill_error(err, "protocol.decode_failed",
                   "msgpack payload is too large");
        return -1;
    }

    try {
        const auto size = static_cast<std::size_t>(args->payload_size);
        const std::uint8_t empty_payload[] = {0};
        const auto* begin = size == 0 ? empty_payload : args->payload;
        const auto* end = begin + size;
        if (!msgpack_nesting_within(begin, end, kMaxMsgpackDepth)) {
            fill_error(err, "protocol.decode_failed",
                       "msgpack payload exceeds maximum nesting depth");
            return -1;
        }
        const auto message = nlohmann::json::from_msgpack(begin, end);
        auto* inst = static_cast<msgpack_instance*>(self->user_data);
        if (enforce_payload_schema(inst, args->route_name, message,
                                   "protocol.decode_failed", err) != 0) {
            return -1;
        }
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
        fill_error(err, "protocol.encode_failed",
                   "invalid msgpack encode args");
        return -1;
    }
    if (args->message_json == nullptr && args->message_json_size > 0) {
        fill_error(err, "protocol.encode_failed", "msgpack JSON input is null");
        return -1;
    }
    if (args->message_json_size >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        fill_error(err, "protocol.encode_failed",
                   "msgpack JSON input is too large");
        return -1;
    }

    try {
        nlohmann::json message = nlohmann::json::object();
        const auto size = static_cast<std::size_t>(args->message_json_size);
        if (args->message_json != nullptr && size > 0) {
            // nlohmann's JSON parser is iterative, but to_msgpack() walks the
            // DOM recursively; guard deep input the same way as decode so a
            // hostile message cannot overflow the stack here either.
            const char* begin = args->message_json;
            const char* end = begin + size;
            std::size_t depth = 0;
            bool in_string = false;
            bool escaped = false;
            for (const char* it = begin; it != end; ++it) {
                const char c = *it;
                if (in_string) {
                    if (escaped) {
                        escaped = false;
                    } else if (c == '\\') {
                        escaped = true;
                    } else if (c == '"') {
                        in_string = false;
                    }
                    continue;
                }
                if (c == '"') {
                    in_string = true;
                } else if (c == '{' || c == '[') {
                    if (++depth > kMaxMsgpackDepth) {
                        fill_error(
                            err, "protocol.encode_failed",
                            "msgpack input exceeds maximum nesting depth");
                        return -1;
                    }
                } else if (c == '}' || c == ']') {
                    if (depth == 0) {
                        break;  // let the parser report the unbalanced close
                    }
                    --depth;
                }
            }
            message = nlohmann::json::parse(args->message_json,
                                            args->message_json + size);
        }
        auto* inst = static_cast<msgpack_instance*>(self->user_data);
        if (enforce_payload_schema(inst, args->route_name, message,
                                   "protocol.encode_failed", err) != 0) {
            return -1;
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
                   shield_plugin_instance_v1** out, shield_error_v1* err) {
    if (out == nullptr) {
        fill_error(err, "plugin.create.invalid",
                   "msgpack create output pointer is null", "create");
        return -1;
    }

    auto* inst = new msgpack_instance;
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
                           "msgpack config is not valid JSON", "create");
                delete inst;
                return -1;
            }
        }
        if (!shield::plugins::load_payload_schema_settings(
                inst->settings, config, &settings_error)) {
            if (inst->host_api != nullptr && inst->host_api->log != nullptr) {
                inst->host_api->log(SHIELD_LOG_ERROR, "protocol.msgpack",
                                    inst->instance_id.c_str(),
                                    settings_error.c_str());
            }
            fill_error(err, "plugin.create.failed",
                       "invalid msgpack payload schema settings", "create");
            delete inst;
            return -1;
        }
    }

    inst->codec.struct_size = sizeof(shield_protocol_codec_v1);
    inst->codec.codec_name = "msgpack";
    inst->codec.version = "1.1.0";
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
        "protocol.msgpack",
        "1.1.0",
        msgpack_create,
    };
    return &abi;
}
