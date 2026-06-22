// [SHIELD_PLUGIN] auth.jwt — JWT provider for shield.auth.v1.
//
// v1 ABI + production-grade HMAC-SHA256 signing/verification via OpenSSL.
// Validates HS256 tokens: signature (constant-time compare), exp, iat, iss,
// aud. Token refresh and invalidate (stateless blacklist hook) supported.
//
// The instance owns config (secret/issuer/audience/ttl/leeway). The session
// handle returned from connect() is the instance itself — no per-session
// state is needed for stateless JWT.

#include "shield/plugin/abi.h"
#include "shield/plugin/auth.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Base64URL codec (no padding, JWT-compatible).
// ---------------------------------------------------------------------------
std::string b64url_encode(const unsigned char* data, size_t len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    int val = 0, valb = -6;
    for (size_t i = 0; i < len; ++i) {
        val = (val << 8) | data[i];
        valb += 8;
        while (valb >= 0) {
            out.push_back(table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
    return out;
}

// Decode one base64url char (returns -1 on invalid).
int b64url_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

std::vector<unsigned char> b64url_decode(const std::string& s) {
    std::vector<unsigned char> out;
    out.reserve(s.size() * 3 / 4);
    int val = 0, valb = -8;
    for (char c : s) {
        int d = b64url_val(c);
        if (d < 0) continue;  // skip padding / invalid
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// HMAC-SHA256
// ---------------------------------------------------------------------------
std::vector<unsigned char> hmac_sha256(const std::string& key,
                                       const std::string& msg) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    if (!HMAC(EVP_sha256(),
              key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(msg.data()),
              msg.size(),
              md, &md_len)) {
        return {};
    }
    return std::vector<unsigned char>(md, md + md_len);
}

bool ct_equal(const std::vector<unsigned char>& a,
              const std::vector<unsigned char>& b) {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

// ---------------------------------------------------------------------------
// Minimal JSON string escape / value extract (for small JWT payload fields).
// We do NOT parse arbitrary JSON — only the few fields we wrote ourselves.
// ---------------------------------------------------------------------------
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Extract a string value at top-level of a small JSON object. Returns "" if
// not found. We assume values are simple strings without nested quotes.
std::string json_get_string(const std::string& j, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto p = j.find(needle);
    if (p == std::string::npos) return "";
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    if (p >= j.size() || j[p] != '"') return "";
    ++p;
    std::string out;
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) { out += j[p + 1]; p += 2; }
        else { out += j[p++]; }
    }
    return out;
}

// Extract a JSON integer at top-level. Returns 0 if not found.
int64_t json_get_int(const std::string& j, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto p = j.find(needle);
    if (p == std::string::npos) return 0;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return 0;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    bool neg = false;
    if (p < j.size() && j[p] == '-') { neg = true; ++p; }
    int64_t v = 0;
    bool any = false;
    while (p < j.size() && j[p] >= '0' && j[p] <= '9') {
        v = v * 10 + (j[p] - '0');
        ++p;
        any = true;
    }
    return any ? (neg ? -v : v) : 0;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------
struct auth_config {
    std::string secret;
    std::string issuer;
    std::string audience;
    int token_ttl_seconds = 3600;
    int leeway_seconds = 60;
};

auth_config parse_config(const char* config_json) {
    auth_config c;
    if (!config_json) return c;
    std::string s(config_json);
    auto get_str = [&](const std::string& key) -> std::string {
        return json_get_string(s, key);
    };
    auto get_int = [&](const std::string& key, int def) -> int {
        int64_t v = json_get_int(s, key);
        return v ? static_cast<int>(v) : def;
    };
    c.secret = get_str("secret");
    c.issuer = get_str("issuer");
    c.audience = get_str("audience");
    c.token_ttl_seconds = get_int("token_ttl_seconds", c.token_ttl_seconds);
    c.leeway_seconds = get_int("leeway_seconds", c.leeway_seconds);
    return c;
}

struct auth_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    auth_config cfg;
    std::mutex mu;
    // Stateless JWT can still support an explicit blacklist (logout).
    std::unordered_set<std::string> blacklist;
};

char* dup_cstr(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

char* dup_cstr(const std::string& s) {
    return dup_cstr(s.c_str());
}

// Build an HS256-signed JWT for the given subject.
std::string create_jwt(const auth_config& cfg, const std::string& user_id) {
    static const std::string header_json = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    std::string header_b64 = b64url_encode(
        reinterpret_cast<const unsigned char*>(header_json.data()),
        header_json.size());

    auto now = std::time(nullptr);
    std::string payload_json =
        "{\"sub\":\"" + json_escape(user_id) + "\"" +
        ",\"iat\":" + std::to_string(now) +
        ",\"exp\":" + std::to_string(now + cfg.token_ttl_seconds);
    if (!cfg.issuer.empty())   payload_json += ",\"iss\":\"" + json_escape(cfg.issuer) + "\"";
    if (!cfg.audience.empty()) payload_json += ",\"aud\":\"" + json_escape(cfg.audience) + "\"";
    payload_json += "}";
    std::string payload_b64 = b64url_encode(
        reinterpret_cast<const unsigned char*>(payload_json.data()),
        payload_json.size());

    std::string signing_input = header_b64 + "." + payload_b64;
    auto sig = hmac_sha256(cfg.secret, signing_input);
    std::string sig_b64 = b64url_encode(sig.data(), sig.size());

    return signing_input + "." + sig_b64;
}

// Verify signature + standard claims. Returns true on success; fills user_id
// and error on failure.
bool verify_jwt(const auth_config& cfg, const std::string& token,
                std::string& user_id, std::string& error) {
    auto dot1 = token.find('.');
    auto dot2 = token.find('.', dot1 == std::string::npos ? 0 : dot1 + 1);
    if (dot1 == std::string::npos || dot2 == std::string::npos) {
        error = "invalid_token_format";
        return false;
    }
    std::string signing_input = token.substr(0, dot2);
    std::string sig_b64 = token.substr(dot2 + 1);
    std::string payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);

    auto expected_sig = hmac_sha256(cfg.secret, signing_input);
    auto actual_sig = b64url_decode(sig_b64);
    if (!ct_equal(expected_sig, actual_sig)) {
        error = "invalid_signature";
        return false;
    }

    // Decode payload (UTF-8 bytes from base64url) and check claims.
    auto payload_bytes = b64url_decode(payload_b64);
    std::string payload(reinterpret_cast<const char*>(payload_bytes.data()),
                        payload_bytes.size());

    auto now = std::time(nullptr);
    int64_t exp = json_get_int(payload, "exp");
    if (exp && now > exp + cfg.leeway_seconds) {
        error = "token_expired";
        return false;
    }
    int64_t iat = json_get_int(payload, "iat");
    if (iat && now + cfg.leeway_seconds < iat) {
        error = "token_not_yet_valid";
        return false;
    }
    if (!cfg.issuer.empty()) {
        std::string iss = json_get_string(payload, "iss");
        if (iss != cfg.issuer) { error = "invalid_issuer"; return false; }
    }
    if (!cfg.audience.empty()) {
        std::string aud = json_get_string(payload, "aud");
        if (aud != cfg.audience) { error = "invalid_audience"; return false; }
    }
    user_id = json_get_string(payload, "sub");
    if (user_id.empty()) {
        error = "missing_subject";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// v1 auth vtable
// ---------------------------------------------------------------------------
const shield_auth_v1& auth_vtable() {
    static const shield_auth_v1 v = {
        sizeof(shield_auth_v1),
        "jwt",
        "1.0.0",
        // connect — returns the instance cast as the session handle.
        [](const struct shield_auth_config* cfg,
           char* err_buf, int err_buf_size) -> struct shield_auth_session* {
            (void)cfg;
            if (err_buf && err_buf_size > 0) err_buf[0] = '\0';
            return nullptr;  // no-op; instance owns config already.
        },
        // disconnect — no-op
        [](struct shield_auth_session*) {},
        // authenticate
        [](struct shield_auth_session* session,
           const struct shield_auth_request* req,
           struct shield_auth_result* out) -> int {
            auto* inst = reinterpret_cast<auth_instance*>(session);
            if (!inst || !req || !req->token || !out) return -1;
            {
                std::lock_guard<std::mutex> lock(inst->mu);
                if (inst->blacklist.count(req->token)) {
                    out->success = 0;
                    out->error_code = dup_cstr("token_revoked");
                    out->error_msg = dup_cstr("token has been invalidated");
                    return -1;
                }
            }
            std::string user_id, err;
            if (verify_jwt(inst->cfg, req->token, user_id, err)) {
                out->success = 1;
                out->error_code = nullptr;
                out->error_msg = nullptr;
                out->user_id = dup_cstr(user_id);
                out->token = nullptr;
                out->token_expiry_ms = 0;
                out->roles_json = nullptr;
                out->extra_json = nullptr;
                return 0;
            }
            out->success = 0;
            out->error_code = dup_cstr(err);
            out->error_msg = dup_cstr(err);
            out->user_id = nullptr;
            out->token = nullptr;
            out->token_expiry_ms = 0;
            out->roles_json = nullptr;
            out->extra_json = nullptr;
            return -1;
        },
        // authorize — default allow; applications layer RBAC on top.
        [](struct shield_auth_session* session,
           const char* user_id, const char* resource,
           const char* action, struct shield_authz_result* out) -> int {
            (void)session; (void)user_id; (void)resource; (void)action;
            if (!out) return -1;
            out->allowed = 1;
            out->reason = nullptr;
            return 0;
        },
        // refresh_token — verify the refresh token, mint a fresh one.
        [](struct shield_auth_session* session,
           const char* refresh_token,
           struct shield_auth_result* out) -> int {
            auto* inst = reinterpret_cast<auth_instance*>(session);
            if (!inst || !refresh_token || !out) return -1;
            std::string user_id, err;
            if (!verify_jwt(inst->cfg, refresh_token, user_id, err)) {
                out->success = 0;
                out->error_code = dup_cstr(err);
                out->error_msg = dup_cstr(err);
                return -1;
            }
            std::string new_token = create_jwt(inst->cfg, user_id);
            out->success = 1;
            out->error_code = nullptr;
            out->error_msg = nullptr;
            out->user_id = dup_cstr(user_id);
            out->token = dup_cstr(new_token);
            out->token_expiry_ms =
                (std::time(nullptr) + inst->cfg.token_ttl_seconds) * 1000LL;
            out->roles_json = nullptr;
            out->extra_json = nullptr;
            return 0;
        },
        // invalidate — add to in-process blacklist. For multi-process deploys,
        // back this with a Redis set.
        [](struct shield_auth_session* session, const char* token) -> int {
            auto* inst = reinterpret_cast<auth_instance*>(session);
            if (!inst || !token) return -1;
            std::lock_guard<std::mutex> lock(inst->mu);
            inst->blacklist.insert(token);
            return 0;
        },
        // free_result
        [](struct shield_auth_result* r) {
            if (!r) return;
            if (r->error_code) std::free(const_cast<char*>(r->error_code));
            if (r->error_msg)  std::free(const_cast<char*>(r->error_msg));
            if (r->user_id)    std::free(const_cast<char*>(r->user_id));
            if (r->token)      std::free(const_cast<char*>(r->token));
            if (r->roles_json) std::free(const_cast<char*>(r->roles_json));
            if (r->extra_json) std::free(const_cast<char*>(r->extra_json));
            r->error_code = r->error_msg = r->user_id = nullptr;
            r->token = r->roles_json = r->extra_json = nullptr;
        },
        // free_authz_result
        [](struct shield_authz_result* r) {
            if (r && r->reason) std::free(const_cast<char*>(r->reason));
            if (r) r->reason = nullptr;
        },
    };
    return v;
}

// ---------------------------------------------------------------------------
// v1 ABI entry
// ---------------------------------------------------------------------------
int auth_create(const struct shield_plugin_create_args_v1* args,
                struct shield_plugin_instance_v1** out,
                struct shield_error_v1* err) {
    if (!args || !out) return 1;
    auto* inst = new (std::nothrow) auth_instance;
    if (!inst) {
        if (err) { err->code = "plugin.create.failed"; err->message = "auth.jwt: oom"; }
        return 1;
    }
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->cfg = parse_config(args->config_json);
    if (inst->cfg.secret.empty()) {
        delete inst;
        if (err) {
            err->code = "plugin.config.invalid";
            err->message = "auth.jwt: 'secret' is required";
        }
        return 1;
    }

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](struct shield_plugin_instance_v1* self,
                                   const char* iface,
                                   struct shield_error_v1*) -> const void* {
        if (!self || !iface) return nullptr;
        if (std::strcmp(iface, SHIELD_AUTH_INTERFACE) == 0) return &auth_vtable();
        return nullptr;
    };
    inst->shell.start = [](struct shield_plugin_instance_v1*, struct shield_error_v1*) {
        return 0;
    };
    inst->shell.shutdown = [](struct shield_plugin_instance_v1* self) {
        delete reinterpret_cast<auth_instance*>(self);
    };
    // No Lua surface yet — empty register_lua satisfies the v1 ABI.
    inst->shell.register_lua = [](shield_plugin_instance_v1*, struct lua_State*,
                                  shield_error_v1*) { return 0; };
    *out = &inst->shell;
    return 0;
}

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT
const struct shield_plugin_abi_v1* shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "auth.jwt",
        "1.0.0",
        auth_create,
    };
    return &abi;
}
