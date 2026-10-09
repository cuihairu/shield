// Coverage tests for src/lua/lua_crypto.cpp (shield.crypto) and the
// scripts/lib/jwt.lua reference implementation built on it.
//
// Codec/hash correctness is pinned by RFC vectors: RFC 4648 (base64 and
// base64url), RFC 6234 (SHA-256), RFC 4231 (HMAC-SHA256 test cases 1-4),
// RFC 8032 (Ed25519 §7.2/§7.3), RFC 8037 A.1 (Ed25519 JWK + signature).
// jwt.lua cases verify sign/verify round-trips (HS256 and EdDSA), the alg
// pin, the kid pin, the exp/nbf/iss/aud validation matrix, and the JWKS
// build/parse helpers.
#define BOOST_TEST_MODULE CovLuaCrypto

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <cstdio>
#include <string>

#include "shield/caf_initializer.hpp"
#include "shield/lua/binding.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_crypto.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#ifndef SHIELD_SOURCE_DIR
#define SHIELD_SOURCE_DIR "."
#endif

using namespace shield::lua;

namespace {

bool run_script(shd::state& lua, const std::string& code) {
    auto result = lua.script(code);
    if (!result.valid()) {
        const shd::error e = result.get_error();
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

// Independent C++-side Ed25519 (OpenSSL direct) used to cross-check what
// jwt.sign produces from the same shield.crypto primitive.
std::string cpp_ed25519_sign(const std::string& seed, const std::string& msg) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(seed.data()), seed.size());
    BOOST_REQUIRE(pkey != nullptr);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    BOOST_REQUIRE(ctx != nullptr);
    BOOST_REQUIRE(EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) ==
                  1);
    size_t siglen = 64;
    std::string sig(64, '\0');
    BOOST_REQUIRE(
        EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(sig.data()),
                       &siglen,
                       reinterpret_cast<const unsigned char*>(msg.data()),
                       msg.size()) == 1);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    sig.resize(siglen);
    return sig;
}

// Lua state with shield.crypto registered and jwt.lua loaded as global "jwt".
struct CryptoState {
    shd::state lua;

    CryptoState() {
        lua.open_libraries(shd::lib::base | shd::lib::string | shd::lib::math |
                           shd::lib::table | shd::lib::os);
        shd::table shield = lua.create_table();
        shield::lua::register_crypto_api(shield);
        lua["shield"] = shield;
        shd::object jwt = lua.script_file(std::string(SHIELD_SOURCE_DIR) +
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
    const shd::object tok = s.lua["token"];
    const std::string token = tok.as<std::string>();
    const size_t first = token.find('.');
    const size_t second = token.find('.', first + 1);
    BOOST_REQUIRE(first != std::string::npos && second != std::string::npos);
    const std::string signing_input = token.substr(0, second);
    const std::string sig = token.substr(second + 1);
    const shd::function b64url = s.lua["shield"]["crypto"]["base64url_encode"];
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

// json_escape: quotes, backslashes and control characters survive the
// sign -> payload-segment -> verify round trip, and the emitted JSON carries
// the documented forms (named escapes for the JSON six, \u00xx for the
// other control bytes). Deterministic bytes: claims key insertion order must
// not leak into the token (keys are emitted sorted).
BOOST_AUTO_TEST_CASE(JwtJsonEscapeEncoding) {
    CryptoState s;
    BOOST_CHECK(run_script(s.lua, R"lua(
local sub = 'a"b\\c' .. string.char(10, 9, 0, 8, 12, 13, 1)
local token = jwt.sign({sub = sub}, 'k')
local enc = shield.crypto.base64url_decode(token:match('^.-%.(.-)%.'))
local bs = string.char(92)
assert(enc:find(bs .. '"', 1, true), enc)
assert(enc:find(bs .. bs, 1, true), enc)
assert(enc:find(bs .. 'n', 1, true), enc)
assert(enc:find(bs .. 't', 1, true), enc)
assert(enc:find(bs .. 'b', 1, true), enc)
assert(enc:find(bs .. 'f', 1, true), enc)
assert(enc:find(bs .. 'r', 1, true), enc)
assert(enc:find(bs .. 'u0000', 1, true), enc)
assert(enc:find(bs .. 'u0001', 1, true), enc)
local back = jwt.verify(token, 'k')
assert(back and back.sub == sub, tostring(back))
-- Determinism: insertion order must not change the token bytes.
local t1 = {b = 1}; t1.a = 2
local t2 = {a = 2}; t2.b = 1
assert(jwt.sign(t1, 'k') == jwt.sign(t2, 'k'))
)lua"));
}

// json_encode_value numeric arms: integers below 2^53 emit via %d, other
// numbers via %.14g (exponent form once 15+ digits), and non-finite numbers
// or non-serializable values are hard errors instead of silent corruption.
BOOST_AUTO_TEST_CASE(JwtJsonNumberEncoding) {
    CryptoState s;
    BOOST_CHECK(run_script(s.lua, R"lua(
local function payload_of(v)
    local tok = jwt.sign(v, 'k')
    return shield.crypto.base64url_decode(tok:match('^.-%.(.-)%.'))
end
assert(payload_of({n = 42}):find('"n":42', 1, true))
assert(payload_of({n = 1.5}):find('"n":1.5', 1, true))
-- 1e16 is integral but >= 2^53: emitted via %.14g in exponent form.
assert(payload_of({n = 1e16}):find('"n":1e+16', 1, true))
assert(payload_of({n = 9007199254740991}):find('"n":9007199254740991', 1, true))
local claims = jwt.verify(jwt.sign({n = 1.5, m = 1e16, big = 9007199254740991}, 'k'), 'k')
assert(claims.n == 1.5 and claims.m == 1e16 and claims.big == 9007199254740991)
local _, err = pcall(jwt.sign, {n = 0 / 0}, 'k')
assert(err and err:find('non-finite', 1, true), tostring(err))
_, err = pcall(jwt.sign, {n = math.huge}, 'k')
assert(err)
_, err = pcall(jwt.sign, {n = -math.huge}, 'k')
assert(err)
_, err = pcall(jwt.sign, {f = print}, 'k')
assert(err and err:find('cannot encode function', 1, true), tostring(err))
)lua"));
}

// json_decode escape paths, driven with attacker-shaped payloads whose
// signatures are recomputed through shield.crypto (so verification gets past
// the signature gate and exercises the parser): simple escapes, \uXXXX in
// all four UTF-8 width classes including surrogate pairs, empty containers,
// literals, and whitespace tolerance.
BOOST_AUTO_TEST_CASE(JwtJsonDecodeEscapes) {
    CryptoState s;
    BOOST_CHECK(run_script(s.lua, R"lua(
local function forge(payload)
    local h = shield.crypto.base64url_encode('{"alg":"HS256","typ":"JWT"}')
    local p = shield.crypto.base64url_encode(payload)
    local si = h .. '.' .. p
    return si .. '.' ..
        shield.crypto.base64url_encode(shield.crypto.hmac_sha256('k', si))
end
-- simple escapes: \" \\ \/ \b \f \n \r \t
local claims = jwt.verify(forge('{"s":"a\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti"}'), 'k')
assert(claims.s == 'a"b\\c/d' ..
    string.char(8) .. 'e' .. string.char(12) .. 'f' .. string.char(10) ..
    'g' .. string.char(13) .. 'h' .. string.char(9) .. 'i',
    tostring(claims and claims.s))
-- \uXXXX widths: 1-byte (A), 2-byte (é), 3-byte (中), 4-byte via pair (😀)
claims = jwt.verify(forge('{"w":"\\u0041\\u00e9\\u4e2d\\ud83d\\ude00"}'), 'k')
assert(claims.w == 'A' ..
    string.char(0xC3, 0xA9) .. string.char(0xE4, 0xB8, 0xAD) ..
    string.char(0xF0, 0x9F, 0x98, 0x80),
    tostring(claims and claims.w))
-- literals, empty containers, whitespace forms.
claims = jwt.verify(forge(
    '{ "flag" : true , "off" : false , "gone" : null , "o" : { } ,' ..
    ' "a" : [ ] , "n" : 7 }'), 'k')
assert(claims.flag == true and claims.off == false and claims.gone == nil)
assert(next(claims.o) == nil and #claims.a == 0 and claims.n == 7)
)lua"));
}

// json_decode malformed arms, again with valid signatures so the failures
// come from the parser: bad escape characters, truncated and non-hex \u
// escapes, unterminated strings, trailing content, bad numbers, and a
// payload that decodes to a non-table JSON value.
BOOST_AUTO_TEST_CASE(JwtJsonDecodeMalformed) {
    CryptoState s;
    BOOST_CHECK(run_script(s.lua, R"lua(
local function forge(payload)
    local h = shield.crypto.base64url_encode('{"alg":"HS256","typ":"JWT"}')
    local p = shield.crypto.base64url_encode(payload)
    local si = h .. '.' .. p
    return si .. '.' ..
        shield.crypto.base64url_encode(shield.crypto.hmac_sha256('k', si))
end
local function malformed(payload)
    local _, code, msg = jwt.verify(forge(payload), 'k')
    assert(code == 'malformed',
        payload .. ' -> ' .. tostring(code) .. ' ' .. tostring(msg))
    return msg
end
malformed('{"s":"\\q"}')          -- unknown escape letter
malformed('{"s":"\\u0"}')         -- truncated \\u escape
malformed('{"s":"\\u00Z1"}')      -- non-hex \\u escape
malformed('{"s":"abc')            -- unterminated string
malformed('{} trailing')          -- content after the top-level value
malformed('{"n":-}')              -- number with no digits
local msg = malformed('999')      -- decodes to a number, not a table
assert(msg:find('payload', 1, true), msg)
)lua"));
}

// verify() input guards the earlier matrix does not touch (empty / missing
// key, typed-wrong exp/nbf with valid signatures) and the sign() header
// override path (custom header keeps working tokens: alg is defaulted back
// to HS256 so verify's pin still accepts it).
BOOST_AUTO_TEST_CASE(JwtVerifyKeyAndTypedClaimArms) {
    CryptoState s;
    BOOST_CHECK(run_script(s.lua, R"lua(
local function forge(payload)
    local h = shield.crypto.base64url_encode('{"alg":"HS256","typ":"JWT"}')
    local p = shield.crypto.base64url_encode(payload)
    local si = h .. '.' .. p
    return si .. '.' ..
        shield.crypto.base64url_encode(shield.crypto.hmac_sha256('k', si))
end
local token = jwt.sign({sub = 'p1'}, 'k')
local _, code = jwt.verify(token, '')
assert(code == 'malformed', tostring(code))
_, code = jwt.verify(token)
assert(code == 'malformed', tostring(code))
_, code = jwt.verify(forge('{"exp":"9"}'), 'k')
assert(code == 'malformed', tostring(code))
_, code = jwt.verify(forge('{"nbf":"x"}'), 'k')
assert(code == 'malformed', tostring(code))
-- sign() opts.header override: typ/kid pass through, alg defaults back.
local tok2 = jwt.sign({sub = 'p2'}, 'k',
                      {header = {typ = 'JWT+X', kid = 'k1'}})
local hdr = shield.crypto.base64url_decode(tok2:match('^(.-)%.'))
assert(hdr:find('"alg":"HS256"', 1, true), hdr)
assert(hdr:find('"kid":"k1"', 1, true), hdr)
assert(jwt.verify(tok2, 'k').sub == 'p2')
)lua"));
}

// Claims whose keys are not consecutive integers degrade to a JSON object --
// and the values must survive that degradation. Regression pin: the object
// encoder used to collect tostring(k) and then look the value up with the
// string form, so a numeric key (a sparse array such as {[1]='a',[3]='c'})
// resolved to nil and the claim was silently emitted as null. Also pins the
// total ordering for keys that share a string form (1 and "1" coexist in one
// Lua table) and sign()'s argument guards.
BOOST_AUTO_TEST_CASE(JwtNonArrayObjectKeysAndSignGuards) {
    CryptoState s;
    BOOST_CHECK(run_script(s.lua, R"lua(
local function payload_of(v)
    local tok = jwt.sign(v, 'k')
    return shield.crypto.base64url_decode(tok:match('^.-%.(.-)%.'))
end
-- Sparse numeric keys: object fallback, values intact.
assert(payload_of({[1] = 'a', [3] = 'c'}) == '{"1":"a","3":"c"}',
    payload_of({[1] = 'a', [3] = 'c'}))
assert(payload_of({[2] = 'b'}) == '{"2":"b"}')
assert(payload_of({[7] = true}) == '{"7":true}')
local claims = jwt.verify(jwt.sign({[1] = 'a', [3] = 'c'}, 'k'), 'k')
assert(claims and claims['1'] == 'a' and claims['3'] == 'c', tostring(claims))
-- Two distinct keys with the same string form: both emitted, order stable.
local collide = {[1] = 'num', ['1'] = 'str'}
assert(payload_of(collide) == '{"1":"num","1":"str"}', payload_of(collide))
assert(jwt.sign(collide, 'k') == jwt.sign(collide, 'k'), 'not deterministic')
-- Plain string claims keep the documented sorted order.
assert(payload_of({b = 2, a = 1, C = 3}) == '{"C":3,"a":1,"b":2}')
-- Nested objects degrade the same way.
assert(payload_of({meta = {[2] = 'x', name = 'n'}}) ==
    '{"meta":{"2":"x","name":"n"}}')
-- sign() input guards.
local ok, err = pcall(jwt.sign, 'not-a-table', 'k')
assert(not ok and err:find('claims must be a table', 1, true), tostring(err))
ok, err = pcall(jwt.sign, {}, '')
assert(not ok and err:find('key must be a non', 1, true), tostring(err))
ok, err = pcall(jwt.sign, {}, 42)
assert(not ok and err:find('key must be a non', 1, true), tostring(err))
)lua"));
}

// The production registration path (register_full_shield_api) must expose the
// same shield.crypto table — this covers the call site in lua_api.cpp.
BOOST_AUTO_TEST_CASE(RegisteredViaFullShieldApi) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::string, shd::lib::math,
                       shd::lib::table, shd::lib::os, shd::lib::coroutine);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);
    auto result = lua.script(
        "assert(type(shield.crypto) == 'table')\n"
        "assert(shield.crypto.hex_encode('a') == '61')\n"
        "assert(#shield.crypto.sha256('x') == 32)\n");
    if (!result.valid()) {
        const shd::error e = result.get_error();
        std::fprintf(stderr, "lua error: %s\n", e.what());
    }
    BOOST_CHECK(result.valid());
}

// ---------------------------------------------------------------------------
// crypto phase 2: key derivation.
// PBKDF2-HMAC-SHA256 vectors are the canonical SHA-256 set (cross-checked
// against RFC 8018 reference implementations, same vectors as the RFC 7914
// §11 appendices). HKDF vectors are RFC 5869 test cases 1-3 verbatim.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Pbkdf2HmacSha256RfcVectors) {
    CryptoState s;
    // P="password" S="salt" dkLen=32, iterations 1 / 2 / 4096.
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.hex_encode("
                           "shield.crypto.pbkdf2_hmac_sha256('password', "
                           "'salt', 1, 32)) == "
                           "'120fb6cffcf8b32c43e7225256c4f837a86548c92ccc354"
                           "80805987cb70be17b')"));
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.hex_encode("
                           "shield.crypto.pbkdf2_hmac_sha256('password', "
                           "'salt', 2, 32)) == "
                           "'ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251d"
                           "fd6e2d85a95474c43')"));
    BOOST_CHECK(run_script(s.lua,
                           "assert(shield.crypto.hex_encode("
                           "shield.crypto.pbkdf2_hmac_sha256('password', "
                           "'salt', 4096, 32)) == "
                           "'c5e478d59288c841aa530db6845c4c8d962893a001ce4e1"
                           "1a4963873aa98134a')"));
    // Multi-block output (dkLen=40) with a longer password and salt.
    BOOST_CHECK(run_script(
        s.lua,
        "assert(shield.crypto.hex_encode(shield.crypto.pbkdf2_hmac_sha256("
        "'passwordPASSWORDpassword', "
        "'saltSALTsaltSALTsaltSALTsaltSALTsalt', 4096, 40)) == "
        "'348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c"
        "635518c7dac47e9')"));
}

BOOST_AUTO_TEST_CASE(Pbkdf2HmacSha256GuardArms) {
    CryptoState s;
    // Every argument guard is a real, reachable API surface.
    const char* arms[][2] = {
        // {code, expected error substring}
        {"pcall(shield.crypto.pbkdf2_hmac_sha256, 'p', 's', 0, 32)",
         "iterations must be >= 1"},
        {"pcall(shield.crypto.pbkdf2_hmac_sha256, 'p', 's', -1, 32)",
         "iterations must be >= 1"},
        {"pcall(shield.crypto.pbkdf2_hmac_sha256, 'p', 's', 10000001, 32)",
         "iterations exceeds 10000000 limit"},
        {"pcall(shield.crypto.pbkdf2_hmac_sha256, 'p', '', 1, 32)",
         "salt must not be empty"},
        {"pcall(shield.crypto.pbkdf2_hmac_sha256, 'p', 's', 1, 0)",
         "derived key length must be >= 1"},
        {"pcall(shield.crypto.pbkdf2_hmac_sha256, 'p', 's', 1, 1025)",
         "derived key length exceeds 1024 limit"},
    };
    for (const auto& arm : arms) {
        std::string code = std::string("local ok, err = ") + arm[0] +
                           "\nassert(not ok and err:find('" + arm[1] +
                           "', 1, true), tostring(err))";
        BOOST_CHECK_MESSAGE(run_script(s.lua, code), arm[1]);
    }
}

// Empty password is valid PBKDF2 input; cross-check against an independent
// C++-side OpenSSL call (same pattern as cpp_hmac_sha256 above).
BOOST_AUTO_TEST_CASE(Pbkdf2HmacSha256EmptyPasswordCrossCheck) {
    CryptoState s;
    unsigned char dk[32];
    BOOST_REQUIRE_EQUAL(
        PKCS5_PBKDF2_HMAC("", 0, reinterpret_cast<const unsigned char*>("s"), 1,
                          1, EVP_sha256(), sizeof(dk), dk),
        1);
    std::string expect = hex_from_cpp(
        std::string(reinterpret_cast<const char*>(dk), sizeof(dk)));
    BOOST_CHECK(run_script(
        s.lua,
        "assert(shield.crypto.hex_encode(shield.crypto.pbkdf2_hmac_sha256("
        "'', 's', 1, 32)) == '" +
            expect + "')"));
}

BOOST_AUTO_TEST_CASE(HkdfSha256Rfc5869Vectors) {
    CryptoState s;
    // Test case 1: IKM=0x0b x22, salt=0x00..0x0c, info=0xf0..0xf9, L=42.
    BOOST_CHECK(run_script(
        s.lua,
        "local ikm = string.rep('\\11', 22)\n"
        "local salt = '\\0\\1\\2\\3\\4\\5\\6\\7\\8\\9\\10\\11\\12'\n"
        "local info = '\\240\\241\\242\\243\\244\\245\\246\\247\\248\\249'\n"
        "assert(shield.crypto.hex_encode(shield.crypto.hkdf_sha256(ikm, "
        "salt, info, 42)) == "
        "'3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34"
        "007208d5b887185865')"));
    // Test case 2: 80-byte ranges, L=82 (exercises multi-block expand).
    BOOST_CHECK(run_script(
        s.lua,
        "local function range(a, b) "
        "local t = {} for i = a, b do t[#t + 1] = string.char(i) end "
        "return table.concat(t) end\n"
        "assert(shield.crypto.hex_encode(shield.crypto.hkdf_sha256("
        "range(0, 79), range(96, 175), range(176, 255), 82)) == "
        "'b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59"
        "045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c"
        "58179ec3e87c14c01d5c1f3434f1d87')"));
    // Test case 3: empty salt and empty info (zero-form salt path).
    BOOST_CHECK(run_script(
        s.lua,
        "local ikm = string.rep('\\11', 22)\n"
        "assert(shield.crypto.hex_encode(shield.crypto.hkdf_sha256(ikm, '', "
        "'', 42)) == "
        "'8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d"
        "201395faa4b61a96c8')"));
    // Non-empty salt with empty info (skips the add1 call).
    BOOST_CHECK(run_script(
        s.lua,
        "local ikm = string.rep('\\11', 22)\n"
        "assert(#shield.crypto.hkdf_sha256(ikm, 'salt', '', 32) == 32)"));
}

BOOST_AUTO_TEST_CASE(HkdfSha256GuardArms) {
    CryptoState s;
    const char* arms[][2] = {
        {"pcall(shield.crypto.hkdf_sha256, '', 's', '', 32)",
         "ikm must not be empty"},
        {"pcall(shield.crypto.hkdf_sha256, 'ikm', 's', '', 0)",
         "output length must be >= 1"},
        {"pcall(shield.crypto.hkdf_sha256, 'ikm', 's', '', 8161)",
         "exceeds RFC 5869 8160-byte limit"},
    };
    for (const auto& arm : arms) {
        std::string code = std::string("local ok, err = ") + arm[0] +
                           "\nassert(not ok and err:find('" + arm[1] +
                           "', 1, true), tostring(err))";
        BOOST_CHECK_MESSAGE(run_script(s.lua, code), arm[1]);
    }
}

// ---------------------------------------------------------------------------
// crypto phase 2: AES-256-GCM AEAD.
// Known-answer vectors are NIST GCM test cases (AES-256, 96-bit IV) and the
// canonical "TLS-style" AES-256-GCM example; both are cross-checked here as
// ciphertext‖tag hex.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(AeadAes256GcmKnownVectors) {
    CryptoState s;
    // NIST GCM test case 13 (AES-256, K=0x00*32, IV=0x00*12, P empty,
    // A empty): tag only.
    BOOST_CHECK(
        run_script(s.lua,
                   "local k = string.rep('\\0', 32)\n"
                   "local n = string.rep('\\0', 12)\n"
                   "assert(shield.crypto.hex_encode("
                   "shield.crypto.aead_aes256gcm_encrypt(k, n, '', '')) == "
                   "'530f8afbc74536b9a963b4f1c4cb738b')"));
    // NIST GCM test case 14 (AES-256, zero K/IV, P = 0x00*16, A empty).
    BOOST_CHECK(run_script(
        s.lua,
        "local k = string.rep('\\0', 32)\n"
        "local n = string.rep('\\0', 12)\n"
        "assert(shield.crypto.hex_encode("
        "shield.crypto.aead_aes256gcm_encrypt(k, n, string.rep('\\0', 16), "
        "'')) == "
        "'cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab9"
        "19')"));
    // Canonical AES-256-GCM reference (non-zero key/IV, 60-byte P, 20-byte
    // AAD). Output is ciphertext(60) || tag(16).
    BOOST_CHECK(run_script(
        s.lua,
        "local k = shield.crypto.hex_decode("
        "'feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308')\n"
        "local n = shield.crypto.hex_decode('cafebabefacedbaddecaf888')\n"
        "local aad = shield.crypto.hex_decode('feedfacedeadbeeffeedfacedead"
        "beefabaddad2')\n"
        "local p = shield.crypto.hex_decode("
        "'d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
        "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39')\n"
        "assert(shield.crypto.hex_encode(shield.crypto.aead_aes256gcm_encrypt("
        "k, n, p, aad)) == "
        "'522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8c"
        "b08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f66276fc6ece0f4"
        "e1768cddf8853bb2d551b')"));
    // Round trip incl. the AAD binding (decrypt with the same AAD).
    BOOST_CHECK(run_script(
        s.lua,
        "local k = shield.crypto.random_bytes(32)\n"
        "local n = shield.crypto.random_bytes(12)\n"
        "local p = 'attack at dawn'\n"
        "local aad = 'header'\n"
        "local c = shield.crypto.aead_aes256gcm_encrypt(k, n, p, aad)\n"
        "assert(shield.crypto.aead_aes256gcm_decrypt(k, n, c, aad) == p)"));
}

BOOST_AUTO_TEST_CASE(AeadAes256GcmRejectsForgery) {
    CryptoState s;
    // Tampered tag, tampered ciphertext, wrong AAD, wrong nonce, wrong key
    // must all raise — never return plaintext.
    BOOST_CHECK(run_script(
        s.lua,
        "local k = string.rep('\\7', 32)\n"
        "local n = shield.crypto.random_bytes(12)\n"
        "local c = shield.crypto.aead_aes256gcm_encrypt(k, n, 'secret', "
        "'aad')\n"
        "local function raises(f) local ok = pcall(f) return not ok end\n"
        "assert(raises(function() "
        "shield.crypto.aead_aes256gcm_decrypt(k, n, c:sub(1, -2) .. "
        "string.char((c:byte(-1) + 1) % 256), 'aad') end))\n"
        "local bad = c:sub(1, 1) ~= '\\0' and "
        "('\\0' .. c:sub(2)) or ('\\1' .. c:sub(2))\n"
        "assert(raises(function() "
        "shield.crypto.aead_aes256gcm_decrypt(k, n, bad, 'aad') end))\n"
        "assert(raises(function() "
        "shield.crypto.aead_aes256gcm_decrypt(k, n, c, 'other') end))\n"
        "assert(raises(function() "
        "shield.crypto.aead_aes256gcm_decrypt(k, "
        "shield.crypto.random_bytes(12), c, 'aad') end))\n"
        "assert(raises(function() "
        "shield.crypto.aead_aes256gcm_decrypt(string.rep('\\9', 32), n, c, "
        "'aad') end))"));
}

BOOST_AUTO_TEST_CASE(AeadAes256GcmGuardArms) {
    CryptoState s;
    const char* arms[][2] = {
        {"pcall(shield.crypto.aead_aes256gcm_encrypt, string.rep('k', 31), "
         "string.rep('n', 12), 'p', '')",
         "key must be 32 bytes"},
        {"pcall(shield.crypto.aead_aes256gcm_encrypt, string.rep('k', 32), "
         "string.rep('n', 11), 'p', '')",
         "nonce must be 12 bytes"},
        {"pcall(shield.crypto.aead_aes256gcm_decrypt, string.rep('k', 33), "
         "string.rep('n', 12), string.rep('x', 16), '')",
         "key must be 32 bytes"},
        {"pcall(shield.crypto.aead_aes256gcm_decrypt, string.rep('k', 32), "
         "string.rep('n', 13), string.rep('x', 16), '')",
         "nonce must be 12 bytes"},
        {"pcall(shield.crypto.aead_aes256gcm_decrypt, string.rep('k', 32), "
         "string.rep('n', 12), string.rep('x', 15), '')",
         "shorter than the 16-byte tag"},
    };
    for (const auto& arm : arms) {
        std::string code = std::string("local ok, err = ") + arm[0] +
                           "\nassert(not ok and err:find('" + arm[1] +
                           "', 1, true), tostring(err))";
        BOOST_CHECK_MESSAGE(run_script(s.lua, code), arm[1]);
    }
    // Empty AAD is valid and round-trips.
    BOOST_CHECK(run_script(
        s.lua,
        "local k = shield.crypto.random_bytes(32)\n"
        "local n = shield.crypto.random_bytes(12)\n"
        "assert(shield.crypto.aead_aes256gcm_decrypt(k, n, "
        "shield.crypto.aead_aes256gcm_encrypt(k, n, 'x', ''), '') == 'x')"));
}

// ---------------------------------------------------------------------------
// Ed25519 (RFC 8032 §7.2/§7.3): deterministic signatures over raw keys.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Ed25519Rfc8032Vectors) {
    CryptoState s;
    // §7.2 TEST 1 (empty message): the seed derives the known public key,
    // signing produces the exact 64-byte RFC signature, verify accepts it.
    BOOST_CHECK(run_script(
        s.lua,
        "local seed = shield.crypto.hex_decode("
        "'9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')\n"
        "assert(shield.crypto.hex_encode("
        "shield.crypto.ed25519_public_key(seed)) == "
        "'d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')\n"
        "local sig = shield.crypto.ed25519_sign(seed, '')\n"
        "assert(#sig == 64)\n"
        "assert(shield.crypto.hex_encode(sig) == "
        "'e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b')\n"
        "assert(shield.crypto.ed25519_verify("
        "shield.crypto.hex_decode("
        "'d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a'), "
        "'', sig))"));
    // §7.3 EXAMPLE (the SHA("abc") message, af82).
    BOOST_CHECK(run_script(
        s.lua,
        "local seed = shield.crypto.hex_decode("
        "'c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7')\n"
        "local pub = shield.crypto.hex_decode("
        "'fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025')\n"
        "local msg = shield.crypto.hex_decode('af82')\n"
        "local sig = shield.crypto.ed25519_sign(seed, msg)\n"
        "assert(shield.crypto.hex_encode(sig) == "
        "'6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
        "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a')\n"
        "assert(shield.crypto.ed25519_verify(pub, msg, sig))"));
}

BOOST_AUTO_TEST_CASE(Ed25519SignVerifyRoundTrip) {
    CryptoState s;
    // Fresh keypair, non-trivial message: sign → verify accepts; any change
    // to the message or signature is rejected with false (never an error).
    BOOST_CHECK(run_script(
        s.lua,
        "local seed = shield.crypto.random_bytes(32)\n"
        "local pub = shield.crypto.ed25519_public_key(seed)\n"
        "assert(#pub == 32)\n"
        "local msg = 'withdraw 100 credits to acct-771'\n"
        "local sig = shield.crypto.ed25519_sign(seed, msg)\n"
        "assert(#sig == 64)\n"
        "assert(shield.crypto.ed25519_verify(pub, msg, sig))\n"
        "assert(not shield.crypto.ed25519_verify(pub, msg .. 'x', sig))\n"
        "local bad = sig:sub(1, 1) ~= '\\0' and "
        "('\\0' .. sig:sub(2)) or ('\\1' .. sig:sub(2))\n"
        "assert(not shield.crypto.ed25519_verify(pub, msg, bad))\n"
        "assert(not shield.crypto.ed25519_verify(pub, '', sig))\n"
        "local other = shield.crypto.ed25519_public_key("
        "shield.crypto.random_bytes(32))\n"
        "assert(not shield.crypto.ed25519_verify(other, msg, sig))"));
}

BOOST_AUTO_TEST_CASE(Ed25519SignatureLengthArms) {
    CryptoState s;
    // Malformed signature lengths answer false on the non-throwing path.
    BOOST_CHECK(run_script(
        s.lua,
        "local seed = shield.crypto.random_bytes(32)\n"
        "local pub = shield.crypto.ed25519_public_key(seed)\n"
        "local sig = shield.crypto.ed25519_sign(seed, 'm')\n"
        "assert(not shield.crypto.ed25519_verify(pub, 'm', sig:sub(1, 63)))\n"
        "assert(not shield.crypto.ed25519_verify(pub, 'm', sig .. 'x'))\n"
        "assert(not shield.crypto.ed25519_verify(pub, 'm', ''))"));
}

BOOST_AUTO_TEST_CASE(Ed25519GuardArms) {
    CryptoState s;
    const char* arms[][2] = {
        {"pcall(shield.crypto.ed25519_public_key, string.rep('k', 31))",
         "secret key must be 32 bytes"},
        {"pcall(shield.crypto.ed25519_public_key, string.rep('k', 33))",
         "secret key must be 32 bytes"},
        {"pcall(shield.crypto.ed25519_sign, string.rep('k', 31), 'm')",
         "secret key must be 32 bytes"},
        {"pcall(shield.crypto.ed25519_sign, string.rep('k', 33), 'm')",
         "secret key must be 32 bytes"},
        {"pcall(shield.crypto.ed25519_verify, string.rep('p', 31), 'm', "
         "string.rep('s', 64))",
         "public key must be 32 bytes"},
        {"pcall(shield.crypto.ed25519_verify, string.rep('p', 33), 'm', "
         "string.rep('s', 64))",
         "public key must be 32 bytes"},
    };
    for (const auto& arm : arms) {
        std::string code = std::string("local ok, err = ") + arm[0] +
                           "\nassert(not ok and err:find('" + arm[1] +
                           "', 1, true), tostring(err))";
        BOOST_CHECK_MESSAGE(run_script(s.lua, code), arm[1]);
    }
}

// ---------------------------------------------------------------------------
// EdDSA JWT (RFC 8037): alg=Ed25519 sign/verify on top of the Ed25519
// primitives, with kid support and JWKS key sets.
// ---------------------------------------------------------------------------

// RFC 8037 A.1: the JWK key material and the signature over the A.1 message
// are pinned byte-for-byte. The A.1 private key is the RFC 8032 §7.2 TEST 1
// seed, so decoding d and x must reproduce that seed and its public key —
// this cross-locks the JWKS x value against the already-pinned RFC 8032
// vector and guards against a mis-recorded base64url constant.
BOOST_AUTO_TEST_CASE(JwtEdDSARfc8037A1Vector) {
    CryptoState s;
    BOOST_CHECK(run_script(
        s.lua,
        "local d = shield.crypto.base64url_decode("
        "'nWGxne_9WmC6hEr0kuwsxERJxWl7MmkZcDusAxyuf2A')\n"
        "assert(shield.crypto.hex_encode(d) == "
        "'9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')\n"
        "local x = shield.crypto.base64url_decode("
        "'11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo')\n"
        "assert(shield.crypto.hex_encode(x) == "
        "'d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')"
        "\n"));
    // A.1 signature over "Example of Ed25519 Signing".
    BOOST_CHECK(run_script(
        s.lua,
        "local seed = shield.crypto.hex_decode("
        "'9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')\n"
        "local sig = shield.crypto.ed25519_sign(seed, 'Example of Ed25519 "
        "Signing')\n"
        "assert(shield.crypto.hex_encode(sig) == "
        "'98b9f04b732d7a2c33ca66150520f285832f714742030f7df358924d3b28b8ccb"
        "6b4f227c7ed3ef56a8eac91a02b990827ae33f0c9f956ab62b7c38ac3736205')\n"));
}

// EdDSA sign -> verify round trip with a kid pin, plus a cross-language check:
// the signature segment must equal Ed25519(seed, "header.payload") computed
// here with OpenSSL directly, base64url-encoded (unpadded).
BOOST_AUTO_TEST_CASE(JwtEdDSASignVerifyRoundTrip) {
    CryptoState s;
    const bool ok = run_script(
        s.lua,
        "local seed = shield.crypto.hex_decode("
        "'9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')\n"
        "local pub = shield.crypto.ed25519_public_key(seed)\n"
        "token = jwt.sign({sub = 'p1', iss = 'gate', exp = 100}, seed,\n"
        "                 {alg = 'EdDSA', kid = 'k1'})\n"
        "local h, p, sig = token:match('([^%.]+)%.([^%.]+)%.([^%.]+)')\n"
        "assert(h and p and sig)\n"
        "assert(shield.crypto.base64url_decode(h):find('EdDSA', 1, true))\n"
        "assert(shield.crypto.base64url_decode(h):find('k1', 1, true))\n"
        "local claims, code, msg = jwt.verify(token, pub, {alg = 'EdDSA', "
        "kid = 'k1', now = 99})\n"
        "assert(claims and claims.sub == 'p1' and claims.iss == 'gate', "
        "tostring(code) .. ' ' .. tostring(msg))\n"
        "claims = jwt.verify(token, pub, {alg = 'EdDSA', now = 99})\n"
        "assert(claims and claims.sub == 'p1')\n");
    BOOST_CHECK(ok);

    // Cross-check the signature bytes from C++.
    const std::string seed = bytes_from_hex(
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    const shd::object tok = s.lua["token"];
    const std::string token = tok.as<std::string>();
    const size_t first = token.find('.');
    const size_t second = token.find('.', first + 1);
    BOOST_REQUIRE(first != std::string::npos && second != std::string::npos);
    const std::string signing_input = token.substr(0, second);
    const std::string sig = token.substr(second + 1);
    const shd::function b64url = s.lua["shield"]["crypto"]["base64url_encode"];
    const std::string expected = b64url(cpp_ed25519_sign(seed, signing_input));
    BOOST_CHECK_EQUAL(sig, expected);
}

// EdDSA rejection matrix: tampered payload, wrong key, short signature,
// undecodable signature segment, algorithm confusion (both directions plus
// alg=none), kid mismatch, wrong-size public key, and the claims arms.
BOOST_AUTO_TEST_CASE(JwtEdDSARejections) {
    CryptoState s;
    BOOST_CHECK(run_script(s.lua, R"lua(
local seed = shield.crypto.random_bytes(32)
local pub = shield.crypto.ed25519_public_key(seed)
local token = jwt.sign({sub = 'p1', exp = os.time() + 600}, seed, {alg = 'EdDSA'})
local h, p, sig = token:match('([^%.]+)%.([^%.]+)%.([^%.]+)')
-- Tampered payload -> bad_signature.
local forged = h .. '.' ..
    shield.crypto.base64url_encode('{\"sub\":\"p2\"}') .. '.' .. sig
local claims, code = jwt.verify(forged, pub, {alg = 'EdDSA'})
assert(not claims and code == 'bad_signature', tostring(code))
-- Wrong key -> bad_signature.
local other = shield.crypto.ed25519_public_key(shield.crypto.random_bytes(32))
claims, code = jwt.verify(token, other, {alg = 'EdDSA'})
assert(not claims and code == 'bad_signature', tostring(code))
-- 63-byte signature -> bad_signature (ed25519_verify answers false).
local raw = shield.crypto.base64url_decode(sig)
claims, code = jwt.verify(h .. '.' .. p .. '.' ..
    shield.crypto.base64url_encode(raw:sub(1, 63)), pub, {alg = 'EdDSA'})
assert(not claims and code == 'bad_signature', tostring(code))
-- Undecodable signature segment -> malformed.
claims, code = jwt.verify(h .. '.' .. p .. '.!!!', pub, {alg = 'EdDSA'})
assert(not claims and code == 'malformed', tostring(code))
-- Algorithm confusion: HS256 token verified as EdDSA -> unsupported_alg.
local hs = jwt.sign({sub = 'p1'}, 'k')
claims, code = jwt.verify(hs, pub, {alg = 'EdDSA'})
assert(not claims and code == 'unsupported_alg', tostring(code))
-- EdDSA token verified as HS256 (the default) -> unsupported_alg.
claims, code = jwt.verify(token, 'k')
assert(not claims and code == 'unsupported_alg', tostring(code))
-- alg:none with EdDSA -> unsupported_alg.
local none = shield.crypto.base64url_encode('{\"alg\":\"none\",\"typ\":\"JWT\"}') .. '.' ..
    shield.crypto.base64url_encode('{\"sub\":\"p1\"}') .. '.AA'
claims, code = jwt.verify(none, pub, {alg = 'EdDSA'})
assert(not claims and code == 'unsupported_alg', tostring(code))
-- kid mismatch -> bad_kid.
local kidded = jwt.sign({sub = 'p1'}, seed, {alg = 'EdDSA', kid = 'k1'})
claims, code = jwt.verify(kidded, pub, {alg = 'EdDSA', kid = 'k2'})
assert(not claims and code == 'bad_kid', tostring(code))
-- Wrong-size public key -> malformed.
claims, code = jwt.verify(token, string.rep('p', 31), {alg = 'EdDSA'})
assert(not claims and code == 'malformed', tostring(code))
-- Expired / bad issuer on an EdDSA token.
local exp = jwt.sign({sub = 'p1', exp = 1000}, seed, {alg = 'EdDSA'})
claims, code = jwt.verify(exp, pub, {alg = 'EdDSA', now = 1001})
assert(not claims and code == 'expired', tostring(code))
local iss = jwt.sign({sub = 'p1', iss = 'gate'}, seed, {alg = 'EdDSA'})
claims, code = jwt.verify(iss, pub, {alg = 'EdDSA', issuer = 'other'})
assert(not claims and code == 'bad_issuer', tostring(code))
)lua"));
}

// EdDSA key-size guards on both sign (seed) and verify (public key).
BOOST_AUTO_TEST_CASE(JwtEdDSAKeyGuards) {
    CryptoState s;
    const char* arms[][2] = {
        {"pcall(jwt.sign, {sub='p1'}, string.rep('s',31), {alg='EdDSA'})",
         "32-byte"},
        {"pcall(jwt.sign, {sub='p1'}, string.rep('s',33), {alg='EdDSA'})",
         "32-byte"},
    };
    for (const auto& arm : arms) {
        std::string code = std::string("local ok, err = ") + arm[0] +
                           "\nassert(not ok and err:find('" + arm[1] +
                           "', 1, true), tostring(err))";
        BOOST_CHECK_MESSAGE(run_script(s.lua, code), arm[1]);
    }
    BOOST_CHECK(run_script(
        s.lua,
        "local token = jwt.sign({sub = 'p1'}, string.rep('s', 32), {alg = "
        "'EdDSA'})\n"
        "local _, code = jwt.verify(token, string.rep('p', 31), {alg = "
        "'EdDSA'})\n"
        "assert(code == 'malformed', tostring(code))\n"
        "_, code = jwt.verify(token, string.rep('p', 33), {alg = 'EdDSA'})\n"
        "assert(code == 'malformed', tostring(code))\n"));
}

// HS256 kid round trip: the pin works and a mismatch is rejected.
BOOST_AUTO_TEST_CASE(JwtHs256KidRoundTrip) {
    CryptoState s;
    BOOST_CHECK(
        run_script(s.lua,
                   "local token = jwt.sign({sub = 'p1'}, 'k', {kid = 'k1'})\n"
                   "local claims = jwt.verify(token, 'k', {kid = 'k1'})\n"
                   "assert(claims and claims.sub == 'p1')\n"
                   "local _, code = jwt.verify(token, 'k', {kid = 'k2'})\n"
                   "assert(code == 'bad_kid', tostring(code))\n"));
}

// JWKS build from the A.1 seed: the document must carry the A.1 x value and
// the OKP/Ed25519/kid fields, and parsing it back must return the A.1 public
// key under the same kid.
BOOST_AUTO_TEST_CASE(JwksRfc8037A1Vector) {
    CryptoState s;
    BOOST_CHECK(run_script(
        s.lua,
        "local seed = shield.crypto.hex_decode("
        "'9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')\n"
        "local doc = jwt.jwks_build({{kid = 'k1', seed = seed}})\n"
        "assert(doc:find('\"x\":\"11qYAYKxCrfVS_"
        "7TyWQHOg7hcvPapiMlrwIaaPcHURo\"', 1, true), doc)\n"
        "assert(doc:find('\"kty\":\"OKP\"', 1, true), doc)\n"
        "assert(doc:find('\"crv\":\"Ed25519\"', 1, true), doc)\n"
        "assert(doc:find('\"kid\":\"k1\"', 1, true), doc)\n"
        "local keys = jwt.jwks_parse(doc)\n"
        "assert(#keys == 1)\n"
        "assert(keys[1].kid == 'k1')\n"
        "assert(shield.crypto.hex_encode(keys[1].public_key) == "
        "'d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')"
        "\n"));
}

// JWKS parse errors and the skip matrix: non-JSON, missing/non-array keys,
// non-object entries, missing/undecodable/wrong-size x all reject as
// "malformed"; non-OKP and non-Ed25519 entries are skipped; an empty keys
// array and a table input parse fine.
BOOST_AUTO_TEST_CASE(JwksParseErrors) {
    CryptoState s;
    BOOST_CHECK(run_script(s.lua, R"lua(
local function rej(doc, what)
    local _, code, msg = jwt.jwks_parse(doc)
    assert(not _ and code == 'malformed', what .. ' -> ' .. tostring(code))
    return msg
end
rej('not json', 'not json')
rej('{"keys": 42}', 'keys not array')
rej('{"keys": [42]}', 'entry not object')
rej('{"keys": [{"kty":"OKP","crv":"Ed25519"}]}', 'missing x')
rej('{"keys": [{"kty":"OKP","crv":"Ed25519","x":"!!!"}]}', 'undecodable x')
rej('{"keys": [{"kty":"OKP","crv":"Ed25519","x":"' .. string.rep('A', 44) .. '"}]}', 'x not 32 bytes')
-- Non-Ed25519 entries are skipped, not errors.
local keys = jwt.jwks_parse('{"keys": [{"kty":"RSA","n":"x","e":"AQAB"}]}')
assert(#keys == 0)
keys = jwt.jwks_parse('{"keys": [{"kty":"OKP","crv":"X25519","x":"' .. string.rep('A', 44) .. '"}]}')
assert(#keys == 0)
-- Empty keys array parses to an empty result.
keys = jwt.jwks_parse('{"keys": []}')
assert(#keys == 0)
-- A table input (already-decoded document) works too.
local seed = shield.crypto.hex_decode('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')
keys = jwt.jwks_parse({keys = {{kty = "OKP", crv = "Ed25519",
    x = shield.crypto.base64url_encode(shield.crypto.ed25519_public_key(seed)),
    kid = 'k9'}}})
assert(#keys == 1 and keys[1].kid == 'k9')
-- A missing kid comes back as nil, not an error.
keys = jwt.jwks_parse('{"keys": [{"kty":"OKP","crv":"Ed25519","x":"' ..
    shield.crypto.base64url_encode(shield.crypto.ed25519_public_key(seed)) .. '"}]}')
assert(#keys == 1 and keys[1].kid == nil)
)lua"));
}

// jwks_build argument guards: every assert is a real, reachable API surface.
BOOST_AUTO_TEST_CASE(JwksBuildGuards) {
    CryptoState s;
    const char* arms[][2] = {
        {"pcall(jwt.jwks_build, {})", "non-empty array"},
        {"pcall(jwt.jwks_build, {42})", "must be a table"},
        {"pcall(jwt.jwks_build, {{seed = string.rep('s', 32)}})", "kid"},
        {"pcall(jwt.jwks_build, {{kid = 'k'}})", "seed"},
        {"pcall(jwt.jwks_build, {{kid = 'k', seed = string.rep('s', 31)}})",
         "seed"},
        {"pcall(jwt.jwks_build, {{kid = 'k', seed = string.rep('s', 33)}})",
         "seed"},
        {"pcall(jwt.jwks_build, {{kid = '', seed = string.rep('s', 32)}})",
         "kid"},
    };
    for (const auto& arm : arms) {
        std::string code = std::string("local ok, err = ") + arm[0] +
                           "\nassert(not ok and err:find('" + arm[1] +
                           "', 1, true), tostring(err))";
        BOOST_CHECK_MESSAGE(run_script(s.lua, code), arm[1]);
    }
}

// Full JWKS flow: build a key set, parse it back, verify a token signed with
// the seed using the parsed public key.
BOOST_AUTO_TEST_CASE(JwksJwtRoundTrip) {
    CryptoState s;
    BOOST_CHECK(run_script(
        s.lua,
        "local seed = shield.crypto.random_bytes(32)\n"
        "local doc = jwt.jwks_build({{kid = 'k1', seed = seed}})\n"
        "local keys = jwt.jwks_parse(doc)\n"
        "assert(#keys == 1 and keys[1].kid == 'k1')\n"
        "local token = jwt.sign({sub = 'p1'}, seed, {alg = 'EdDSA', kid = "
        "'k1'})\n"
        "local claims = jwt.verify(token, keys[1].public_key, {alg = 'EdDSA', "
        "kid = 'k1'})\n"
        "assert(claims and claims.sub == 'p1')\n"));
}

BOOST_AUTO_TEST_SUITE_END()
