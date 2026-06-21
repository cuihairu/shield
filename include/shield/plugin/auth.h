// [SHIELD_PLUGIN] shield.auth.v1 interface.
//
// Authentication / authorization provider. connect() initializes the
// provider (load keys, connect to auth server) and returns a session handle.
#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_AUTH_INTERFACE "shield.auth.v1"

struct shield_auth_session;  // opaque, plugin-defined

struct shield_auth_config {
    const char* issuer;
    const char* audience;
    const char* secret;          // HMAC secret (NULL if using public keys)
    const char* public_keys_json;// JWKS or PEM set (NULL for HMAC)
    int token_ttl_seconds;
    int leeway_seconds;
    const char* extra_json;
};

struct shield_auth_request {
    const char* token;
    const char* provider;
    const char* device_id;
    const char* extra_json;
};

struct shield_auth_result {
    int success;
    const char* error_code;
    const char* error_msg;
    const char* user_id;
    const char* token;            // refreshed token, NULL if unchanged
    int64_t token_expiry_ms;
    const char* roles_json;
    const char* extra_json;
};

struct shield_authz_result {
    int allowed;
    const char* reason;
};

struct shield_auth_v1 {
    uint32_t struct_size;
    const char* name;            // "jwt" | "steam" | "wechat" | ...
    const char* version;

    struct shield_auth_session* (*connect)(const struct shield_auth_config* cfg,
                                           char* err_buf, int err_buf_size);
    void (*disconnect)(struct shield_auth_session* session);

    int (*authenticate)(struct shield_auth_session* session,
                        const struct shield_auth_request* req,
                        struct shield_auth_result* out);
    int (*authorize)(struct shield_auth_session* session,
                     const char* user_id, const char* resource,
                     const char* action, struct shield_authz_result* out);
    int (*refresh_token)(struct shield_auth_session* session,
                         const char* refresh_token,
                         struct shield_auth_result* out);
    int (*invalidate)(struct shield_auth_session* session, const char* token);

    void (*free_result)(struct shield_auth_result* result);
    void (*free_authz_result)(struct shield_authz_result* result);
};

#ifdef __cplusplus
}
#endif
