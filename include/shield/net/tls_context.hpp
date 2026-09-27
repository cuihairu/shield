// [SHIELD_NET] Server TLS context factory
//
// Builds a boost::asio::ssl::context from PEM cert/key files with the
// server-side policy from docs/tls-design.md baked in (TLS 1.2 minimum,
// no compression, unencrypted private keys). Every failure mode returns
// false with a caller-readable error string so bootstrap can fail loudly
// at startup instead of serving plaintext by accident.
#pragma once

#include <boost/asio/ssl/context.hpp>
#include <memory>
#include <string>

namespace shield::net {

/// @brief Build a server TLS context from PEM files.
/// @param cert_file Path to the PEM certificate chain.
/// @param key_file Path to the PEM private key (unencrypted).
/// @param[out] out Receives the context on success.
/// @param[out] error Receives a human-readable failure reason on failure
///                   (missing file, malformed PEM, cert/key mismatch).
/// @return true when the context is ready to serve handshakes.
bool make_tls_server_context(const std::string& cert_file,
                             const std::string& key_file,
                             std::shared_ptr<boost::asio::ssl::context>& out,
                             std::string* error);

}  // namespace shield::net
