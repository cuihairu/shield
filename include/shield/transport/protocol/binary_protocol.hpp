// [CORE]
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace shield::protocol {

class BinaryProtocol {
public:
    // Encodes a message into a byte vector with a 4-byte length prefix.
    static std::vector<char> encode(const std::string &message);

    // Decodes a message from a byte buffer.
    // Returns the decoded message and the number of bytes consumed.
    // If not enough data is available, returns an empty string and 0 bytes
    // consumed.
    // Throws std::runtime_error if the declared message size exceeds
    // max_message_size.
    static std::pair<std::string, size_t> decode(const char *buffer,
                                                 size_t length);

    // Maximum allowed message body size (16 MB by default).
    static constexpr size_t max_message_size = 16 * 1024 * 1024;

private:
    static constexpr size_t HEADER_SIZE = 4;
};

}  // namespace shield::protocol
