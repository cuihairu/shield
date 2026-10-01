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
#include <sol/sol.hpp>  // B0 seam: only the register_full_shield_api test below
                        // still drives the sol2-based lua_api surface.
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

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math,
                       sol::lib::table, sol::lib::os, sol::lib::coroutine);
    register_full_shield_api(lua, &manager, &runtime);
    // sol2 path: register_full_shield_api is still sol2-based until the
    // lua_api migration batch; run_script above takes the thin layer's state.
    auto result = lua.safe_script(
        "assert(type(shield.crypto) == 'table')\n"
        "assert(shield.crypto.hex_encode('a') == '61')\n"
        "assert(#shield.crypto.sha256('x') == 32)\n",
        sol::script_pass_on_error);
    if (!result.valid()) {
        std::fprintf(stderr, "lua error: %s\n",
                     result.get<sol::error>().what());
    }
    BOOST_CHECK(result.valid());
}

BOOST_AUTO_TEST_SUITE_END()
