// [SHIELD_NET] Session transport abstraction implementation
#include "shield/net/session_stream.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

namespace shield::net {

void PlainStream::async_write(const boost::asio::const_buffer& data,
                              IoHandler handler) {
    boost::asio::async_write(
        socket_, data,
        boost::asio::bind_executor(executor_, std::move(handler)));
}

void PlainStream::async_read_some(const boost::asio::mutable_buffer& data,
                                  IoHandler handler) {
    socket_.async_read_some(
        data, boost::asio::bind_executor(executor_, std::move(handler)));
}

void TlsStream::async_write(const boost::asio::const_buffer& data,
                            IoHandler handler) {
    boost::asio::async_write(
        stream_, data,
        boost::asio::bind_executor(executor_, std::move(handler)));
}

void TlsStream::async_read_some(const boost::asio::mutable_buffer& data,
                                IoHandler handler) {
    stream_.async_read_some(
        data, boost::asio::bind_executor(executor_, std::move(handler)));
}

}  // namespace shield::net
