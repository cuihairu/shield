// Coverage tests for src/lua/lua_crypto.cpp (shield.crypto) and the
// scripts/lib/jwt.lua reference implementation built on it.
//
// Codec/hash correctness is pinned by RFC vectors: RFC 4648 (base64 and
// base64url), RFC 6234 (SHA-256), RFC 4231 (HMAC-SHA256 test cases 1-4).
// jwt.lua cases verify sign/verify round-trips, the alg-none pin, and the
// exp/nbf/iss/aud validation matrix.
#define BOOST_TEST_MODULE CovLuaCrypto

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <cstdio>
#include <sol/sol.hpp>
#include <string>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_crypto.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#ifndef SHIELD_SOURCE_DIR
#define SHIELD_SOURCE_DIR "."
#endif

using namespace shield::lua;

namespace {

bool run_script(sol::state& lua, const std::string& code) {
    auto result = lua.safe_script(code, sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error e = result;
        std::fprintf(stderr, "lua error: %s\n", e.what());
        return false;
    }
    return true;
}

std::string hex_from_cpp(const std::string& raw) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out += digits[c >> 4];
        out += digits[c & 0x0F];
    }
    return out;
}

std::string bytes_from_hex(const std::string& hex) {
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
    }
    return out;
}

// Independent C++-side HMAC-SHA256 (OpenSSL direct) used to cross-check what
// jwt.sign produces from the same shield.crypto primitive.
std::string cpp_hmac_sha256(const std::string& key, const std::string& data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), md,
         &len);
    return std::string(reinterpret_cast<const char*>(md), len);
}

// Lua state with shield.crypto registered and jwt.lua loaded as global "jwt".
struct CryptoState {
    sol::state lua;

    CryptoState() {
        lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math,
                           sol::lib::table, sol::lib::os);
        sol::table shield = lua.create_table();
        shield::lua::register_crypto_api(shield);
        lua["shield"] = shield;
        sol::object jwt = lua.script_file(std::string(SHIELD_SOURCE_DIR) +
                                          "/scripts/lib/jwt.lua");
        lua["jwt"] = jwt;
    }
};

}  // namespace

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

BOOST_AUTO_TEST_SUITE(CovLuaCrypto)

// ---------------------------------------------------------------------------
// RFC 4648 test vectors (§4 standard base64, '=' padding).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Rfc4648Base64Vectors) {
    CryptoState s;
    BOOST_CHECK(
        run_script(s.lua, "assert(shield.crypto.base64_encode('') == '')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64_encode('f') == 'Zg==')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64_encode('fo') == 'Zm8=')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64_encode('foo') == 'Zm9v')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64_encode('foob') == 'Zm9vYg==')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64_encode('fooba') == 'Zm9vYmE=')"));
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.base64_encode('foobar') == "
                           "'Zm9vYmFy')"));
    // Round trips.
    for (const char* input :
         {"", "f", "fo", "foo", "foob", "fooba", "foobar"}) {
        BOOST_CHECK(run_script(
            s.lua,
            std::string("assert(shield.crypto.base64_decode(shield.crypto."
                        "base64_encode('") +
                input + "')) == '" + input + "')"));
    }
}

// RFC 4648 §5 base64url: '-'/'_' alphabet, padding omitted on encode.
BOOST_AUTO_TEST_CASE(Rfc4648Base64urlVectors) {
    CryptoState s;
    BOOST_CHECK(
        run_script(s.lua, "assert(shield.crypto.base64url_encode('') == '')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64url_encode('f') == 'Zg')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64url_encode('fo') == 'Zm8')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64url_encode('foo') == 'Zm9v')"));
    // Bytes that map to '+' and '/' in the standard alphabet become '-' and
    // '_' (0xfb -> 62,'w'; 0xfb 0xef -> '-','-','8').
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.base64url_encode('\\251') "
                           "== '-w')"));
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.base64url_encode('\\251"
                           "\\239') == '--8')"));
    // Decode accepts both padded and unpadded input, url and standard.
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.base64url_decode('Zg') == "
                           "'f')"));
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.base64url_decode('Zg==') "
                           "== 'f')"));
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.base64url_decode('Zm8') == "
                           "'fo')"));
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.base64_decode('Zm9vYg==') "
                           "== 'foob')"));
    // Standard decoder also accepts url alphabet (same value table).
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.base64_decode('--8') == "
                           "'\\251\\239')"));
}

BOOST_AUTO_TEST_CASE(Base64DecodeErrors) {
    CryptoState s;
    // Invalid character.
    BOOST_CHECK(
        !run_script(s.lua, "assert(shield.crypto.base64_decode('Zm9v!'))"));
    // Chars straddling the range checks' inner boundaries: '{' sits between
    // 'Z' and 'a', ':' between '9' and 'A'... every else-if arm of the value
    // table must reject them (4-char body so the length check passes first).
    BOOST_CHECK(
        !run_script(s.lua, "assert(shield.crypto.base64_decode('@@@@'))"));
    BOOST_CHECK(
        !run_script(s.lua, "assert(shield.crypto.base64_decode('{{{{'))"));
    BOOST_CHECK(
        !run_script(s.lua, "assert(shield.crypto.base64_decode('::::'))"));
    // Too much padding.
    BOOST_CHECK(
        !run_script(s.lua, "assert(shield.crypto.base64_decode('Z===='))"));
    // Impossible length (1 char of body).
    BOOST_CHECK(!run_script(s.lua, "assert(shield.crypto.base64_decode('Z'))"));
    BOOST_CHECK(
        !run_script(s.lua, "assert(shield.crypto.base64url_decode('Z'))"));
}

// The 62/63 rows of the value table ('+'/'-' and '/'/'_') are not hit by the
// RFC vectors above; decode one byte through every true-arm character.
// 0xfb encodes '+w==' / '-w', 0xff encodes '/w==' / '_w'.
BOOST_AUTO_TEST_CASE(Base64DecodeAlphabetArms) {
    CryptoState s;
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64_decode('+w==') == '\\251')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64_decode('/w==') == '\\255')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64url_decode('-w') == '\\251')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64url_decode('_w') == '\\255')"));
    // Encoders produce the same forms back.
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64_encode('\\251') == '+w==')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64_encode('\\255') == '/w==')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.base64url_encode('\\255') == '_w')"));
}

BOOST_AUTO_TEST_CASE(HexCodec) {
    CryptoState s;
    BOOST_CHECK(
        run_script(s.lua, "assert(shield.crypto.hex_encode('') == '')"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.hex_encode('\\x00\\xff') == '00ff')"));
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.hex_decode('00ff') == "
                           "'\\x00\\xff')"));
    // Upper-case hex accepted on decode; encode is lower-case.
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.hex_decode('00FF') == "
                           "'\\x00\\xff')"));
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.hex_encode(shield.crypto."
                           "hex_decode('DEADBEEF')) == 'deadbeef')"));
    // Errors: odd length, invalid character.
    BOOST_CHECK(!run_script(s.lua, "assert(shield.crypto.hex_decode('abc'))"));
    BOOST_CHECK(!run_script(s.lua, "assert(shield.crypto.hex_decode('zz'))"));
    // Nibble range checks: ':' is past '9', 'g' past 'f', 'G' past 'F',
    // '/' below '0' — and '0:'/'0/' drive the lo<0 arm with a valid hi nibble.
    BOOST_CHECK(!run_script(s.lua, "assert(shield.crypto.hex_decode('0/'))"));
    BOOST_CHECK(!run_script(s.lua, "assert(shield.crypto.hex_decode('0:'))"));
    BOOST_CHECK(!run_script(s.lua, "assert(shield.crypto.hex_decode('ag'))"));
    BOOST_CHECK(!run_script(s.lua, "assert(shield.crypto.hex_decode('0G'))"));
}

// RFC 6234 SHA-256 test vectors (plus the empty-string vector).
BOOST_AUTO_TEST_CASE(Rfc6234Sha256Vectors) {
    CryptoState s;
    BOOST_CHECK(run_script(
        s.lua,
        "assert(shield.crypto.hex_encode(shield.crypto.sha256('')) == "
        "'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855')"));
    BOOST_CHECK(run_script(
        s.lua,
        "assert(shield.crypto.hex_encode(shield.crypto.sha256('abc')) == "
        "'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')"));
    BOOST_CHECK(
        run_script(s.lua,
                   "local d = string.rep('a', 1000000)\n"
                   "assert(shield.crypto.hex_encode(shield.crypto.sha256(d)) "
                   "== "
                   "'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39cc"
                   "c7112cd0')"));
}

// RFC 4231 HMAC-SHA256 test cases 1-4.
BOOST_AUTO_TEST_CASE(Rfc4231HmacSha256Vectors) {
    CryptoState s;
    // TC1: key = 0x0b x20, data "Hi There".
    s.lua["k1"] = bytes_from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    BOOST_CHECK(run_script(
        s.lua,
        "assert(shield.crypto.hex_encode(shield.crypto.hmac_sha256(k1, 'Hi "
        "There')) == "
        "'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7')"));
    // TC2: "Jefe" / "what do ya want for nothing?".
    BOOST_CHECK(run_script(
        s.lua,
        "assert(shield.crypto.hex_encode(shield.crypto.hmac_sha256('Jefe', "
        "'what do ya want for nothing?')) == "
        "'5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843')"));
    // TC3: key 0xaa x20, data 0xdd x50 (built programmatically so the byte
    // counts are right by construction).
    const auto repeat_byte = [](unsigned char b, size_t n) {
        return std::string(n, static_cast<char>(b));
    };
    s.lua["k3"] = repeat_byte(0xaa, 20);
    s.lua["d3"] = repeat_byte(0xdd, 50);
    BOOST_CHECK(run_script(
        s.lua,
        "assert(shield.crypto.hex_encode(shield.crypto.hmac_sha256(k3, d3)) == "
        "'773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe')"));
    // TC4: key 0x01..0x19 (25 bytes), data 0xcd x50 (different byte from
    // TC3's 0xdd payload).
    s.lua["k4"] =
        bytes_from_hex("0102030405060708090a0b0c0d0e0f10111213141516171819");
    s.lua["d4"] = repeat_byte(0xcd, 50);
    BOOST_CHECK(run_script(
        s.lua,
        "assert(shield.crypto.hex_encode(shield.crypto.hmac_sha256(k4, d4)) == "
        "'82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b')"));
}

BOOST_AUTO_TEST_CASE(RandomBytes) {
    CryptoState s;
    BOOST_CHECK(
        run_script(s.lua, "assert(shield.crypto.random_bytes(0) == '')"));
    BOOST_CHECK(run_script(
        s.lua,
        "local r = shield.crypto.random_bytes(32)\n"
        "assert(#r == 32)\n"
        "assert(r ~= shield.crypto.random_bytes(32))  -- astronomically "
        "likely to differ"));
    BOOST_CHECK(!run_script(s.lua, "assert(shield.crypto.random_bytes(-1))"));
    BOOST_CHECK(!run_script(
        s.lua, "assert(shield.crypto.random_bytes(1048577))"));  // > 1 MiB
}

BOOST_AUTO_TEST_CASE(ConstantTimeCompare) {
    CryptoState s;
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.constant_time_compare('abc', 'abc'))"));
    BOOST_CHECK(run_script(
        s.lua,
        "assert(not shield.crypto.constant_time_compare('abc', 'abd'))"));
    BOOST_CHECK(run_script(
        s.lua,
        "assert(not shield.crypto.constant_time_compare('abc', 'abcd'))"));
    BOOST_CHECK(run_script(
        s.lua, "assert(shield.crypto.constant_time_compare('', ''))"));
}

// sign -> verify round trip plus a cross-language check: the signature
// segment must equal HMAC-SHA256(key, "header.payload") computed here with
// OpenSSL directly, base64url-encoded (unpadded).
BOOST_AUTO_TEST_CASE(JwtSignVerifyRoundTripAndCrossCheck) {
    CryptoState s;
    const bool ok = run_script(
        s.lua,
        "token = jwt.sign({sub = 'p1', iss = 'gate', admin = true, exp = 100}, "
        "'secret')\n"
        "local h, p, sig = token:match('([^%.]+)%.([^%.]+)%.([^%.]+)')\n"
        "assert(h and p and sig)\n"
        "assert(shield.crypto.base64url_decode(h):find('HS256', 1, true))\n"
        "local claims, code, msg = jwt.verify(token, 'secret', {now = 99})\n"
        "assert(claims and claims.sub == 'p1' and claims.admin == true and "
        "claims.iss == 'gate', tostring(code) .. ' ' .. tostring(msg))\n");
    BOOST_CHECK(ok);

    // Cross-check the signature bytes from C++.
    const sol::object tok = s.lua["token"];
    const std::string token = tok.as<std::string>();
    const size_t first = token.find('.');
    const size_t second = token.find('.', first + 1);
    BOOST_REQUIRE(first != std::string::npos && second != std::string::npos);
    const std::string signing_input = token.substr(0, second);
    const std::string sig = token.substr(second + 1);
    const sol::function b64url = s.lua["shield"]["crypto"]["base64url_encode"];
    const std::string expected =
        b64url(cpp_hmac_sha256("secret", signing_input));
    BOOST_CHECK_EQUAL(sig, expected);
}

BOOST_AUTO_TEST_CASE(JwtHeaderAndClaimTypes) {
    CryptoState s;
    // Nested claims and arrays encode/decode through the private JSON codec.
    BOOST_CHECK(
        run_script(s.lua,
                   "local token = jwt.sign({sub = 'p1', roles = {'a', 'b'}, "
                   "meta = {level = 3}}, 'k')\n"
                   "local claims = jwt.verify(token, 'k')\n"
                   "assert(claims.roles[1] == 'a' and claims.roles[2] == 'b')\n"
                   "assert(claims.meta.level == 3)\n"));
}

BOOST_AUTO_TEST_CASE(JwtRejections) {
    CryptoState s;
    // Tampered payload -> bad_signature.
    BOOST_CHECK(run_script(
        s.lua,
        "local token = jwt.sign({sub = 'p1', exp = os.time() + 600}, 'k')\n"
        "local h, p, sig = token:match('([^%.]+)%.([^%.]+)%.([^%.]+)')\n"
        "local forged = h .. '.' .. "
        "shield.crypto.base64url_encode('{\\\"sub\\\":"
        "\\\"p2\\\"}') .. '.' .. sig\n"
        "local claims, code = jwt.verify(forged, 'k')\n"
        "assert(not claims and code == 'bad_signature', tostring(code))\n"));
    // Wrong key -> bad_signature.
    BOOST_CHECK(run_script(
        s.lua,
        "local token = jwt.sign({sub = 'p1'}, 'key_a')\n"
        "local claims, code = jwt.verify(token, 'key_b')\n"
        "assert(not claims and code == 'bad_signature', tostring(code))\n"));
    // alg:none -> unsupported_alg (rejected before the signature matters).
    BOOST_CHECK(run_script(
        s.lua,
        "local raw = shield.crypto.base64url_encode('{\\\"alg\\\":\\\"none\\\","
        "\\\"typ\\\":\\\"JWT\\\"}') .. '.' ..\n"
        "    shield.crypto.base64url_encode('{\\\"sub\\\":\\\"p1\\\"}') .. "
        "'.'\n"
        "local claims, code = jwt.verify(raw .. 'AA', 'k')\n"
        "assert(not claims and code == 'unsupported_alg', tostring(code))\n"));
    // Expired (now past exp), honouring leeway.
    BOOST_CHECK(run_script(
        s.lua,
        "local token = jwt.sign({sub = 'p1', exp = 1000}, 'k')\n"
        "local _, code = jwt.verify(token, 'k', {now = 1001})\n"
        "assert(code == 'expired', tostring(code))\n"
        "local claims = jwt.verify(token, 'k', {now = 1001, leeway = 5})\n"
        "assert(claims and claims.sub == 'p1')\n"));
    // Not yet valid (nbf in the future).
    BOOST_CHECK(
        run_script(s.lua,
                   "local token = jwt.sign({sub = 'p1', nbf = 2000}, 'k')\n"
                   "local _, code = jwt.verify(token, 'k', {now = 1999})\n"
                   "assert(code == 'not_yet_valid', tostring(code))\n"
                   "local claims = jwt.verify(token, 'k', {now = 2000})\n"
                   "assert(claims)\n"));
    // Issuer / audience pinning; aud accepts string or array form.
    BOOST_CHECK(run_script(
        s.lua,
        "local token = jwt.sign({iss = 'gate', aud = 'players'}, 'k')\n"
        "assert(jwt.verify(token, 'k', {issuer = 'gate', audience = "
        "'players'}))\n"
        "local _, code = jwt.verify(token, 'k', {issuer = 'other'})\n"
        "assert(code == 'bad_issuer', tostring(code))\n"
        "_, code = jwt.verify(token, 'k', {audience = 'admins'})\n"
        "assert(code == 'bad_audience', tostring(code))\n"
        "local multi = jwt.sign({aud = {'a', 'players'}}, 'k')\n"
        "assert(jwt.verify(multi, 'k', {audience = 'players'}))\n"
        "_, code = jwt.verify(multi, 'k', {audience = 'zz'})\n"
        "assert(code == 'bad_audience', tostring(code))\n"));
    // Malformed tokens: segment counts, empty segments, garbage.
    BOOST_CHECK(run_script(s.lua,
                           "local _, code = jwt.verify('a.b', 'k')\n"
                           "assert(code == 'malformed', tostring(code))\n"
                           "_, code = jwt.verify('a.b.c.d', 'k')\n"
                           "assert(code == 'malformed', tostring(code))\n"
                           "_, code = jwt.verify('.b.c', 'k')\n"
                           "assert(code == 'malformed', tostring(code))\n"
                           "_, code = jwt.verify('!!!.???.*', 'k')\n"
                           "assert(code == 'malformed', tostring(code))\n"
                           "_, code = jwt.verify(42, 'k')\n"
                           "assert(code == 'malformed', tostring(code))\n"));
}

// The production registration path (register_full_shield_api) must expose the
// same shield.crypto table — this covers the call site in lua_api.cpp.
BOOST_AUTO_TEST_CASE(RegisteredViaFullShieldApi) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math,
                       sol::lib::table, sol::lib::os, sol::lib::coroutine);
    register_full_shield_api(lua, &manager, &runtime);
    BOOST_CHECK(run_script(lua,
                           "assert(type(shield.crypto) == 'table')\n"
                           "assert(shield.crypto.hex_encode('a') == '61')\n"
                           "assert(#shield.crypto.sha256('x') == 32)\n"));
}

BOOST_AUTO_TEST_SUITE_END()
