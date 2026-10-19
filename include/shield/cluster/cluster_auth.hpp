// PSK handshake primitives. No secret is serialized or logged.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "shield/cluster/cluster_messages.hpp"

namespace shield::cluster {

// RAND_bytes-backed 256-bit nonce, encoded as lowercase hex.
std::string cluster_auth_nonce();

// Length-prefixed, domain/phase-separated transcript, HMAC-SHA256 hex.
// Covers protocol, both identities/epochs, nonces and UTC timestamps.
std::string cluster_auth_proof(std::string_view token, std::string_view phase,
                               const HelloMsg& hello,
                               const HelloAckMsg& ack = {});
bool cluster_auth_equal(std::string_view expected, std::string_view proof);
bool cluster_auth_fresh(int64_t timestamp_ms, int64_t now_ms, int window_ms);

}  // namespace shield::cluster
