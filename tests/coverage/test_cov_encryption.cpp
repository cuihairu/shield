#define BOOST_TEST_MODULE CovEncryption
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "shield/transport/encryption.hpp"

using shield::transport::AesGcmEncryption;
using shield::transport::Cipher;
using shield::transport::create_encryption;
using shield::transport::NoEncryption;

namespace {

constexpr const char* kKey128 = "0123456789abcdef";
constexpr const char* kKey256 = "0123456789abcdef0123456789abcdef";

std::string to_text(const std::vector<uint8_t>& data) {
    return std::string(data.begin(), data.end());
}

}  // namespace

BOOST_AUTO_TEST_SUITE(CovEncryption)

BOOST_AUTO_TEST_CASE(NoEncryptionPassesDataThrough) {
    NoEncryption enc;
    auto data = enc.encrypt("plain");
    BOOST_CHECK_EQUAL(to_text(data), "plain");
    BOOST_CHECK_EQUAL(to_text(enc.decrypt(data)), "plain");
    BOOST_CHECK_EQUAL(enc.cipher_name(), "none");
}

BOOST_AUTO_TEST_CASE(Aes128RoundTrip) {
    AesGcmEncryption enc(kKey128);
    BOOST_CHECK_EQUAL(enc.cipher_name(), "aes-128-gcm");
    auto data = enc.encrypt("secret payload");
    BOOST_CHECK_GE(data.size(), 12u + 16u + 14u);
    BOOST_CHECK_NE(to_text(data), "secret payload");
    BOOST_CHECK_EQUAL(to_text(enc.decrypt(data)), "secret payload");
}

BOOST_AUTO_TEST_CASE(Aes256RoundTrip) {
    AesGcmEncryption enc(kKey256);
    BOOST_CHECK_EQUAL(enc.cipher_name(), "aes-256-gcm");
    auto data = enc.encrypt("other payload");
    BOOST_CHECK_NE(to_text(data), "other payload");
    BOOST_CHECK_EQUAL(to_text(enc.decrypt(data)), "other payload");
}

BOOST_AUTO_TEST_CASE(ConstructorRejectsInvalidKeyLength) {
    BOOST_CHECK_THROW((AesGcmEncryption("short")), std::invalid_argument);
    std::string bad_key(33, 'k');
    BOOST_CHECK_THROW((AesGcmEncryption(bad_key)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(DecryptRejectsTooShortData) {
    AesGcmEncryption enc(kKey128);
    std::vector<uint8_t> short_data(27, 0x00);
    BOOST_CHECK_THROW(enc.decrypt(short_data), std::runtime_error);
    BOOST_CHECK_THROW(enc.decrypt(std::vector<uint8_t>{}), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DecryptRejectsCorruptedTag) {
    AesGcmEncryption enc(kKey256);
    auto data = enc.encrypt("authentic data");
    data.back() ^= 0xFF;
    BOOST_CHECK_THROW(enc.decrypt(data), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DecryptRejectsCorruptedCipherText) {
    AesGcmEncryption enc(kKey128);
    auto data = enc.encrypt("authentic data");
    data[12] ^= 0x01;
    BOOST_CHECK_THROW(enc.decrypt(data), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(FactoryCreatesByCipher) {
    auto none = create_encryption(Cipher::NONE, "");
    BOOST_REQUIRE(none != nullptr);
    BOOST_CHECK_EQUAL(none->cipher_name(), "none");
    BOOST_CHECK_EQUAL(to_text(none->encrypt("abc")), "abc");

    auto aes128 = create_encryption(Cipher::AES_128_GCM, kKey128);
    BOOST_REQUIRE(aes128 != nullptr);
    BOOST_CHECK_EQUAL(aes128->cipher_name(), "aes-128-gcm");
    auto data = aes128->encrypt("factory");
    BOOST_CHECK_EQUAL(to_text(aes128->decrypt(data)), "factory");

    auto aes256 = create_encryption(Cipher::AES_256_GCM, kKey256);
    BOOST_REQUIRE(aes256 != nullptr);
    BOOST_CHECK_EQUAL(aes256->cipher_name(), "aes-256-gcm");
    data = aes256->encrypt("factory-256");
    BOOST_CHECK_EQUAL(to_text(aes256->decrypt(data)), "factory-256");

    BOOST_CHECK(create_encryption(Cipher::CHACHA20_POLY1305, kKey256) ==
                nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
