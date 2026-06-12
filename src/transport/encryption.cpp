// [SHIELD_TRANSPORT] Encryption implementation
#include "shield/transport/encryption.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace shield::transport {

// AesGcmEncryption implementation
AesGcmEncryption::AesGcmEncryption(std::string_view key)
    : key_(key.begin(), key.end()) {

    if (key_.size() == 16) {
        cipher_ = Cipher::AES_128_GCM;
    } else if (key_.size() == 32) {
        cipher_ = Cipher::AES_256_GCM;
    } else {
        throw std::invalid_argument("Key must be 16 or 32 bytes");
    }
}

std::vector<uint8_t> AesGcmEncryption::encrypt(std::string_view data) {
    const EVP_CIPHER* cipher = nullptr;
    if (cipher_ == Cipher::AES_128_GCM) {
        cipher = EVP_aes_128_gcm();
    } else {
        cipher = EVP_aes_256_gcm();
    }

    // Generate IV
    std::vector<uint8_t> iv(12);  // GCM standard IV size
    RAND_bytes(iv.data(), iv.size());

    // Initialize cipher context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, cipher, nullptr, key_.data(), iv.data());

    // Allocate output: IV + ciphertext + tag
    std::vector<uint8_t> output(iv.size() + data.size() + 16);
    std::copy(iv.begin(), iv.end(), output.begin());

    // Encrypt
    int len;
    EVP_EncryptUpdate(ctx, output.data() + iv.size(), &len,
                     reinterpret_cast<const uint8_t*>(data.data()),
                     data.size());

    int final_len;
    EVP_EncryptFinal_ex(ctx, output.data() + iv.size() + len, &final_len);

    size_t tag_offset = iv.size() + static_cast<size_t>(len + final_len);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16,
                        output.data() + tag_offset);
    output.resize(tag_offset + 16);

    EVP_CIPHER_CTX_free(ctx);

    return output;
}

std::vector<uint8_t> AesGcmEncryption::decrypt(
    const std::vector<uint8_t>& data) {

    if (data.size() < 12 + 16) {  // IV (12) + tag (16) minimum
        throw std::runtime_error("Invalid encrypted data");
    }

    const EVP_CIPHER* cipher = nullptr;
    if (cipher_ == Cipher::AES_128_GCM) {
        cipher = EVP_aes_128_gcm();
    } else {
        cipher = EVP_aes_256_gcm();
    }

    // Extract IV, ciphertext, and tag
    const uint8_t* iv = data.data();
    const uint8_t* ciphertext = data.data() + 12;
    size_t ciphertext_len = data.size() - 12 - 16;
    const uint8_t* tag = data.data() + data.size() - 16;

    // Initialize cipher context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, cipher, nullptr, key_.data(), iv);

    // Set expected tag
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                       const_cast<uint8_t*>(tag));

    // Decrypt
    std::vector<uint8_t> output(ciphertext_len);
    int len;
    EVP_DecryptUpdate(ctx, output.data(), &len, ciphertext, ciphertext_len);

    // Verify and finalize
    int final_len;
    int ret = EVP_DecryptFinal_ex(ctx, output.data() + len, &final_len);

    EVP_CIPHER_CTX_free(ctx);

    if (ret <= 0) {
        throw std::runtime_error("Decryption failed (tag mismatch)");
    }

    output.resize(len + final_len);
    return output;
}

std::string AesGcmEncryption::cipher_name() const {
    if (cipher_ == Cipher::AES_128_GCM) {
        return "aes-128-gcm";
    }
    return "aes-256-gcm";
}

// Factory function
std::unique_ptr<Encryption> create_encryption(
    Cipher cipher,
    std::string_view key) {

    if (cipher == Cipher::NONE) {
        return std::make_unique<NoEncryption>();
    }

    if (cipher == Cipher::AES_128_GCM || cipher == Cipher::AES_256_GCM) {
        return std::make_unique<AesGcmEncryption>(key);
    }

    return nullptr;
}

}  // namespace shield::transport
