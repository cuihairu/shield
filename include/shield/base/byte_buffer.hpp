// [SHIELD_BASE] ByteBuffer for binary data handling
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace shield::base {

/// @brief Byte buffer for handling binary data
class ByteBuffer {
public:
    // Constructors
    ByteBuffer() = default;

    explicit ByteBuffer(size_t capacity) {
        data_.resize(capacity);
        write_pos_ = 0;
    }

    ByteBuffer(const void* ptr, size_t size)
        : data_(static_cast<const uint8_t*>(ptr),
                static_cast<const uint8_t*>(ptr) + size) {}

    // Access
    const uint8_t* data() const { return data_.data(); }
    uint8_t* data() { return data_.data(); }

    size_t size() const { return data_.size(); }
    size_t capacity() const { return data_.capacity(); }
    bool empty() const { return data_.empty(); }

    // Views
    std::span<const uint8_t> span() const { return data_; }
    std::span<uint8_t> span() { return data_; }

    // Mutations
    void clear() {
        data_.clear();
        write_pos_ = 0;
    }

    void resize(size_t new_size) { data_.resize(new_size); }
    void reserve(size_t new_capacity) { data_.reserve(new_capacity); }

    // Write operations
    void write(const void* ptr, size_t size) {
        const auto* src = static_cast<const uint8_t*>(ptr);
        data_.insert(data_.end(), src, src + size);
    }

    void write_uint8(uint8_t value) { write(&value, sizeof(value)); }
    void write_uint16(uint16_t value) { write(&value, sizeof(value)); }
    void write_uint32(uint32_t value) { write(&value, sizeof(value)); }
    void write_uint64(uint64_t value) { write(&value, sizeof(value)); }

    // Read operations (with position tracking)
    uint8_t read_uint8() {
        if (read_pos_ + sizeof(uint8_t) > data_.size()) return 0;
        uint8_t value = data_[read_pos_];
        read_pos_ += sizeof(uint8_t);
        return value;
    }

    uint16_t read_uint16() {
        if (read_pos_ + sizeof(uint16_t) > data_.size()) return 0;
        uint16_t value;
        std::memcpy(&value, data_.data() + read_pos_, sizeof(uint16_t));
        read_pos_ += sizeof(uint16_t);
        return value;
    }

    uint32_t read_uint32() {
        if (read_pos_ + sizeof(uint32_t) > data_.size()) return 0;
        uint32_t value;
        std::memcpy(&value, data_.data() + read_pos_, sizeof(uint32_t));
        read_pos_ += sizeof(uint32_t);
        return value;
    }

    uint64_t read_uint64() {
        if (read_pos_ + sizeof(uint64_t) > data_.size()) return 0;
        uint64_t value;
        std::memcpy(&value, data_.data() + read_pos_, sizeof(uint64_t));
        read_pos_ += sizeof(uint64_t);
        return value;
    }

    // Position
    size_t read_position() const { return read_pos_; }
    void set_read_position(size_t pos) { read_pos_ = pos; }

    // String utilities
    std::string to_string() const {
        return std::string(reinterpret_cast<const char*>(data_.data()),
                           data_.size());
    }

    static ByteBuffer from_string(const std::string& str) {
        return ByteBuffer(str.data(), str.size());
    }

    // Hex dump for debugging
    std::string hex_dump(size_t max_bytes = 64) const {
        const char hex_chars[] = "0123456789abcdef";
        std::string result;
        size_t bytes_to_show = std::min(size(), max_bytes);

        for (size_t i = 0; i < bytes_to_show; ++i) {
            uint8_t byte = data_[i];
            result.push_back(hex_chars[(byte >> 4) & 0x0f]);
            result.push_back(hex_chars[byte & 0x0f]);
            if ((i + 1) % 16 == 0) {
                result.push_back('\n');
            } else {
                result.push_back(' ');
            }
        }

        if (size() > max_bytes) {
            result += "...";
        }
        return result;
    }

private:
    std::vector<uint8_t> data_;
    size_t read_pos_ = 0;
    size_t write_pos_ = 0;
};

}  // namespace shield::base
