#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace shield::protocol {

class BinaryProtocol {
public:
    // Encodes a message into a byte vector with a 4-byte length prefix.
    static std::vector<char> encode(const std::string& message);

    // Decodes a message from a byte buffer.
    // Returns the decoded message and the number of bytes consumed.
    // If not enough data is available, returns an empty string and 0 bytes consumed.
    static std::pair<std::string, size_t> decode(const char* buffer, size_t length);

private:
    static const size_t HEADER_SIZE = 4;
};

} // namespace shield::protocol
