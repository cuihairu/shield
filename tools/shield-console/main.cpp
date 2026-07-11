// shield-console: Interactive diagnostics CLI client
//
// Connects to a shield server's Unix domain socket console and provides
// an interactive REPL with line editing, history, and dynamic prompt.
//
// Usage: shield-console [--sock /tmp/shield-console.sock]

#include <replxx.hxx>

#include <boost/asio.hpp>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

namespace {

struct ConsoleClient {
    boost::asio::io_context io;
    boost::asio::local::stream_protocol::socket socket;
    boost::asio::streambuf read_buf;
    std::string socket_path;
    bool attached = false;
    std::string attached_service;

    ConsoleClient(const std::string& path)
        : socket(io), socket_path(path) {}

    bool connect() {
        try {
            boost::asio::local::stream_protocol::endpoint ep(socket_path);
            socket.connect(ep);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Failed to connect to " << socket_path << ": "
                      << e.what() << std::endl;
            return false;
        }
    }

    void send_line(const std::string& line) {
        boost::asio::write(socket, boost::asio::buffer(line + "\n"));
    }

    // Read one JSON line from the server. Returns empty string on error/EOF.
    std::string recv_line() {
        boost::system::error_code ec;
        std::size_t n = boost::asio::read_until(socket, read_buf, '\n', ec);
        if (ec) return "";
        auto bufs = read_buf.data();
        std::string data(boost::asio::buffers_begin(bufs),
                         boost::asio::buffers_begin(bufs) + n);
        read_buf.consume(n);
        // Strip trailing \n
        if (!data.empty() && data.back() == '\n') data.pop_back();
        if (!data.empty() && data.back() == '\r') data.pop_back();
        return data;
    }

    // Send a command and process all responses until the next prompt-worthy
    // response. Returns false if the connection was lost.
    bool send_and_receive(const std::string& line) {
        send_line(line);

        // Process responses
        while (true) {
            std::string resp_str = recv_line();
            if (resp_str.empty()) {
                std::cerr << "Connection lost." << std::endl;
                return false;
            }

            nlohmann::json resp;
            try {
                resp = nlohmann::json::parse(resp_str);
            } catch (...) {
                // Non-JSON output (e.g. raw print from Lua)
                std::cout << resp_str << std::endl;
                continue;
            }

            std::string type = resp.value("type", "");

            if (type == "attached") {
                attached = true;
                attached_service = resp.value("service", "");
                // Don't print anything, prompt will change
                return true;
            }

            if (type == "detached") {
                attached = false;
                attached_service.clear();
                return true;
            }

            if (type == "continue") {
                // Multiline input, more needed
                return true;
            }

            if (type == "result") {
                if (resp.contains("data")) {
                    const auto& data = resp["data"];
                    if (data.is_null()) {
                        // No output for nil result
                    } else if (data.is_string()) {
                        std::cout << data.get<std::string>() << std::endl;
                    } else if (data.is_array() && resp.contains("lines")) {
                        // Help-style output with "lines" key
                        for (const auto& line : resp["lines"]) {
                            std::cout << line.get<std::string>() << std::endl;
                        }
                    } else {
                        std::cout << data.dump(2) << std::endl;
                    }
                }
                if (resp.contains("lines")) {
                    for (const auto& line : resp["lines"]) {
                        std::cout << line.get<std::string>() << std::endl;
                    }
                }
                return true;
            }

            if (type == "output") {
                // Lua print() output
                if (resp.contains("text")) {
                    std::cout << resp["text"].get<std::string>() << std::endl;
                }
                continue;  // More responses may follow
            }

            if (type == "error") {
                std::cerr << "Error: " << resp.value("message", "unknown")
                          << std::endl;
                return true;
            }

            // Unknown type, print raw
            std::cout << resp_str << std::endl;
            return true;
        }
    }

    std::string get_prompt() const {
        if (attached) {
            return "lua:" + attached_service + "> ";
        }
        return "shield> ";
    }

    void disconnect() {
        boost::system::error_code ec;
        socket.close(ec);
    }
};

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [--sock <path>]" << std::endl;
    std::cerr << "  --sock  Path to console Unix socket "
              << "(default: /tmp/shield-console.sock)" << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string sock_path = "/tmp/shield-console.sock";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--sock" && i + 1 < argc) {
            sock_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    ConsoleClient client(sock_path);
    if (!client.connect()) {
        return 1;
    }

    std::cout << "Connected to shield console (" << sock_path << ")"
              << std::endl;
    std::cout << "Type 'help' for available commands, 'exit' to quit."
              << std::endl;

    // Set up replxx
    replxx::Replxx rx;
    rx.install_window_change_handler();

    // Main REPL loop
    while (true) {
        std::string prompt = client.get_prompt();
        const char* line_cstr = rx.input(prompt);
        if (!line_cstr) {
            // EOF (Ctrl+D)
            break;
        }

        std::string line(line_cstr);
        if (line.empty()) continue;

        // Add to history
        rx.history_add(line);

        // Handle local exit
        if (!client.attached &&
            (line == "exit" || line == "quit" || line == "q")) {
            break;
        }

        // Send to server and process response
        if (!client.send_and_receive(line)) {
            break;
        }
    }

    client.disconnect();
    return 0;
}
