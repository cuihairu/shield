// [SHIELD_NET] Session transport abstraction
//
// The single IO surface TcpSession needs from a connection transport
// (docs/tls-design.md): one composed write, one read_some, one close path.
// PlainStream wraps a raw tcp::socket (the pre-TLS path, unchanged);
// TlsStream wraps ssl::stream<tcp::socket> after the listener-completed
// handshake. Both bind completion handlers to the executor set via
// set_executor() -- the session strand -- so TcpSession's "every completion
// lands on the strand" invariant survives the abstraction.
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace shield::net {

class SessionStream {
public:
    /// Completion shape of both async_read_some and composed async_write.
    using IoHandler =
        std::function<void(const boost::system::error_code&, std::size_t)>;

    /// Lowest-layer socket type. basic_socket (not basic_stream_socket):
    /// ssl::stream's lowest_layer() only exposes the general socket facade,
    /// which is all the session needs (endpoint, options, hard close).
    using LowestLayer = boost::asio::ip::tcp::socket::lowest_layer_type;

    // Compiler artifact: this line defines the abstract base's deleting-dtor
    // D0 (unreachable — deletion dispatches through the concrete stream's
    // vtable entry, never through the abstract class's own) next to the
    // base-object dtor D2 that the suites exercise dozens of times; the
    // always-zero D0 blocks would mask the line.
    virtual ~SessionStream() = default;  // GCOVR_EXCL_LINE

    /// @brief Lowest-layer socket (endpoint queries, options, hard close).
    virtual LowestLayer& lowest_socket() = 0;

    /// @brief Write the whole buffer; the handler runs on the strand set by
    /// set_executor() (or the stream's own executor before that).
    virtual void async_write(const boost::asio::const_buffer& data,
                             IoHandler handler) = 0;

    /// @brief Read whatever is available (one TLS record for TlsStream);
    /// handler semantics as async_write().
    virtual void async_read_some(const boost::asio::mutable_buffer& data,
                                 IoHandler handler) = 0;

    /// @brief Install the serialization executor for all subsequent
    /// completions. Called exactly once by TcpSession before the first IO
    /// operation; pre-attach IO (the listener-side TLS handshake) runs on the
    /// stream's own executor where no session exists to race with.
    virtual void set_executor(boost::asio::any_io_executor ex) = 0;

    /// @brief "tcp" for PlainStream, "tls" for TlsStream (log/diagnostic tag).
    virtual std::string_view transport_name() const = 0;
};

/// @brief Raw-TCP transport: the pre-TLS session path, byte-for-byte.
class PlainStream final : public SessionStream {
public:
    explicit PlainStream(boost::asio::ip::tcp::socket socket)
        : socket_(std::move(socket)), executor_(socket_.get_executor()) {}

    LowestLayer& lowest_socket() override { return socket_.lowest_layer(); }

    void async_write(const boost::asio::const_buffer& data,
                     IoHandler handler) override;

    void async_read_some(const boost::asio::mutable_buffer& data,
                         IoHandler handler) override;

    void set_executor(boost::asio::any_io_executor ex) override {
        executor_ = std::move(ex);
    }

    std::string_view transport_name() const override { return "tcp"; }

private:
    boost::asio::ip::tcp::socket socket_;
    boost::asio::any_io_executor executor_;
};

/// @brief TLS transport: an already-handshaken ssl::stream<tcp::socket>.
/// The listener owns the handshake phase and hands over a TlsStream only
/// after async_handshake(server) succeeded, so no session ever sees a
/// half-negotiated stream.
class TlsStream final : public SessionStream {
public:
    explicit TlsStream(boost::asio::ip::tcp::socket socket,
                       boost::asio::ssl::context& context)
        : stream_(std::move(socket), context),
          executor_(stream_.lowest_layer().get_executor()) {}

    /// @brief The wrapped TLS stream (listener-side handshake only).
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& raw() {
        return stream_;
    }

    LowestLayer& lowest_socket() override { return stream_.lowest_layer(); }

    void async_write(const boost::asio::const_buffer& data,
                     IoHandler handler) override;

    void async_read_some(const boost::asio::mutable_buffer& data,
                         IoHandler handler) override;

    void set_executor(boost::asio::any_io_executor ex) override {
        executor_ = std::move(ex);
    }

    std::string_view transport_name() const override { return "tls"; }

private:
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream_;
    boost::asio::any_io_executor executor_;
};

}  // namespace shield::net
