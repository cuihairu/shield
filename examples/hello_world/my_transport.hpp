// MyTransport - 自定义传输层示例 (spec)
// 处理原始字节流：私有二进制帧、加密握手等
// 解码后的消息路由到 Lua gateway，不在此处做业务逻辑

#pragma once

#include "shield/net/transport.hpp"

class MyTransport : public shield::Transport {
public:
    // 收到原始网络数据
    void on_raw_data(shield::Connection& conn, const char* data, size_t len) override {
        // 示例：自定义二进制帧 [2字节长度][payload]
        // 实际场景可能涉及解密、解压、自定义帧格式
        while (len >= 2) {
            uint16_t payload_len = static_cast<uint8_t>(data[0]) << 8
                                 | static_cast<uint8_t>(data[1]);
            if (len < 2 + payload_len) break;

            // 解码后交给 Lua gateway 处理
            dispatch_to_lua(conn, data + 2, payload_len);

            data += 2 + payload_len;
            len -= 2 + payload_len;
        }
    }

    // 发送数据前的编码
    void on_encode(shield::Connection& conn, const char* data, size_t len,
                   shield::Buffer& out) override {
        // 加上 2 字节长度头
        uint16_t header = static_cast<uint16_t>(len);
        out.push_back(static_cast<char>((header >> 8) & 0xFF));
        out.push_back(static_cast<char>(header & 0xFF));
        out.append(data, len);
    }
};
