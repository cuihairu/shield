// tools/bots/chaos.cpp - malformed-input injection implementations.
#include "chaos.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/write.hpp>
#include <cstdarg>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

#include "bot_client.hpp"

namespace shield::bots {

namespace {

using boost::asio::ip::tcp;

std::pair<std::string, std::string> split_host_port(const std::string& target) {
    const auto colon = target.rfind(':');
    if (colon == std::string::npos) {
        return {target, "8001"};
    }
    return {target.substr(0, colon), target.substr(colon + 1)};
}

/// Hostile connection: byte-level writes, no reply reading, harsh closes.
struct RawLink {
    boost::asio::io_context io;
    tcp::socket socket{io};
    bool connected = false;

    bool connect(const std::string& target) {
        const auto [host, port] = split_host_port(target);
        boost::system::error_code ec;
        tcp::resolver resolver(io);
        const auto endpoints = resolver.resolve(host, port, ec);
        if (ec) {
            return false;
        }
        socket = tcp::socket(io);
        boost::asio::connect(socket, endpoints, ec);
        connected = !ec;
        return connected;
    }

    bool send(const std::vector<std::uint8_t>& bytes) {
        if (!connected) {
            return false;
        }
        boost::system::error_code ec;
        boost::asio::write(socket, boost::asio::buffer(bytes), ec);
        if (ec) {
            connected = false;
        }
        return !ec;
    }

    void close_reset() {
        // SO_LINGER 0: close() emits RST instead of FIN — the harshest
        // teardown a peer can deliver short of cable pull.
        if (socket.is_open()) {
            boost::system::error_code ec;
            socket.set_option(tcp::socket::linger(true, 0), ec);
            socket.close(ec);
        }
        connected = false;
    }

    void close() {
        boost::system::error_code ec;
        socket.close(ec);
        connected = false;
    }
};

std::vector<std::uint8_t> junk(std::size_t n, std::uint32_t seed) {
    std::vector<std::uint8_t> bytes(n);
    for (auto& byte : bytes) {
        seed = seed * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(seed >> 24);
    }
    return bytes;
}

void note(std::vector<std::string>& lines, const char* fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    lines.emplace_back(buffer);
}

/// The verdict function: after an injection the gateway must still serve a
/// normal login+move round trip. Connection closed / frame dropped is fine;
/// anything that breaks the probe is an anomaly.
bool survived(const ChaosOptions& options, const std::string& mode,
              ChaosReport& report, double* probe_login_ms = nullptr) {
    std::this_thread::sleep_for(options.settle);
    auto probe =
        probe_hello_world(options.target, options.player_prefix + "-probe",
                          options.probe_timeout);
    if (probe.ok) {
        if (probe_login_ms != nullptr) {
            *probe_login_ms = probe.login_ms;
        }
        note(report.lines, "survive[%s]: probe ok (login %.1fms move %.1fms)",
             mode.c_str(), probe.login_ms, probe.move_ms);
        return true;
    }
    const auto detail =
        mode + ": probe failed after injection — " + probe.detail;
    report.anomalies.push_back(detail);
    report.lines.push_back("ANOMALY[" + mode + "] " + probe.detail);
    return false;
}

ChaosReport run_bad_frame(const ChaosOptions& options) {
    ChaosReport report;
    const std::string mode = "bad_frame";
    for (int i = 0; i < options.count; ++i) {
        RawLink link;
        if (!link.connect(options.target)) {
            note(report.lines,
                 "inject[%s %d]: connect refused (listener gone?)",
                 mode.c_str(), i);
            survived(options, mode, report);
            continue;
        }
        const auto body = nlohmann::json{{"player_id", options.player_prefix +
                                                           std::to_string(i)}}
                              .dump();
        const auto whole = encode_frame(1, body);
        switch (i % 3) {
            case 0: {
                // Truncated header: 3 of the 4 header bytes, then FIN.
                link.send({whole.begin(), whole.begin() + 3});
                link.close();
                note(report.lines, "inject[%s %d]: truncated header (3B) + FIN",
                     mode.c_str(), i);
                break;
            }
            case 1: {
                // Pure garbage: no valid header anywhere.
                link.send(
                    junk(64, 0xDEADC0DEu + static_cast<std::uint32_t>(i)));
                link.close();
                note(report.lines, "inject[%s %d]: 64B garbage", mode.c_str(),
                     i);
                break;
            }
            default: {
                // Lying length: header promises 0xFFFF bytes, 10 arrive, FIN.
                std::vector<std::uint8_t> lying{0x00, 0x01, 0xFF, 0xFF};
                const auto tail = junk(10, 0xBADC0DEu);
                lying.insert(lying.end(), tail.begin(), tail.end());
                link.send(lying);
                link.close();
                note(report.lines,
                     "inject[%s %d]: header says 65535B, sends 10B, FIN",
                     mode.c_str(), i);
                break;
            }
        }
        if (!survived(options, mode, report)) {
            break;  // gateway is down; further injections prove nothing new
        }
    }
    return report;
}

ChaosReport run_oversize(const ChaosOptions& options) {
    ChaosReport report;
    const std::string mode = "oversize";
    for (int i = 0; i < options.count; ++i) {
        RawLink link;
        if (!link.connect(options.target)) {
            note(report.lines, "inject[%s %d]: connect refused", mode.c_str(),
                 i);
            survived(options, mode, report);
            continue;
        }
        if (i % 2 == 0) {
            // Max legal idlen frame: header declares 0xFFFF, all 65535 junk
            // bytes actually delivered. The json codec must reject it.
            std::vector<std::uint8_t> frame{0x00, 0x01, 0xFF, 0xFF};
            const auto body = junk(0xFFFF, 0xFEEDF00Du);
            frame.insert(frame.end(), body.begin(), body.end());
            link.send(frame);
            link.close();
            note(report.lines,
                 "inject[%s %d]: full 65535B junk body (max idlen)",
                 mode.c_str(), i);
        } else {
            // Header promises 0xFFFF, connection dies at 30000 — the reader
            // must give up instead of buffering forever.
            std::vector<std::uint8_t> frame{0x00, 0x01, 0xFF, 0xFF};
            const auto body = junk(30000, 0xCAFEBABEu);
            frame.insert(frame.end(), body.begin(), body.end());
            link.send(frame);
            link.close_reset();
            note(report.lines, "inject[%s %d]: promised 65535B, RST at 30000B",
                 mode.c_str(), i);
        }
        if (!survived(options, mode, report)) {
            break;
        }
    }
    return report;
}

ChaosReport run_route_flood(const ChaosOptions& options) {
    ChaosReport report;
    const std::string mode = "route_flood";
    for (int i = 0; i < options.count; ++i) {
        if (i % 2 == 0) {
            // Logged-in session, then a flood of never-registered route ids,
            // then the session must still answer a normal move (or the server
            // must have dropped us cleanly — probe decides).
            BotClient client(options.target);
            if (!client.connect_with_backoff(1)) {
                note(report.lines, "inject[%s %d]: connect refused",
                     mode.c_str(), i);
                survived(options, mode, report);
                continue;
            }
            LoginConfig login;
            login.player_id =
                options.player_prefix + "-flood-" + std::to_string(i);
            const auto reply =
                client.login(login, options.probe_timeout, nullptr);
            if (!reply) {
                const auto detail = mode + ": baseline login failed";
                report.anomalies.push_back(detail);
                report.lines.push_back("ANOMALY[" + mode +
                                       "] baseline login failed");
                break;
            }
            for (int n = 0; n < 200; ++n) {
                const std::uint32_t hostile =
                    (n % 3 == 0) ? 0u : (n % 3 == 1 ? 0xFFFFu : 9999u);
                if (!client.send(hostile, "{}")) {
                    break;  // server dropped us — acceptable, probe decides
                }
            }
            note(report.lines,
                 "inject[%s %d]: 200 unknown-route frames on live session",
                 mode.c_str(), i);
            if (client.alive()) {
                client.send(2, R"({"x":1,"y":2})");
            }
        } else {
            // Move-before-login: route 2 belongs to the player service, but a
            // fresh session targets auth. The gateway must not crash on the
            // mismatch however it routes it.
            BotClient client(options.target);
            if (!client.connect_with_backoff(1)) {
                note(report.lines, "inject[%s %d]: connect refused",
                     mode.c_str(), i);
                survived(options, mode, report);
                continue;
            }
            const bool move_went = client.send(2, R"({"x":1,"y":2})");
            LoginConfig login;
            login.player_id =
                options.player_prefix + "-order-" + std::to_string(i);
            const auto reply = client.login(login, options.probe_timeout);
            note(report.lines,
                 "inject[%s %d]: move-before-login (move=%s, login=%s)",
                 mode.c_str(), i, move_went ? "sent" : "failed",
                 reply ? "ok" : (client.alive() ? "timeout" : "closed"));
        }
        if (!survived(options, mode, report)) {
            break;
        }
    }
    return report;
}

ChaosReport run_disconnect_flood(const ChaosOptions& options) {
    ChaosReport report;
    const std::string mode = "disconnect_flood";
    for (int i = 0; i < options.count * 8; ++i) {
        RawLink link;
        if (!link.connect(options.target)) {
            // Refusals DURING the flood are expected backpressure; note but
            // do not fail — the verdict is the post-flood probe.
            note(report.lines, "inject[%s %d]: connect refused mid-flood",
                 mode.c_str(), i);
            continue;
        }
        switch (i % 4) {
            case 0:
                break;  // connect, immediate FIN, zero bytes
            case 1: {
                const auto whole = encode_frame(1, R"({"player_id":"x"})");
                link.send({whole.begin(), whole.begin() + 2});
                link.close();
                break;  // half header then FIN
            }
            case 2: {
                link.send(encode_frame(1, R"({"player_id":"x"})"));
                link.close();  // full request, close before reading the reply
                break;
            }
            default:
                link.close_reset();  // RST teardown
                break;
        }
    }
    note(report.lines, "inject[%s]: %d rapid connect/close cycles done",
         mode.c_str(), options.count * 8);
    // Listener health beyond the single probe: repeated refusals here mean
    // the flood exhausted the listener (fd leak / wedged accept loop).
    int refused = 0;
    for (int n = 0; n < 5; ++n) {
        RawLink link;
        if (!link.connect(options.target)) {
            ++refused;
        } else {
            link.close();
        }
    }
    if (refused >= 3) {
        const auto detail = mode + ": " + std::to_string(refused) +
                            "/5 fresh connects refused after flood";
        report.anomalies.push_back(detail);
        report.lines.push_back("ANOMALY[" + mode + "] " + detail);
    }
    survived(options, mode, report);
    return report;
}

}  // namespace

std::vector<std::string> chaos_mode_names() {
    return {"bad_frame", "oversize", "route_flood", "disconnect_flood"};
}

ChaosReport run_chaos(const std::string& mode, const ChaosOptions& options) {
    if (mode == "all") {
        ChaosReport merged;
        for (const auto& name : chaos_mode_names()) {
            auto part = run_chaos(name, options);
            merged.lines.insert(merged.lines.end(), part.lines.begin(),
                                part.lines.end());
            merged.anomalies.insert(merged.anomalies.end(),
                                    part.anomalies.begin(),
                                    part.anomalies.end());
            if (!part.clean()) {
                note(merged.lines,
                     "mode %s left the gateway unhealthy; "
                     "stopping remaining modes",
                     name.c_str());
                break;
            }
        }
        return merged;
    }
    if (mode == "bad_frame") {
        return run_bad_frame(options);
    }
    if (mode == "oversize") {
        return run_oversize(options);
    }
    if (mode == "route_flood") {
        return run_route_flood(options);
    }
    if (mode == "disconnect_flood") {
        return run_disconnect_flood(options);
    }
    throw std::runtime_error("unknown chaos mode: " + mode + " (expected all|" +
                             "bad_frame|oversize|"
                             "route_flood|disconnect_flood)");
}

}  // namespace shield::bots
