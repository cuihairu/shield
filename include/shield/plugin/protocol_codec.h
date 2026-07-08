// [SHIELD_PLUGIN] shield.protocol.codec.v1 interface.
//
// Protocol codec providers implement BodyCodec semantics behind a stable C
// ABI. The host keeps the fixed transport pipeline; plugins only translate
// between payload bytes and canonical JSON business messages.
#pragma once

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_PROTOCOL_CODEC_INTERFACE "shield.protocol.codec.v1"

struct shield_protocol_decode_args_v1 {
    uint32_t route_id;
    uint16_t codec_id;
    uint16_t schema_id;
    const char* route_name;
    const uint8_t* payload;
    uint64_t payload_size;
};

struct shield_protocol_decode_result_v1 {
    const char* message_json;
    uint64_t message_json_size;
};

struct shield_protocol_encode_args_v1 {
    uint32_t route_id;
    uint16_t codec_id;
    uint16_t schema_id;
    const char* route_name;
    const char* message_json;
    uint64_t message_json_size;
};

struct shield_protocol_encode_result_v1 {
    const uint8_t* payload;
    uint64_t payload_size;
};

struct shield_protocol_codec_v1 {
#ifdef __cplusplus
    static constexpr const char* interface_name =
        SHIELD_PROTOCOL_CODEC_INTERFACE;
#endif

    uint32_t struct_size;
    const char* codec_name;      // "protobuf" | "sproto" | ...
    const char* version;
    void* user_data;             // plugin-owned, may be NULL

    int (*decode)(const struct shield_protocol_codec_v1* self,
                  const struct shield_protocol_decode_args_v1* args,
                  struct shield_protocol_decode_result_v1* out,
                  struct shield_error_v1* err);

    int (*encode)(const struct shield_protocol_codec_v1* self,
                  const struct shield_protocol_encode_args_v1* args,
                  struct shield_protocol_encode_result_v1* out,
                  struct shield_error_v1* err);

    void (*free_decode_result)(
        const struct shield_protocol_codec_v1* self,
        struct shield_protocol_decode_result_v1* result);

    void (*free_encode_result)(
        const struct shield_protocol_codec_v1* self,
        struct shield_protocol_encode_result_v1* result);
};

#ifdef __cplusplus
}  // extern "C"
#endif
