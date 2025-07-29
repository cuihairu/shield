#include "shield/protocol/binary_protocol.hpp"

#include <arpa/inet.h>  // For htonl, ntohl

namespace shield::protocol {

std::vector<char> BinaryProtocol::encode(const std::string& message) {
    uint32_t message_length = message.length();
    uint32_t total_length = HEADER_SIZE + message_length;

    std::vector<char> buffer(total_length);

    // Write total length in network byte order
    uint32_t n_total_length = htonl(total_length);
    std::memcpy(buffer.data(), &n_total_length, HEADER_SIZE);

    // Write message body
    std::memcpy(buffer.data() + HEADER_SIZE, message.data(), message_length);

    return buffer;
}

std::pair<std::string, size_t> BinaryProtocol::decode(const char* buffer,
                                                      size_t length) {
    if (length < HEADER_SIZE) {
        return {"", 0};  // Not enough data for header
    }

    uint32_t total_length;
    std::memcpy(&total_length, buffer, HEADER_SIZE);
    total_length = ntohl(total_length);

    if (length < total_length) {
        return {"", 0};  // Not enough data for full message
    }

    std::string message(buffer + HEADER_SIZE, total_length - HEADER_SIZE);
    return {message, total_length};
}

}  // namespace shield::protocol
