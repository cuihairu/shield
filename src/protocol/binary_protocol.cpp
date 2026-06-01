#include "shield/protocol/binary_protocol.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>  // For htonl, ntohl
#endif

#include <cstring>  // For memcpy

namespace shield::protocol {

std::vector<char> BinaryProtocol::encode(const std::string &message) {
    uint32_t message_length = static_cast<uint32_t>(message.length());
    uint32_t total_length = static_cast<uint32_t>(HEADER_SIZE) + message_length;

    std::vector<char> buffer(total_length);

    // Write total length in network byte order
    uint32_t n_total_length = htonl(total_length);
    std::memcpy(buffer.data(), &n_total_length, HEADER_SIZE);

    // Write message body
    std::memcpy(buffer.data() + HEADER_SIZE, message.data(), message_length);

    return buffer;
}

std::pair<std::string, size_t> BinaryProtocol::decode(const char *buffer,
                                                      size_t length) {
    if (length < HEADER_SIZE) {
        return {"", 0};  // Not enough data for header
    }

    uint32_t total_length;
    std::memcpy(&total_length, buffer, HEADER_SIZE);
    total_length = ntohl(total_length);

    if (total_length <= HEADER_SIZE) {
        // Guard against zero or malformed length
        return {"", 0};
    }

    uint32_t body_length = total_length - HEADER_SIZE;
    if (body_length > max_message_size) {
        throw std::runtime_error(
            "BinaryProtocol: declared message size (" +
            std::to_string(body_length) +
            " bytes) exceeds maximum allowed (" +
            std::to_string(max_message_size) + " bytes)");
    }

    if (length < total_length) {
        return {"", 0};  // Not enough data for full message
    }

    std::string message(buffer + HEADER_SIZE, body_length);
    return {message, total_length};
}

}  // namespace shield::protocol
