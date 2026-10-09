// [SHIELD_LUA] shield.crypto implementation.
//
// Hash/MAC/random go through OpenSSL (EVP_Digest / HMAC / RAND_bytes) — it is
// already in the dependency tree and keeps nothing security-relevant in-tree.
// The base64/hex codecs are plain byte-format code written locally on
// purpose: EVP_DecodeBlock's padding-count quirk and BIO_f_base64's newline
// handling make the OpenSSL codecs harder to use correctly than the ~30-line
// RFC 4648 reference, and both variants here are covered by RFC vectors in
// tests/coverage/test_cov_lua_crypto.cpp.
#include "shield/lua/lua_crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include "shield/lua/binding.hpp"

namespace shield::lua {
namespace {

constexpr size_t kMaxRandomBytes = 1u << 20;  // 1 MiB guard for random_bytes

constexpr char kStdAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kUrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

bool base64_value(char c, int8_t* out) {
    if (c >= 'A' && c <= 'Z') {
        *out = static_cast<int8_t>(c - 'A');
    } else if (c >= 'a' && c <= 'z') {
        *out = static_cast<int8_t>(c - 'a' + 26);
    } else if (c >= '0' && c <= '9') {
        *out = static_cast<int8_t>(c - '0' + 52);
    } else if (c == '+' || c == '-') {
        *out = 62;
    } else if (c == '/' || c == '_') {
        *out = 63;
    } else {
        return false;
    }
    return true;
}

// Shared decoder for the standard (padded) and URL (unpadded) variants. Both
// alphabets are accepted per RFC 4648 §5's "canonical" note — the alternate
// characters are unambiguous because '+/' and '-_' occupy disjoint positions
// in the value table above.
std::string base64_decode_impl(const std::string& input) {
    // Strip trailing '=' padding; everything else must be alphabet chars.
    size_t end = input.size();
    while (end > 0 && input[end - 1] == '=') {
        --end;
    }
    const size_t body = end;
    if (input.size() - body > 2) {
        throw std::runtime_error("invalid base64: too much padding");
    }
    // 0-remaining => 4n (canonical); 1-remaining is impossible per RFC 4648.
    if (body % 4 == 1) {
        throw std::runtime_error("invalid base64 length");
    }

    std::string out;
    out.reserve((body / 4) * 3 + 3);
    int8_t vals[4];
    int count = 0;
    uint32_t acc = 0;
    for (size_t i = 0; i < body; ++i) {
        if (!base64_value(input[i], &vals[count])) {
            throw std::runtime_error("invalid base64 character");
        }
        acc = (acc << 6) | static_cast<uint32_t>(vals[count]);
        if (++count == 4) {
            out += static_cast<char>((acc >> 16) & 0xFF);
            out += static_cast<char>((acc >> 8) & 0xFF);
            out += static_cast<char>(acc & 0xFF);
            acc = 0;
            count = 0;
        }
    }
    if (count == 2) {
        out += static_cast<char>((acc >> 4) & 0xFF);
    } else if (count == 3) {
        out += static_cast<char>((acc >> 10) & 0xFF);
        out += static_cast<char>((acc >> 2) & 0xFF);
    }
    return out;
}

// The throw arms of the three OpenSSL wrappers below are defensive only:
// one-shot digest/HMAC/RAND with a fixed algorithm has no injectable
// in-process failure, so no test can drive them. The throw line is excluded
// from the line metric by a START/STOP exclusion pair, and the if line's
// never-taken true arm is registered with a branch marker (branch records are
// keyed on the if line, the line records on the throw line). One trap worth
// keeping: the scanner treats any "EXCL_" token in comments as a real marker,
// so an unterminated region named in prose swallows every later marker in the
// file — write around the literal names.

std::string digest_sha256(const std::string& data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    const int ok =
        EVP_Digest(data.data(), data.size(), md, &len, EVP_sha256(), nullptr);
    if (ok != 1) {  // GCOVR_EXCL_BR_LINE (defensive arm)
        // GCOVR_EXCL_START (defensive: EVP_Digest API contract arm)
        throw std::runtime_error("sha256: EVP_Digest failed");
        // GCOVR_EXCL_STOP
    }
    return std::string(reinterpret_cast<const char*>(md), len);
}

std::string hmac_sha256_impl(const std::string& key, const std::string& data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    const unsigned char* mac =
        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(),
             md, &len);
    if (mac == nullptr) {  // GCOVR_EXCL_BR_LINE (defensive arm)
        // GCOVR_EXCL_START (defensive: HMAC API contract arm)
        throw std::runtime_error("hmac_sha256: HMAC failed");
        // GCOVR_EXCL_STOP
    }
    return std::string(reinterpret_cast<const char*>(md), len);
}

std::string random_bytes_impl(int n) {
    if (n < 0) {
        throw std::runtime_error("random_bytes: count must be >= 0");
    }
    if (static_cast<size_t>(n) > kMaxRandomBytes) {
        throw std::runtime_error("random_bytes: count exceeds 1 MiB limit");
    }
    std::string out;
    if (n == 0) {
        return out;
    }
    out.resize(static_cast<size_t>(n));
    const int rc = RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), n);
    if (rc != 1) {  // GCOVR_EXCL_BR_LINE (defensive arm)
        // GCOVR_EXCL_START (defensive: RAND_bytes API contract arm)
        throw std::runtime_error("random_bytes: RAND_bytes failed");
        // GCOVR_EXCL_STOP
    }
    return out;
}  // GCOVR_EXCL_LINE (throw:false cleanup block, gcovr 8.6 artifact)

// --- key derivation (crypto phase 2) --------------------------------------
// PBKDF2-HMAC-SHA256 (RFC 8018 §5.2) and HKDF-SHA256 (RFC 5869), the account
// password-hashing and key-expansion primitives. Same one-shot OpenSSL
// convention as the digest wrappers: fixed algorithm, so the OpenSSL contract
// arms stay defensive-only under the marker discipline described above.
// Argument guards (iterations/length/salt bounds) are real API surface and
// all driven by tests.

constexpr int kMaxPbkdf2Iterations = 10'000'000;
constexpr int kMaxPbkdf2KeyBytes = 1024;
// RFC 5869 §2.3: HKDF-Expand output is capped at 255 * HashLen (32 for
// SHA-256) = 8160 bytes.
constexpr int kMaxHkdfOutputBytes = 255 * 32;

std::string pbkdf2_hmac_sha256_impl(const std::string& password,
                                    const std::string& salt, int iterations,
                                    int dklen) {
    if (iterations < 1) {
        throw std::runtime_error("pbkdf2_hmac_sha256: iterations must be >= 1");
    }
    if (iterations > kMaxPbkdf2Iterations) {
        throw std::runtime_error(
            "pbkdf2_hmac_sha256: iterations exceeds 10000000 limit");
    }
    if (salt.empty()) {
        throw std::runtime_error("pbkdf2_hmac_sha256: salt must not be empty");
    }
    if (dklen < 1) {
        throw std::runtime_error(
            "pbkdf2_hmac_sha256: derived key length must be >= 1");
    }
    if (dklen > kMaxPbkdf2KeyBytes) {
        throw std::runtime_error(
            "pbkdf2_hmac_sha256: derived key length exceeds 1024 limit");
    }
    std::string out(static_cast<size_t>(dklen), '\0');
    const int ok = PKCS5_PBKDF2_HMAC(
        password.data(), static_cast<int>(password.size()),
        reinterpret_cast<const unsigned char*>(salt.data()),
        static_cast<int>(salt.size()), iterations, EVP_sha256(), dklen,
        reinterpret_cast<unsigned char*>(out.data()));
    if (ok != 1) {  // GCOVR_EXCL_BR_LINE (defensive arm)
        // GCOVR_EXCL_START (defensive: PKCS5_PBKDF2_HMAC API contract arm)
        throw std::runtime_error("pbkdf2_hmac_sha256: PBKDF2 failed");
        // GCOVR_EXCL_STOP
    }
    return out;
}

std::string hkdf_sha256_impl(const std::string& ikm, const std::string& salt,
                             const std::string& info, int length) {
    if (ikm.empty()) {
        throw std::runtime_error("hkdf_sha256: ikm must not be empty");
    }
    if (length < 1) {
        throw std::runtime_error("hkdf_sha256: output length must be >= 1");
    }
    if (length > kMaxHkdfOutputBytes) {
        throw std::runtime_error(
            "hkdf_sha256: output length exceeds RFC 5869 8160-byte limit");
    }
    // RFC 5869 §2.2: absent salt is replaced by HashLen zero octets —
    // OpenSSL takes the salt verbatim, so supply the zero form here; empty
    // info is simply not added (equivalent to the zero-length form).
    const unsigned char zeros[EVP_MAX_MD_SIZE] = {0};
    const unsigned char* salt_ptr =
        salt.empty() ? zeros
                     : reinterpret_cast<const unsigned char*>(salt.data());
    const int salt_len = salt.empty() ? EVP_MD_size(EVP_sha256())
                                      : static_cast<int>(salt.size());
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
        EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), &EVP_PKEY_CTX_free);
    if (ctx == nullptr || EVP_PKEY_derive_init(ctx.get()) <= 0 ||
        EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) <= 0 ||
        EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(), salt_ptr, salt_len) <= 0 ||
        EVP_PKEY_CTX_set1_hkdf_key(
            ctx.get(), reinterpret_cast<const unsigned char*>(ikm.data()),
            static_cast<int>(ikm.size())) <= 0 ||
        (!info.empty() &&
         EVP_PKEY_CTX_add1_hkdf_info(
             ctx.get(), reinterpret_cast<const unsigned char*>(info.data()),
             static_cast<int>(info.size())) <= 0)) {
        // GCOVR_EXCL_BR_LINE (defensive arm)
        // GCOVR_EXCL_START (defensive: HKDF context setup contract arm)
        throw std::runtime_error("hkdf_sha256: context setup failed");
        // GCOVR_EXCL_STOP
    }
    std::string out(static_cast<size_t>(length), '\0');
    size_t outlen = out.size();
    const int ok = EVP_PKEY_derive(
        ctx.get(), reinterpret_cast<unsigned char*>(out.data()), &outlen);
    if (ok <= 0 ||
        outlen != out.size()) {  // GCOVR_EXCL_BR_LINE (defensive arm)
        // GCOVR_EXCL_START (defensive: EVP_PKEY_derive API contract arm)
        throw std::runtime_error("hkdf_sha256: derive failed");
        // GCOVR_EXCL_STOP
    }
    return out;
}

}  // namespace

void register_crypto_api(shd::table& shield) {
    shd::table crypto = shield.create_table("crypto");

    // --- codecs -----------------------------------------------------------
    // All encoders take/return Lua strings (byte-transparent); the hash and
    // HMAC functions return RAW digests (compose with hex_encode/base64url_
    // encode for text forms — jwt.lua is the worked example).

    crypto.set_function("base64_encode", [](const std::string& data) {
        std::string out;
        out.reserve(4 * ((data.size() + 2) / 3));
        size_t i = 0;
        while (i + 3 <= data.size()) {
            const uint32_t acc =
                (static_cast<unsigned char>(data[i]) << 16) |
                (static_cast<unsigned char>(data[i + 1]) << 8) |
                static_cast<unsigned char>(data[i + 2]);
            out += kStdAlphabet[(acc >> 18) & 0x3F];
            out += kStdAlphabet[(acc >> 12) & 0x3F];
            out += kStdAlphabet[(acc >> 6) & 0x3F];
            out += kStdAlphabet[acc & 0x3F];
            i += 3;
        }
        const size_t rem = data.size() - i;
        if (rem == 1) {
            const uint32_t acc = static_cast<unsigned char>(data[i]) << 16;
            out += kStdAlphabet[(acc >> 18) & 0x3F];
            out += kStdAlphabet[(acc >> 12) & 0x3F];
            out += "==";
        } else if (rem == 2) {
            const uint32_t acc = (static_cast<unsigned char>(data[i]) << 16) |
                                 (static_cast<unsigned char>(data[i + 1]) << 8);
            out += kStdAlphabet[(acc >> 18) & 0x3F];
            out += kStdAlphabet[(acc >> 12) & 0x3F];
            out += kStdAlphabet[(acc >> 6) & 0x3F];
            out += '=';
        }
        return out;
    });  // GCOVR_EXCL_LINE (throw:false cleanup block — gcovr 8.6 artifact)

    crypto.set_function("base64_decode", [](const std::string& data) {
        return base64_decode_impl(data);
    });

    // RFC 4648 §5: URL-safe alphabet, no padding (JWT segment form).
    crypto.set_function("base64url_encode", [](const std::string& data) {
        std::string out;
        out.reserve(4 * ((data.size() + 2) / 3));
        size_t i = 0;
        while (i + 3 <= data.size()) {
            const uint32_t acc =
                (static_cast<unsigned char>(data[i]) << 16) |
                (static_cast<unsigned char>(data[i + 1]) << 8) |
                static_cast<unsigned char>(data[i + 2]);
            out += kUrlAlphabet[(acc >> 18) & 0x3F];
            out += kUrlAlphabet[(acc >> 12) & 0x3F];
            out += kUrlAlphabet[(acc >> 6) & 0x3F];
            out += kUrlAlphabet[acc & 0x3F];
            i += 3;
        }
        const size_t rem = data.size() - i;
        if (rem == 1) {
            const uint32_t acc = static_cast<unsigned char>(data[i]) << 16;
            out += kUrlAlphabet[(acc >> 18) & 0x3F];
            out += kUrlAlphabet[(acc >> 12) & 0x3F];
        } else if (rem == 2) {
            const uint32_t acc = (static_cast<unsigned char>(data[i]) << 16) |
                                 (static_cast<unsigned char>(data[i + 1]) << 8);
            out += kUrlAlphabet[(acc >> 18) & 0x3F];
            out += kUrlAlphabet[(acc >> 12) & 0x3F];
            out += kUrlAlphabet[(acc >> 6) & 0x3F];
        }
        return out;
    });  // GCOVR_EXCL_LINE (throw:false cleanup block — gcovr 8.6 artifact)

    crypto.set_function("base64url_decode", [](const std::string& data) {
        return base64_decode_impl(data);
    });

    crypto.set_function("hex_encode", [](const std::string& data) {
        static const char* kDigits = "0123456789abcdef";
        std::string out;
        out.reserve(data.size() * 2);
        for (unsigned char c : data) {
            out += kDigits[c >> 4];
            out += kDigits[c & 0x0F];
        }
        return out;
    });  // GCOVR_EXCL_LINE (throw:false cleanup block — gcovr 8.6 artifact)

    crypto.set_function("hex_decode", [](const std::string& data) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        if (data.size() % 2 != 0) {
            throw std::runtime_error("invalid hex length");
        }
        std::string out;
        out.reserve(data.size() / 2);
        for (size_t i = 0; i < data.size(); i += 2) {
            const int hi = nibble(data[i]);
            const int lo = nibble(data[i + 1]);
            if (hi < 0 || lo < 0) {
                throw std::runtime_error("invalid hex character");
            }
            out += static_cast<char>((hi << 4) | lo);
        }
        return out;
    });

    // --- digests / MAC / randomness ---------------------------------------

    crypto.set_function(
        "sha256", [](const std::string& data) { return digest_sha256(data); });

    // Direct function binding (not a wrapping lambda): the multi-line lambda
    // form put a never-executed entry block on the continuation line, which
    // gcovr reports as an uncovered line (8.6 artifact). The wrapper and the
    // impl have identical signatures.
    crypto.set_function("hmac_sha256", &hmac_sha256_impl);

    crypto.set_function("random_bytes",
                        [](int n) { return random_bytes_impl(n); });

    // --- key derivation (crypto phase 2) ----------------------------------
    // PBKDF2-HMAC-SHA256 for password hashing (store salt + iterations with
    // the derived key; verify via constant_time_compare), HKDF-SHA256 for
    // expanding negotiated secrets into per-purpose keys. Both return RAW
    // bytes like the digest functions above.

    // Direct function bindings, same rationale as hmac_sha256 above.
    crypto.set_function("pbkdf2_hmac_sha256", &pbkdf2_hmac_sha256_impl);
    crypto.set_function("hkdf_sha256", &hkdf_sha256_impl);

    // Length difference leaks only the length (inherent to any such check);
    // content comparison runs in constant time via OpenSSL CRYPTO_memcmp.
    crypto.set_function("constant_time_compare", [](const std::string& a,
                                                    const std::string& b) {
        if (a.size() != b.size()) {
            return false;
        }
        return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
    });
}

}  // namespace shield::lua
