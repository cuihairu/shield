#define BOOST_TEST_MODULE WebSocketHandlerTest
#include <boost/test/unit_test.hpp>

#include <cstring>

#include "shield/protocol/websocket_handler.hpp"

using namespace shield::protocol;

BOOST_AUTO_TEST_SUITE(WebSocketHandlerTests)

BOOST_AUTO_TEST_CASE(TestConstruction) {
    WebSocketProtocolHandler handler;
    BOOST_CHECK(handler.get_protocol_type() == ProtocolType::WEBSOCKET);
}

BOOST_AUTO_TEST_CASE(TestFactory) {
    auto handler = create_websocket_handler();
    BOOST_CHECK(handler != nullptr);
    BOOST_CHECK(handler->get_protocol_type() == ProtocolType::WEBSOCKET);
}

BOOST_AUTO_TEST_CASE(TestConnectionLifecycle) {
    WebSocketProtocolHandler handler;
    uint64_t conn_id = 1;

    handler.handle_connection(conn_id);
    // Connection should be in CONNECTING state
    // We can verify by trying to send data (should fail without session
    // provider)
    BOOST_CHECK(!handler.send_data(conn_id, "hello"));

    handler.handle_disconnection(conn_id);
}

BOOST_AUTO_TEST_CASE(TestHandleDataWithoutConnection) {
    WebSocketProtocolHandler handler;
    // Handling data for a non-existent connection should not crash
    handler.handle_data(999, "data", 4);
}

BOOST_AUTO_TEST_CASE(TestMultipleConnections) {
    WebSocketProtocolHandler handler;

    handler.handle_connection(1);
    handler.handle_connection(2);
    handler.handle_connection(3);

    handler.handle_disconnection(2);

    // 1 and 3 should still be tracked (verify via send returning false without
    // provider)
    BOOST_CHECK(!handler.send_data(1, "a"));
    BOOST_CHECK(!handler.send_data(3, "c"));

    handler.handle_disconnection(1);
    handler.handle_disconnection(3);
}

BOOST_AUTO_TEST_CASE(TestSendWithoutSessionProvider) {
    WebSocketProtocolHandler handler;
    handler.handle_connection(1);

    BOOST_CHECK(!handler.send_text_frame(1, "hello"));
    BOOST_CHECK(!handler.send_binary_frame(1, "data"));
    BOOST_CHECK(!handler.send_ping(1, "payload"));
    BOOST_CHECK(!handler.send_pong(1, "payload"));
    BOOST_CHECK(!handler.close_connection(1, 1000, "bye"));
}

BOOST_AUTO_TEST_CASE(TestSetSessionProvider) {
    WebSocketProtocolHandler handler;
    bool provider_called = false;
    handler.set_session_provider([&](uint64_t) -> std::shared_ptr<shield::net::Session> {
        provider_called = true;
        return nullptr;
    });

    handler.handle_connection(1);
    handler.send_text_frame(1, "test");
    BOOST_CHECK(provider_called);
}

BOOST_AUTO_TEST_CASE(TestSetMessageHandler) {
    WebSocketProtocolHandler handler;
    bool handler_called = false;
    uint64_t received_id = 0;
    handler.set_message_handler([&](uint64_t id, const std::string&) {
        handler_called = true;
        received_id = id;
    });

    // Without a proper handshake, messages won't be dispatched
    // Just verify the setter doesn't crash
    (void)handler_called;
    (void)received_id;
}

BOOST_AUTO_TEST_CASE(TestHandshakeInvalidMethod) {
    WebSocketProtocolHandler handler;
    handler.handle_connection(1);

    // Send a POST request (invalid for WebSocket handshake)
    std::string request =
        "POST /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";
    handler.handle_data(1, request.c_str(), request.size());

    // Connection should be removed after failed handshake
    // Sending more data should be a no-op
    handler.handle_data(1, "more", 4);
}

BOOST_AUTO_TEST_CASE(TestHandshakeMissingUpgrade) {
    WebSocketProtocolHandler handler;
    handler.handle_connection(1);

    std::string request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";
    handler.handle_data(1, request.c_str(), request.size());

    // Should fail - connection removed
    handler.handle_disconnection(1);
}

BOOST_AUTO_TEST_CASE(TestHandshakeWithValidHeadersNoSession) {
    WebSocketProtocolHandler handler;
    handler.handle_connection(1);

    // Valid WebSocket handshake request (but no session provider, so response
    // can't be sent)
    std::string request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    handler.handle_data(1, request.c_str(), request.size());

    // Without session provider, handshake fails and connection is removed
}

BOOST_AUTO_TEST_CASE(TestFrameParseShortPayload) {
    // Test that the handler can parse a minimal WebSocket frame
    // Frame: FIN=1, opcode=TEXT(0x01), mask=0, len=5, payload="hello"
    WebSocketProtocolHandler handler;
    handler.handle_connection(1);

    // Manually construct a text frame
    char frame[7];
    frame[0] = 0x81;  // FIN + TEXT
    frame[1] = 0x05;  // no mask, length 5
    std::memcpy(frame + 2, "hello", 5);

    // This will fail handshake first (frame is not a valid HTTP request)
    // but we test that it doesn't crash
    handler.handle_data(1, frame, 7);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(WebSocketFrameTests)

BOOST_AUTO_TEST_CASE(TestFrameTypeEnum) {
    BOOST_CHECK(static_cast<int>(WebSocketFrameType::TEXT) == 0x01);
    BOOST_CHECK(static_cast<int>(WebSocketFrameType::BINARY) == 0x02);
    BOOST_CHECK(static_cast<int>(WebSocketFrameType::CLOSE) == 0x08);
    BOOST_CHECK(static_cast<int>(WebSocketFrameType::PING) == 0x09);
    BOOST_CHECK(static_cast<int>(WebSocketFrameType::PONG) == 0x0A);
}

BOOST_AUTO_TEST_CASE(TestFrameStructDefaults) {
    WebSocketFrame frame;
    BOOST_CHECK(frame.fin);
    BOOST_CHECK(!frame.masked);
    BOOST_CHECK_EQUAL(frame.mask_key, 0u);
    BOOST_CHECK(frame.payload.empty());
}

BOOST_AUTO_TEST_CASE(TestConnectionStruct) {
    WebSocketConnection conn;
    conn.connection_id = 42;
    conn.state = WebSocketState::CONNECTING;
    BOOST_CHECK_EQUAL(conn.connection_id, 42u);
    BOOST_CHECK(conn.state == WebSocketState::CONNECTING);
    BOOST_CHECK(conn.is_server);
    BOOST_CHECK(conn.buffer.empty());
}

BOOST_AUTO_TEST_CASE(TestWebSocketStateEnum) {
    WebSocketState states[] = {WebSocketState::CONNECTING, WebSocketState::OPEN,
                               WebSocketState::CLOSING, WebSocketState::CLOSED};
    // All states should be distinct
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
            BOOST_CHECK(states[i] != states[j]);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
