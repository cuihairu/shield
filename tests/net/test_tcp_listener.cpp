#define BOOST_TEST_MODULE TcpListenerTests
#include <boost/test/unit_test.hpp>

#include "shield/net/listener.hpp"

#include <boost/asio.hpp>

#include <cstdint>

namespace {

std::uint16_t reserve_ephemeral_port(boost::asio::io_context& io) {
    boost::asio::ip::tcp::acceptor acceptor(
        io, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));
    return acceptor.local_endpoint().port();
}

}  // namespace

BOOST_AUTO_TEST_SUITE(TcpListenerTests)

BOOST_AUTO_TEST_CASE(PortConflictLeavesListenerClosed) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    shield::net::SessionCallbacks callbacks;
    shield::net::TcpListener first(io, port, callbacks);
    BOOST_REQUIRE(first.is_open());

    shield::net::TcpListener second(io, port, callbacks);
    BOOST_CHECK(!second.is_open());
}

BOOST_AUTO_TEST_SUITE_END()
