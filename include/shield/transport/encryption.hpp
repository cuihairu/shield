// [SHIELD_TRANSPORT] Encryption interface
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shield::transport {

/// @brief Encryption cipher
enum class Cipher {
    NONE,
    AES_128_GCM,
    AES_256_GCM,
    CHACHA20_POLY1305
};

/// @brief Encryption interface
class Encryption {
public:
    virtual ~Encryption() = default;

    /// @brief Encrypt data
    virtual std::vector<uint8_t> encrypt(std::string_view data) = 0;

    /// @brief Decrypt data
    virtual std::vector<uint8_t> decrypt(const std::vector<uint8_t>& data) = 0;

    /// @brief Get cipher name
    virtual std::string cipher_name() const = 0;
};

/// @brief No encryption (pass-through)
class NoEncryption : public Encryption {
public:
    std::vector<uint8_t> encrypt(std::string_view data) override {
        return std::vector<uint8_t>(data.begin(), data.end());
    }

    std::vector<uint8_t> decrypt(const std::vector<uint8_t>& data) override {
        return data;
    }

    std::string cipher_name() const override { return "none"; }
};

/// @brief AES-GCM encryption
class AesGcmEncryption : public Encryption {
public:
    /// @brief Initialize with key
    /// @param key 128-bit or 256-bit key
    explicit AesGcmEncryption(std::string_view key);

    std::vector<uint8_t> encrypt(std::string_view data) override;
    std::vector<uint8_t> decrypt(const std::vector<uint8_t>& data) override;

    std::string cipher_name() const override;

private:
    std::vector<uint8_t> key_;
    Cipher cipher_;
};

/// @brief Create encryption by cipher type
std::unique_ptr<Encryption> create_encryption(
    Cipher cipher,
    std::string_view key);

}  // namespace shield::transport
