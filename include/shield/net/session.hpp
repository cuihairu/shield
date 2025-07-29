#pragma once

#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <atomic>

namespace shield::net {

class Session : public std::enable_shared_from_this<Session> {
public:
    using ReadCallback = std::function<void(const char*, size_t)>;
    using CloseCallback = std::function<void()>;

    explicit Session(boost::asio::ip::tcp::socket socket);

    void start();
    void close();

    void on_read(ReadCallback callback) { m_read_callback = std::move(callback); }
    void on_close(CloseCallback callback) { m_close_callback = std::move(callback); }

    uint64_t id() const { return m_id; }

    void send(const char* data, size_t length);

private:
    void do_read();
    void do_write();

    boost::asio::ip::tcp::socket m_socket;
    enum { max_length = 8192 };
    char m_read_buffer[max_length];
    std::vector<char> m_write_buffer;

    ReadCallback m_read_callback;
    CloseCallback m_close_callback;

    uint64_t m_id;
    static std::atomic<uint64_t> s_next_id;
}; 

} // namespace shield::net
