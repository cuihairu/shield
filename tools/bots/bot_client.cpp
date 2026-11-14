// tools/bots/bot_client.cpp - idlen framing + blocking client loop.
#include "bot_client.hpp"

#include <algorithm>
#include <boost/asio/connect.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <nlohmann/json.hpp>
#include <thread>

namespace shield::bots {

namespace {

std::pair<std::string, std::string> split_target(const std::string& target) {
    const auto colon = target.rfind(':');
    if (colon == std::string::npos) {
        return {target, "8001"};
    }
    return {target.substr(0, colon), target.substr(colon + 1)};
}

std::vector<Frame> parse_frames(std::vector<std::uint8_t>& inbox) {
    std::vector<Frame> frames;
    // idlen: 2B route + 2B length + body. Frames are extracted greedily;
    // a partially buffered tail stays in the inbox.
    while (inbox.size() >= 4) {
        const std::uint32_t route_id =
            (static_cast<std::uint32_t>(inbox[0]) << 8) | inbox[1];
        const std::size_t length =
            (static_cast<std::size_t>(inbox[2]) << 8) | inbox[3];
        if (inbox.size() < 4 + length) {
            break;
        }
        Frame frame;
        frame.route_id = route_id;
        frame.body.assign(
            inbox.begin() + 4,
            inbox.begin() + 4 + static_cast<std::ptrdiff_t>(length));
        const auto consumed = 4 + static_cast<std::ptrdiff_t>(length);
        inbox.erase(inbox.begin(), inbox.begin() + consumed);
        frames.push_back(std::move(frame));
    }
    return frames;
}

}  // namespace

std::vector<std::uint8_t> encode_frame(std::uint32_t route_id,
                                       const std::string& body) {
    std::vector<std::uint8_t> frame;
    frame.reserve(4 + body.size());
    frame.push_back(static_cast<std::uint8_t>((route_id >> 8) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(route_id & 0xFF));
    const auto length = body.size();
    frame.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(length & 0xFF));
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

BotClient::BotClient(std::string target)
    : target_(std::move(target)), socket_(io_) {}

bool BotClient::connect_with_backoff(int max_attempts) {
    const auto [host, port] = split_target(target_);
    int delay_ms = 100;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        boost::system::error_code ec;
        boost::asio::ip::tcp::resolver resolver(io_);
        const auto endpoints = resolver.resolve(host, port, ec);
        if (!ec) {
            socket_ = boost::asio::ip::tcp::socket(io_);
            boost::asio::connect(socket_, endpoints, ec);
        }
        if (!ec) {
            alive_ = true;
            return true;
        }
        if (attempt < max_attempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, 2000);
        }
    }
    return false;
}

std::optional<std::string> BotClient::login(const LoginConfig& login,
                                            std::chrono::milliseconds timeout,
                                            double* latency_ms) {
    const auto started = std::chrono::steady_clock::now();
    nlohmann::json body;
    body["player_id"] = login.player_id;
    if (!send(login.route_id, body.dump())) {
        return std::nullopt;
    }
    const auto deadline = started + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto frame =
            poll_frame(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()));
        if (!frame) {
            return std::nullopt;
        }
        if (frame->route_id == login.reply_route_id) {
            if (latency_ms != nullptr) {
                *latency_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
            }
            return frame->body;
        }
        // Other frames arriving before the login reply (s2c push) are
        // dropped — nothing can be correlated before the session is up.
    }
    return std::nullopt;
}

bool BotClient::send(std::uint32_t route_id, const std::string& body) {
    if (!alive_) {
        return false;
    }
    boost::system::error_code ec;
    const auto bytes = encode_frame(route_id, body);
    boost::asio::write(socket_, boost::asio::buffer(bytes), ec);
    if (ec) {
        alive_ = false;
        return false;
    }
    return true;
}

bool BotClient::read_some(std::chrono::milliseconds budget) {
    // Poll available() like the acceptance test's reader: bounded waits,
    // no async machinery, deterministic across platforms.
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        if (socket_.available(ec) > 0) {
            std::vector<std::uint8_t> chunk(4096);
            ec.clear();
            const auto n = socket_.read_some(boost::asio::buffer(chunk), ec);
            if (ec) {
                alive_ = false;
                return false;
            }
            inbox_.insert(inbox_.end(), chunk.begin(),
                          chunk.begin() + static_cast<std::ptrdiff_t>(n));
            for (auto& frame : parse_frames(inbox_)) {
                pending_.push_back(std::move(frame));
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;  // quiet window, not an error
}

std::optional<Frame> BotClient::poll_frame(std::chrono::milliseconds timeout) {
    if (!pending_.empty()) {
        auto frame = std::move(pending_.front());
        pending_.pop_front();
        return frame;
    }
    if (!alive_) {
        return std::nullopt;
    }
    read_some(timeout);
    if (!pending_.empty()) {
        auto frame = std::move(pending_.front());
        pending_.pop_front();
        return frame;
    }
    return std::nullopt;
}

void BotClient::close() {
    boost::system::error_code ec;
    socket_.close(ec);
    alive_ = false;
}

ProbeResult probe_hello_world(const std::string& target,
                              const std::string& player_id,
                              std::chrono::milliseconds timeout) {
    ProbeResult result;
    BotClient client(target);
    if (!client.connect_with_backoff(3)) {
        result.detail = "connect failed";
        return result;
    }
    LoginConfig login;
    login.player_id = player_id;
    auto body = client.login(login, timeout, &result.login_ms);
    if (!body) {
        result.detail = "login failed";
        return result;
    }
    const auto started = std::chrono::steady_clock::now();
    if (!client.send(2, R"({"x":7,"y":9})")) {
        result.detail = "move send failed";
        return result;
    }
    const auto deadline = started + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto frame = client.poll_frame(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()));
        if (!frame) {
            break;
        }
        if (frame->route_id == 200) {
            result.move_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
            result.ok = true;
            return result;
        }
    }
    result.detail = "no move_result";
    return result;
}

}  // namespace shield::bots
