// SEC-2 regression: verify_jwt must reject a token with a missing (or
// non-positive) exp instead of treating it as "never expires". The old
// json_get_int("exp") returned 0 for an absent key and the `if (exp && ...)`
// guard skipped the expiry check entirely, so a forged no-expiry token was
// accepted forever.
//
// Builds a no-exp HS256 token by hand (header.payload.signature with
// payload {"sub":"u1","iat":...} and no "exp"), then calls authenticate via
// the plugin ABI and asserts a token_expired error.
#define BOOST_TEST_MODULE AuthJwt
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <boost/test/unit_test.hpp>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "shield/plugin/abi.h"
#include "shield/plugin/auth.h"
#include "shield/plugin/plugin_library.hpp"

namespace {

using shield::plugin::PluginLibrary;

std::string b64url(const unsigned char* data, size_t len) {
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

std::vector<unsigned char> hmac_sha256(const std::string& key,
                                       const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest, &len);
    return std::vector<unsigned char>(digest, digest + len);
}

std::string make_no_exp_token(const std::string& secret,
                              const std::string& user_id) {
    const std::string header = R"({"alg":"HS256","typ":"JWT"})";
    const std::string payload =
        "{\"sub\":\"" + user_id + "\",\"iat\":1700000000}";
    const std::string h = b64url(
        reinterpret_cast<const unsigned char*>(header.data()), header.size());
    const std::string p = b64url(
        reinterpret_cast<const unsigned char*>(payload.data()), payload.size());
    const std::string signing_input = h + "." + p;
    auto sig = hmac_sha256(secret, signing_input);
    return signing_input + "." + b64url(sig.data(), sig.size());
}

}  // namespace

BOOST_AUTO_TEST_CASE(missing_exp_is_rejected) {
    std::string err;
    PluginLibrary lib = PluginLibrary::load(SHIELD_AUTH_JWT_LIBRARY, err);
    BOOST_REQUIRE_MESSAGE(lib.is_loaded(), err);

    using get_v1_fn = const shield_plugin_abi_v1* (*)();
    void* sym = lib.resolve("shield_plugin_get_v1");
    BOOST_REQUIRE_MESSAGE(sym != nullptr, "entry symbol missing");
    const shield_plugin_abi_v1* abi = reinterpret_cast<get_v1_fn>(sym)();
    BOOST_REQUIRE(abi != nullptr);

    shield_plugin_create_args_v1 cargs{};
    cargs.instance_id = "jwt_sec2";
    cargs.config_json = R"({"secret":"s3cr3t"})";
    shield_plugin_instance_v1* inst = nullptr;
    BOOST_REQUIRE_EQUAL(abi->create(&cargs, &inst, nullptr), 0);
    BOOST_REQUIRE(inst != nullptr);

    const auto* auth = static_cast<const shield_auth_v1*>(
        inst->get_interface(inst, SHIELD_AUTH_INTERFACE, nullptr));
    BOOST_REQUIRE(auth != nullptr);

    // auth.jwt's connect() is a no-op (the instance owns its config parsed at
    // create time); authenticate() takes the instance itself as the opaque
    // session handle.
    auto* session = reinterpret_cast<shield_auth_session*>(inst);

    const std::string token = make_no_exp_token("s3cr3t", "u1");
    shield_auth_request req{};
    req.token = token.c_str();
    shield_auth_result res{};
    BOOST_REQUIRE_EQUAL(auth->authenticate(session, &req, &res), -1);
    BOOST_CHECK_EQUAL(res.success, 0);
    BOOST_REQUIRE(res.error_code != nullptr);
    BOOST_CHECK_EQUAL(std::string(res.error_code), "token_expired");

    auth->free_result(&res);
    auth->disconnect(session);
    inst->shutdown(inst);
}
