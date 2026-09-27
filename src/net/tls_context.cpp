// [SHIELD_NET] Server TLS context factory implementation
#include "shield/net/tls_context.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "shield/log/logger.hpp"

namespace shield::net {

bool make_tls_server_context(const std::string& cert_file,
                             const std::string& key_file,
                             std::shared_ptr<boost::asio::ssl::context>& out,
                             std::string* error) {
    auto fail = [error](const std::string& reason) {
        if (error != nullptr) {
            *error = reason;
        }
        auto& log = shield::log::get_logger("net");
        SHIELD_LOG_ERROR(log, "TLS context rejected: " + reason);
        return false;
    };

    if (cert_file.empty() || key_file.empty()) {
        return fail("tls.cert_file / tls.key_file must not be empty");
    }

    auto ctx = std::make_shared<boost::asio::ssl::context>(
        boost::asio::ssl::context::tls_server);

    boost::system::error_code ec;
    // Minimum TLS 1.2: older protocol versions are deprecated and disabled
    // outright rather than configurable.
    ::SSL_CTX_set_min_proto_version(ctx->native_handle(), TLS1_2_VERSION);
    ctx->set_options(boost::asio::ssl::context::no_compression, ec);
    if (ec) {  // GCOVR_EXCL_START (option application cannot fail for
               // no_compression on any supported OpenSSL build)
        return fail("failed to apply TLS context options: " + ec.message());
        // GCOVR_EXCL_STOP
    }

    ctx->use_certificate_chain_file(cert_file, ec);
    if (ec) {
        return fail("failed to load certificate '" + cert_file +
                    "': " + ec.message());
    }
    ctx->use_private_key_file(key_file, boost::asio::ssl::context::pem, ec);
    if (ec) {
        return fail("failed to load private key '" + key_file +
                    "': " + ec.message());
    }
    // Each load call verifies its own input; the cross-check (cert/key pair
    // belonging together) is a separate OpenSSL step and must not be skipped.
    // GCOVR_EXCL_START (defensive: on OpenSSL 3.x use_private_key_file
    // already rejects a mismatched key at load time with "key values
    // mismatch" -- that earlier arm is what the tests exercise -- so this
    // cross-check's failure arm is unreachable by any test on supported
    // OpenSSL builds. The check itself stays as belt-and-braces for builds
    // that skip the load-time consistency check.)
    if (::SSL_CTX_check_private_key(ctx->native_handle()) != 1) {
        unsigned long err = ::ERR_get_error();
        char buf[256];
        ::ERR_error_string_n(err, buf, sizeof(buf));
        return fail(std::string("certificate/private key mismatch (") + buf +
                    ")");
    }
    // GCOVR_EXCL_STOP

    out = std::move(ctx);
    return true;
}

}  // namespace shield::net
