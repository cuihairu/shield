// [SHIELD_TRANSPORT] Frame implementation
#include "shield/transport/frame.hpp"

#include <algorithm>
#include <cstring>

namespace shield::transport {

// FrameHeader implementation
FrameHeader FrameHeader::from_network(const uint8_t* data) {
    FrameHeader h;
    h.length = (static_cast<uint32_t>(data[0]) << 24) |
               (static_cast<uint32_t>(data[1]) << 16) |
               (static_cast<uint32_t>(data[2]) << 8) |
               static_cast<uint32_t>(data[3]);
    h.flags = (static_cast<uint16_t>(data[4]) << 8) |
              static_cast<uint16_t>(data[5]);
    h.type = (static_cast<uint16_t>(data[6]) << 8) |
             static_cast<uint16_t>(data[7]);
    return h;
}

void FrameHeader::to_network(uint8_t* data) const {
    data[0] = (length >> 24) & 0xFF;
    data[1] = (length >> 16) & 0xFF;
    data[2] = (length >> 8) & 0xFF;
    data[3] = length & 0xFF;
    data[4] = (flags >> 8) & 0xFF;
    data[5] = flags & 0xFF;
    data[6] = (type >> 8) & 0xFF;
    data[7] = type & 0xFF;
}

// Frame implementation
std::vector<uint8_t> Frame::serialize() const {
    std::vector<uint8_t> result;
    result.reserve(total_size());

    result.resize(FrameHeader::HEADER_SIZE);
    header_.to_network(result.data());

    result.insert(result.end(), payload_.begin(), payload_.end());

    return result;
}

bool Frame::parse(const uint8_t* data, size_t size) {
    if (size < FrameHeader::HEADER_SIZE) {
        return false;
    }

    header_ = FrameHeader::from_network(data);

    if (header_.length > size - FrameHeader::HEADER_SIZE) {
        return false;  // Incomplete frame
    }

    payload_.assign(data + FrameHeader::HEADER_SIZE,
                    data + FrameHeader::HEADER_SIZE + header_.length);

    return true;
}

// FrameDecoder implementation
FrameDecoder::FrameDecoder()
    : needed_(FrameHeader::HEADER_SIZE), have_header_(false) {}

std::vector<Frame> FrameDecoder::feed(const uint8_t* data, size_t size) {
    std::vector<Frame> frames;

    buffer_.insert(buffer_.end(), data, data + size);

    while (!buffer_.empty()) {
        if (!have_header_) {
            if (buffer_.size() < FrameHeader::HEADER_SIZE) {
                needed_ = FrameHeader::HEADER_SIZE - buffer_.size();
                break;
            }

            header_ = FrameHeader::from_network(buffer_.data());
            needed_ = header_.length;
            have_header_ = true;
        }

        if (buffer_.size() < FrameHeader::HEADER_SIZE + needed_) {
            break;
        }

        Frame frame;
        frame.header_ = header_;
        frame.payload_.assign(
            buffer_.begin() + FrameHeader::HEADER_SIZE,
            buffer_.begin() + FrameHeader::HEADER_SIZE + header_.length);

        frames.push_back(std::move(frame));

        buffer_.erase(buffer_.begin(),
                     buffer_.begin() + FrameHeader::HEADER_SIZE + header_.length);

        have_header_ = false;
        needed_ = FrameHeader::HEADER_SIZE;
    }

    return frames;
}

void FrameDecoder::reset() {
    buffer_.clear();
    needed_ = FrameHeader::HEADER_SIZE;
    have_header_ = false;
}

// FrameEncoder implementation
std::vector<uint8_t> FrameEncoder::encode(const Frame& frame) {
    return frame.serialize();
}

}  // namespace shield::transport
