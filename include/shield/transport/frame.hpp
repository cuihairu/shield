// [SHIELD_TRANSPORT] Frame types
#pragma once

#include <cstdint>
#include <cstddef>
#include <utility>
#include <vector>
#include "shield/base/byte_buffer.hpp"

namespace shield::transport {

/// @brief Frame header for length-prefixed framing
struct FrameHeader {
    uint32_t length;        // Payload length (big-endian)
    uint16_t flags;         // Frame flags
    uint16_t type;          // Frame type (message, control, etc.)

    static constexpr uint32_t HEADER_SIZE = 8;

    // Convert from/to network byte order
    static FrameHeader from_network(const uint8_t* data);
    void to_network(uint8_t* data) const;
};

/// @brief Complete frame with header and payload
class Frame {
public:
    Frame() = default;

    Frame(uint16_t type, std::vector<uint8_t> payload)
        : payload_(std::move(payload)) {
        header_.length = static_cast<uint32_t>(payload_.size());
        header_.type = type;
        header_.flags = 0;
    }

    const FrameHeader& header() const { return header_; }
    const std::vector<uint8_t>& payload() const { return payload_; }
    std::vector<uint8_t>& mutable_payload() { return payload_; }

    /// @brief Serialize entire frame (header + payload)
    std::vector<uint8_t> serialize() const;

    /// @brief Parse frame from bytes
    /// @return true if frame is complete and valid
    bool parse(const uint8_t* data, size_t size);

    /// @brief Get total frame size (header + payload)
    size_t total_size() const {
        return FrameHeader::HEADER_SIZE + header_.length;
    }

private:
    FrameHeader header_;
    std::vector<uint8_t> payload_;

    friend class FrameDecoder;
};

/// @brief Frame decoder (accumulates bytes and produces frames)
class FrameDecoder {
public:
    FrameDecoder();

    /// @brief Feed data to decoder
    /// @return Vector of complete frames
    std::vector<Frame> feed(const uint8_t* data, size_t size);

    /// @brief Reset decoder state
    void reset();

private:
    std::vector<uint8_t> buffer_;
    FrameHeader header_{};
    size_t needed_ = FrameHeader::HEADER_SIZE;
    bool have_header_ = false;
};

/// @brief Frame encoder (converts frames to bytes)
class FrameEncoder {
public:
    /// @brief Encode a frame
    std::vector<uint8_t> encode(const Frame& frame);
};

}  // namespace shield::transport
