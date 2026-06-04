#define BOOST_TEST_MODULE ProtocolHandlerTest
#include <boost/test/unit_test.hpp>
#include <memory>
#include <string>

#include "shield/protocol/protocol_handler.hpp"

using namespace shield::protocol;

namespace {

class MockHandler : public IProtocolHandler {
public:
    explicit MockHandler(ProtocolType type) : type_(type) {}

    void handle_data(uint64_t, const char*, size_t) override {}
    void handle_connection(uint64_t) override {}
    void handle_disconnection(uint64_t) override {}
    bool send_data(uint64_t, const std::string&) override { return true; }
    ProtocolType get_protocol_type() const override { return type_; }

private:
    ProtocolType type_;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(ProtocolHandlerFactoryTests)

BOOST_AUTO_TEST_CASE(TestRegisterAndCreate) {
    ProtocolHandlerFactory factory;
    factory.register_handler(ProtocolType::TCP, []() {
        return std::make_unique<MockHandler>(ProtocolType::TCP);
    });

    auto handler = factory.create_handler(ProtocolType::TCP);
    BOOST_REQUIRE(handler);
    BOOST_CHECK(handler->get_protocol_type() == ProtocolType::TCP);
}

BOOST_AUTO_TEST_CASE(TestCreateUnregisteredReturnsNull) {
    ProtocolHandlerFactory factory;
    auto handler = factory.create_handler(ProtocolType::UDP);
    BOOST_CHECK(!handler);
}

BOOST_AUTO_TEST_CASE(TestRegisterMultipleTypes) {
    ProtocolHandlerFactory factory;

    factory.register_handler(ProtocolType::TCP, []() {
        return std::make_unique<MockHandler>(ProtocolType::TCP);
    });
    factory.register_handler(ProtocolType::HTTP, []() {
        return std::make_unique<MockHandler>(ProtocolType::HTTP);
    });
    factory.register_handler(ProtocolType::WEBSOCKET, []() {
        return std::make_unique<MockHandler>(ProtocolType::WEBSOCKET);
    });

    auto tcp = factory.create_handler(ProtocolType::TCP);
    auto http = factory.create_handler(ProtocolType::HTTP);
    auto ws = factory.create_handler(ProtocolType::WEBSOCKET);

    BOOST_REQUIRE(tcp);
    BOOST_REQUIRE(http);
    BOOST_REQUIRE(ws);

    BOOST_CHECK(tcp->get_protocol_type() == ProtocolType::TCP);
    BOOST_CHECK(http->get_protocol_type() == ProtocolType::HTTP);
    BOOST_CHECK(ws->get_protocol_type() == ProtocolType::WEBSOCKET);
}

BOOST_AUTO_TEST_CASE(TestOverwriteRegistration) {
    ProtocolHandlerFactory factory;

    factory.register_handler(ProtocolType::TCP, []() {
        return std::make_unique<MockHandler>(ProtocolType::TCP);
    });
    // Overwrite with a new creator
    factory.register_handler(ProtocolType::TCP, []() {
        return std::make_unique<MockHandler>(ProtocolType::TCP);
    });

    auto handler = factory.create_handler(ProtocolType::TCP);
    BOOST_REQUIRE(handler);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ProtocolTypeTests)

BOOST_AUTO_TEST_CASE(TestProtocolTypeEnum) {
    // Verify enum values are distinct
    BOOST_CHECK(ProtocolType::TCP != ProtocolType::UDP);
    BOOST_CHECK(ProtocolType::TCP != ProtocolType::HTTP);
    BOOST_CHECK(ProtocolType::HTTP != ProtocolType::WEBSOCKET);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(HttpStructuresTests)

BOOST_AUTO_TEST_CASE(TestHttpResponseDefaults) {
    HttpResponse response;
    BOOST_CHECK_EQUAL(response.status_code, 200);
    BOOST_CHECK_EQUAL(response.status_text, "OK");
    BOOST_CHECK_EQUAL(response.headers.at("Content-Type"), "application/json");
    BOOST_CHECK(response.body.empty());
}

BOOST_AUTO_TEST_CASE(TestHttpRequestDefaults) {
    HttpRequest request;
    BOOST_CHECK(request.method.empty());
    BOOST_CHECK(request.path.empty());
    BOOST_CHECK(request.body.empty());
    BOOST_CHECK_EQUAL(request.connection_id, 0u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(WebSocketFrameTests)

BOOST_AUTO_TEST_CASE(TestWebSocketFrameDefaults) {
    WebSocketFrame frame;
    BOOST_CHECK(frame.type == WebSocketFrameType::TEXT);
    BOOST_CHECK(frame.fin);
    BOOST_CHECK(!frame.masked);
    BOOST_CHECK_EQUAL(frame.mask_key, 0u);
    BOOST_CHECK(frame.payload.empty());
}

BOOST_AUTO_TEST_SUITE_END()
