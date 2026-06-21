// [SHIELD_PLUGIN] Authentication provider plugin C ABI
//
// Stable C interface for authentication backends (JWT, OAuth2, Steam,
// WeChat, custom game auth, etc.).
//
// Integration with shield_plugin system:
//   type = SHIELD_PLUGIN_TYPE_AUTH, vtable → shield_auth_plugin*

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_AUTH_ABI_VERSION 1

// Authentication request.
struct shield_auth_request {
    const char* token;             // JWT, OAuth token, game SDK token, etc.
    const char* provider;          // "jwt", "steam", "wechat", "custom", etc.
    const char* device_id;         // device identifier (optional)
    const char* extra_json;        // provider-specific data (optional)
};

// Authentication result.
struct shield_auth_result {
    int success;                   // 1 = authenticated, 0 = failed
    const char* error_code;        // NULL or stable error code
    const char* error_msg;         // NULL or human-readable message
    const char* user_id;           // authenticated user ID (NULL if failed)
    const char* token;             // new token (refresh, NULL if unchanged)
    int64_t token_expiry_ms;       // token expiry timestamp, 0 = unknown
    const char* roles_json;        // JSON array of roles, NULL = none
    const char* extra_json;        // provider-specific data, NULL = none
};

// Authorization check result.
struct shield_authz_result {
    int allowed;                   // 1 = allowed, 0 = denied
    const char* reason;            // NULL or denial reason
};

struct shield_auth_plugin {
    uint32_t abi_version;
    const char* name;              // "jwt", "steam", "wechat", "custom"
    const char* version;

    // Initialize the auth provider (load keys, connect to auth server, etc.)
    int (*init)(const char* config_json, char* err_buf, int err_buf_size);
    void (*shutdown)(void);

    // Authenticate a request (validate token, check credentials).
    int (*authenticate)(const struct shield_auth_request* req,
                        struct shield_auth_result* out);

    // Authorize an action (RBAC/ABAC check).
    int (*authorize)(const char* user_id,
                     const char* resource,
                     const char* action,
                     struct shield_authz_result* out);

    // Refresh an expiring token.
    int (*refresh_token)(const char* refresh_token,
                         struct shield_auth_result* out);

    // Invalidate a token (logout).
    int (*invalidate)(const char* token);

    // Memory
    void (*free_result)(struct shield_auth_result* result);
    void (*free_authz_result)(struct shield_authz_result* result);
};

// Entry point exported by every auth plugin DLL.
#ifdef _WIN32
#define SHIELD_AUTH_EXPORT __declspec(dllexport)
#else
#define SHIELD_AUTH_EXPORT __attribute__((visibility("default")))
#endif

SHIELD_AUTH_EXPORT
const struct shield_auth_plugin* shield_auth_plugin_api(void);

#ifdef __cplusplus
}
#endif
