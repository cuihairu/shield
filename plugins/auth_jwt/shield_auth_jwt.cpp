// [SHIELD_PLUGIN] JWT authentication plugin — PLACEHOLDER
//
// WARNING: This is a placeholder implementation for testing/development only.
// DO NOT use in production. It does NOT:
//   - Validate JWT signatures
//   - Check token expiry
//   - Use proper HMAC-SHA256
//   - Implement token blacklisting
//
// A production implementation MUST use a proper JWT library (e.g. jwt-cpp)
// and validate signatures cryptographically.

#include "shield/plugin/plugin.h"
#include "shield/plugin/auth_plugin.h"

#include <cstring>
#include <string>
#include <ctime>

namespace {

struct AuthConfig {
    std::string secret;
    std::string issuer;
    int token_expiry_seconds = 3600;
};

AuthConfig g_config;

// Base64 URL encode (simplified).
std::string base64url_encode(const std::string& input) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    int val = 0, valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            output.push_back(table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) output.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
    return output;
}

// Simple JWT creation (header.payload.signature).
// In production, use a proper JWT library. This is a minimal implementation.
std::string create_jwt(const std::string& user_id, const AuthConfig& config) {
    // Header: {"alg":"HS256","typ":"JWT"}
    std::string header = base64url_encode(R"({"alg":"HS256","typ":"JWT"})");

    // Payload: {"sub":"user_id","iss":"issuer","iat":now,"exp":now+ttl}
    auto now = std::time(nullptr);
    std::string payload_json = R"({"sub":")" + user_id +
        R"(","iss":")" + config.issuer +
        R"(","iat":)" + std::to_string(now) +
        R"(,"exp":)" + std::to_string(now + config.token_expiry_seconds) + "}";
    std::string payload = base64url_encode(payload_json);

    // Signature (simplified - in production use OpenSSL HMAC-SHA256).
    std::string signature_input = header + "." + payload;
    std::string signature = base64url_encode(signature_input + config.secret);

    return header + "." + payload + "." + signature;
}

// Simple JWT validation (check expiry only).
bool validate_jwt(const std::string& token, const AuthConfig& config,
                  std::string& user_id, std::string& error) {
    // Split token into parts.
    auto dot1 = token.find('.');
    auto dot2 = token.find('.', dot1 + 1);
    if (dot1 == std::string::npos || dot2 == std::string::npos) {
        error = "invalid_token_format";
        return false;
    }

    // In production, verify signature here.
    // For now, just extract the payload.
    std::string payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);

    // Simplified: extract user_id from a known position.
    // In production, properly decode base64 and parse JSON.
    if (payload_b64.empty()) {
        error = "invalid_payload";
        return false;
    }

    // For demo purposes, treat the payload as the user_id.
    user_id = payload_b64;
    return true;
}

int auth_init(const char* config_json, char* err_buf, int err_buf_size) {
    if (config_json && config_json[0]) {
        // Parse minimal config (secret, issuer, expiry).
        g_config.secret = config_json;  // Simplified: treat whole JSON as secret
    }
    return 0;
}

void auth_shutdown() {
    g_config = {};
}

int auth_authenticate(const shield_auth_request* req,
                      shield_auth_result* out) {
    if (!req || !req->token) {
        out->success = 0;
        out->error_code = "invalid_request";
        out->error_msg = "token is required";
        return -1;
    }

    std::string user_id, error;
    if (validate_jwt(req->token, g_config, user_id, error)) {
        out->success = 1;
        out->error_code = nullptr;   // NULL = no error, safe to free
        out->error_msg = nullptr;
        out->user_id = strdup(user_id.c_str());
        out->token = nullptr;
        out->token_expiry_ms = 0;
        out->roles_json = nullptr;
        out->extra_json = nullptr;
        return 0;
    } else {
        out->success = 0;
        out->error_code = strdup(error.c_str());
        out->error_msg = strdup(error.c_str());
        out->user_id = nullptr;
        out->token = nullptr;
        out->token_expiry_ms = 0;
        out->roles_json = nullptr;
        out->extra_json = nullptr;
        return -1;
    }
}

int auth_authorize(const char* user_id, const char* resource,
                   const char* action, shield_authz_result* out) {
    // Default: allow all (authorization is application-specific).
    out->allowed = 1;
    out->reason = nullptr;
    return 0;
}

int auth_refresh_token(const char* refresh_token, shield_auth_result* out) {
    // Create a new token for the user.
    std::string user_id, error;
    if (validate_jwt(refresh_token, g_config, user_id, error)) {
        std::string new_token = create_jwt(user_id, g_config);
        out->success = 1;
        out->error_code = "";
        out->error_msg = "";
        out->user_id = strdup(user_id.c_str());
        out->token = strdup(new_token.c_str());
        out->token_expiry_ms = std::time(nullptr) + g_config.token_expiry_seconds;
        return 0;
    }
    out->success = 0;
    out->error_code = "invalid_token";
    return -1;
}

int auth_invalidate(const char* token) {
    // JWT is stateless - can't invalidate without a blacklist.
    // In production, add token to a Redis blacklist.
    return 0;
}

void auth_free_result(shield_auth_result* result) {
    if (!result) return;
    if (result->error_code) std::free(const_cast<char*>(result->error_code));
    if (result->error_msg) std::free(const_cast<char*>(result->error_msg));
    if (result->user_id) std::free(const_cast<char*>(result->user_id));
    if (result->token) std::free(const_cast<char*>(result->token));
    if (result->roles_json) std::free(const_cast<char*>(result->roles_json));
    if (result->extra_json) std::free(const_cast<char*>(result->extra_json));
}

void auth_free_authz_result(shield_authz_result* result) {
    if (result && result->reason) {
        std::free(const_cast<char*>(result->reason));
    }
}

const shield_auth_plugin g_auth_plugin = {
    SHIELD_AUTH_ABI_VERSION,
    "jwt",
    "1.0.0",

    auth_init,
    auth_shutdown,
    auth_authenticate,
    auth_authorize,
    auth_refresh_token,
    auth_invalidate,
    auth_free_result,
    auth_free_authz_result,
};

const shield_plugin g_plugin = {
    SHIELD_PLUGIN_ABI_VERSION,
    SHIELD_PLUGIN_TYPE_AUTH,
    "shield_auth_jwt",
    "1.0.0",
    "JWT authentication plugin",
    "Shield",

    [](const shield_host_t, const shield_host_api*,
       const shield_plugin_config* config,
       char* err, int err_len) -> int {
        if (config && config->count > 0 && config->items[0].value) {
            return auth_init(config->items[0].value, err, err_len);
        }
        return auth_init(nullptr, err, err_len);
    },

    []() { auth_shutdown(); },
    []() -> int { return 1; },
    [](int) -> const shield_plugin_capability* {
        static shield_plugin_capability cap = {"auth", "1.0.0", "JWT authentication"};
        return &cap;
    },

    &g_auth_plugin,
};

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT
const struct shield_plugin* shield_plugin_api(void) {
    return &g_plugin;
}
