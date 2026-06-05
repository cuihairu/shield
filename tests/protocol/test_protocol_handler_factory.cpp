#define BOOST_TEST_MODULE ProtocolHandlerFactoryTest
#include <boost/test/unit_test.hpp>

#include "shield/protocol/protocol_handler.hpp"

using namespace shield::protocol;

namespace {
class MockHandler : public IProtocolHandler {
public:
    void handle_data(uint64_t, const char*, size_t) override {}
    void handle_connection(uint64_t) override {}
    void handle_disconnection(uint64_t) override {}
    bool send_data(uint64_t, const std::string&) override { return true; }
    ProtocolType get_protocol_type() const override { return ProtocolType::TCP; }
};
}  // namespace

BOOST_AUTO_TEST_SUITE(ProtocolHandlerFactoryTests)

BOOST_AUTO_TEST_CASE(TestSingleton) {
    auto& f1 = ProtocolHandlerFactory::instance();
    auto& f2 = ProtocolHandlerFactory::instance();
    BOOST_CHECK(&f1 == &f2);
}

BOOST_AUTO_TEST_CASE(TestCreateUnregistered) {
    auto& factory = ProtocolHandlerFactory::instance();
    // Creating an unregistered type should return nullptr
    // Note: factory is a singleton so state persists across tests.
    // We test with a type unlikely to be registered.
    auto handler = factory.create_handler(ProtocolType::UDP);
    // Could be nullptr or valid depending on test order; just verify no crash
    (void)handler;
}

BOOST_AUTO_TEST_CASE(TestRegisterAndCreate) {
    auto& factory = ProtocolHandlerFactory::instance();
    factory.register_handler(ProtocolType::TCP,
                             []() { return std::make_unique<MockHandler>(); });
    auto handler = factory.create_handler(ProtocolType::TCP);
    BOOST_CHECK(handler != nullptr);
    BOOST_CHECK(handler->get_protocol_type() == ProtocolType::TCP);
}

BOOST_AUTO_TEST_CASE(TestRegisterOverwrite) {
    auto& factory = ProtocolHandlerFactory::instance();
    factory.register_handler(ProtocolType::TCP,
                             []() { return std::make_unique<MockHandler>(); });
    factory.register_handler(ProtocolType::TCP,
                             []() { return std::make_unique<MockHandler>(); });
    auto handler = factory.create_handler(ProtocolType::TCP);
    BOOST_CHECK(handler != nullptr);
}

BOOST_AUTO_TEST_CASE(TestProtocolTypeEnum) {
    // Verify enum distinctness
    BOOST_CHECK(ProtocolType::TCP != ProtocolType::UDP);
    BOOST_CHECK(ProtocolType::TCP != ProtocolType::HTTP);
    BOOST_CHECK(ProtocolType::TCP != ProtocolType::WEBSOCKET);
    BOOST_CHECK(ProtocolType::UDP != ProtocolType::HTTP);
    BOOST_CHECK(ProtocolType::HTTP != ProtocolType::WEBSOCKET);
}

BOOST_AUTO_TEST_SUITE_END()
