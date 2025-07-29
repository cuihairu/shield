#define BOOST_TEST_MODULE BinaryProtocolTest
#include <boost/test/unit_test.hpp>
#include <vector>
#include <string>

#include "shield/protocol/binary_protocol.hpp"

using namespace shield::protocol;

BOOST_AUTO_TEST_SUITE(BinaryProtocolTests)

BOOST_AUTO_TEST_CASE(TestEncodeDecodeSmallMessage) {
    std::string original_message = "Hello, World!";
    
    // Test encoding
    std::vector<char> encoded = BinaryProtocol::encode(original_message);
    
    // Verify encoded message is larger than original (includes header)
    BOOST_CHECK_GT(encoded.size(), original_message.size());
    BOOST_CHECK_EQUAL(encoded.size(), original_message.size() + BinaryProtocol::HEADER_SIZE);
    
    // Test decoding
    auto [decoded_message, bytes_consumed] = BinaryProtocol::decode(encoded.data(), encoded.size());
    
    BOOST_CHECK_EQUAL(decoded_message, original_message);
    BOOST_CHECK_EQUAL(bytes_consumed, encoded.size());
}

BOOST_AUTO_TEST_CASE(TestEncodeDecodeLargeMessage) {
    std::string original_message(1000, 'A'); // 1000 'A' characters
    
    std::vector<char> encoded = BinaryProtocol::encode(original_message);
    auto [decoded_message, bytes_consumed] = BinaryProtocol::decode(encoded.data(), encoded.size());
    
    BOOST_CHECK_EQUAL(decoded_message, original_message);
    BOOST_CHECK_EQUAL(bytes_consumed, encoded.size());
}

BOOST_AUTO_TEST_CASE(TestDecodeInsufficientData) {
    std::string message = "Test message";
    std::vector<char> encoded = BinaryProtocol::encode(message);
    
    // Try to decode with insufficient header data
    auto [decoded1, consumed1] = BinaryProtocol::decode(encoded.data(), 2);
    BOOST_CHECK_EQUAL(decoded1, "");
    BOOST_CHECK_EQUAL(consumed1, 0);
    
    // Try to decode with header but insufficient body data
    auto [decoded2, consumed2] = BinaryProtocol::decode(encoded.data(), BinaryProtocol::HEADER_SIZE + 2);
    BOOST_CHECK_EQUAL(decoded2, "");
    BOOST_CHECK_EQUAL(consumed2, 0);
}

BOOST_AUTO_TEST_CASE(TestDecodeEmptyMessage) {
    std::string empty_message = "";
    
    std::vector<char> encoded = BinaryProtocol::encode(empty_message);
    auto [decoded_message, bytes_consumed] = BinaryProtocol::decode(encoded.data(), encoded.size());
    
    BOOST_CHECK_EQUAL(decoded_message, empty_message);
    BOOST_CHECK_EQUAL(bytes_consumed, encoded.size());
}

BOOST_AUTO_TEST_CASE(TestHeaderSize) {
    // Verify header size constant
    BOOST_CHECK_EQUAL(BinaryProtocol::HEADER_SIZE, sizeof(uint32_t));
}

BOOST_AUTO_TEST_SUITE_END()