#include "shield/cluster/cluster_auth.hpp"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <limits>
#include <stdexcept>

namespace shield::cluster {
namespace {
std::string hex(const unsigned char* bytes, size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 15];
    }
    return result;
}
void append(std::string& out, std::string_view field) {
    out += std::to_string(field.size()) + ":";
    out.append(field);
}
}  // namespace

std::string cluster_auth_nonce() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1)
        throw std::runtime_error("cluster auth random generation failed");
    return hex(bytes.data(), bytes.size());
}

std::string cluster_auth_proof(std::string_view token, std::string_view phase,
                               const HelloMsg& hello, const HelloAckMsg& ack) {
    if (token.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("cluster auth token too long");
    std::string transcript;
    append(transcript, "shield.cluster.psk.v2");
    append(transcript, phase);
    auto message = [&transcript](const auto& msg) {
        append(transcript, std::to_string(msg.proto_version));
        append(transcript, msg.node_id);
        append(transcript, std::to_string(msg.epoch));
        append(transcript, msg.nonce);
        append(transcript, msg.response_nonce);
        append(transcript, std::to_string(msg.timestamp_ms));
    };
    message(hello);
    message(ack);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), token.data(), static_cast<int>(token.size()),
              reinterpret_cast<const unsigned char*>(transcript.data()),
              transcript.size(), digest.data(), &length))
        throw std::runtime_error("cluster auth HMAC failed");
    return hex(digest.data(), length);
}

bool cluster_auth_equal(std::string_view expected, std::string_view proof) {
    return expected.size() == proof.size() &&
           CRYPTO_memcmp(expected.data(), proof.data(), expected.size()) == 0;
}

bool cluster_auth_fresh(int64_t timestamp_ms, int64_t now_ms, int window_ms) {
    // Reject non-positive timestamps before unsigned subtraction (no signed
    // overflow for hostile INT64_MIN/MAX input).
    if (timestamp_ms <= 0 || now_ms <= 0 || window_ms <= 0) return false;
    const auto a = static_cast<uint64_t>(timestamp_ms);
    const auto b = static_cast<uint64_t>(now_ms);
    return (a > b ? a - b : b - a) <= static_cast<uint64_t>(window_ms);
}
}  // namespace shield::cluster
