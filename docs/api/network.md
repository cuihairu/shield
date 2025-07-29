# Network 网络模块 API 文档

网络模块是 Shield 框架的核心网络层，提供高性能的异步 I/O 和连接管理功能。基于 Boost.Asio 实现的 Main-Sub Reactor 架构。

## 📋 模块概览

网络模块包含以下主要类：

- `MasterReactor`: 主 Reactor，负责连接接受
- `SlaveReactor`: 从 Reactor，负责 I/O 处理
- `Session`: 网络会话管理
- `TCPServer`: TCP 服务器封装

## 🌐 MasterReactor 主 Reactor

主 Reactor 负责监听和接受客户端连接，并将连接分发给从 Reactor 处理。

### 类定义

```cpp
namespace shield::net {

struct MasterReactorConfig {
    std::string host = "0.0.0.0";      // 监听地址
    uint16_t port = 8080;              // 监听端口
    int backlog = 1024;                // 监听队列长度
    bool reuse_addr = true;            // 地址重用
    bool reuse_port = false;           // 端口重用
    size_t slave_count = 4;            // 从 Reactor 数量
};

class MasterReactor : public core::Component {
public:
    explicit MasterReactor(const MasterReactorConfig& config);
    virtual ~MasterReactor();
    
    // 组件生命周期
    void on_init() override;
    void on_start() override;
    void on_stop() override;
    
    // Reactor 管理
    void add_slave_reactor(std::unique_ptr<SlaveReactor> slave);
    SlaveReactor* get_slave_reactor(size_t index);
    size_t slave_reactor_count() const;
    
    // 连接管理
    void set_session_factory(std::unique_ptr<ISessionFactory> factory);
    void set_connection_handler(std::function<void(std::shared_ptr<Session>)> handler);
    
    // 统计信息
    struct Statistics {
        std::atomic<uint64_t> total_connections{0};
        std::atomic<uint64_t> active_connections{0};
        std::atomic<uint64_t> accepted_connections{0};
        std::atomic<uint64_t> rejected_connections{0};
    };
    
    const Statistics& get_statistics() const;
    void reset_statistics();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::net
```

### 使用示例

```cpp
// 创建主 Reactor 配置
shield::net::MasterReactorConfig config;
config.host = "0.0.0.0";
config.port = 8080;
config.backlog = 1024;
config.slave_count = 8;  // 8 个工作线程

// 创建主 Reactor
auto master_reactor = std::make_unique<shield::net::MasterReactor>(config);

// 创建从 Reactor 池
for (size_t i = 0; i < config.slave_count; ++i) {
    auto slave = std::make_unique<shield::net::SlaveReactor>(i);
    master_reactor->add_slave_reactor(std::move(slave));
}

// 设置会话工厂
auto session_factory = std::make_unique<GameSessionFactory>();
master_reactor->set_session_factory(std::move(session_factory));

// 设置连接处理器
master_reactor->set_connection_handler([](std::shared_ptr<Session> session) {
    SHIELD_LOG_INFO << "新连接: " << session->get_remote_endpoint();
    session->start();
});

// 启动 Reactor
master_reactor->init();
master_reactor->start();

// 获取统计信息
auto stats = master_reactor->get_statistics();
SHIELD_LOG_INFO << "总连接数: " << stats.total_connections.load();
SHIELD_LOG_INFO << "活跃连接: " << stats.active_connections.load();
```

## ⚡ SlaveReactor 从 Reactor

从 Reactor 在独立线程中处理具体的 I/O 操作。

### 类定义

```cpp
namespace shield::net {

class SlaveReactor : public core::Component {
public:
    explicit SlaveReactor(size_t reactor_id);
    virtual ~SlaveReactor();
    
    // 组件生命周期
    void on_init() override;
    void on_start() override;
    void on_stop() override;
    
    // 会话管理
    void add_session(std::shared_ptr<Session> session);
    void remove_session(uint64_t session_id);
    std::shared_ptr<Session> find_session(uint64_t session_id);
    
    // I/O 上下文访问
    boost::asio::io_context& get_io_context();
    
    // 线程安全的任务投递
    template<typename Function>
    void post(Function&& func);
    
    template<typename Function>
    void dispatch(Function&& func);
    
    // 统计信息
    struct Statistics {
        std::atomic<size_t> session_count{0};
        std::atomic<uint64_t> bytes_received{0};
        std::atomic<uint64_t> bytes_sent{0};
        std::atomic<uint64_t> messages_received{0};
        std::atomic<uint64_t> messages_sent{0};
    };
    
    const Statistics& get_statistics() const;
    size_t get_reactor_id() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::net
```

### 使用示例

```cpp
// 创建从 Reactor
auto slave_reactor = std::make_unique<shield::net::SlaveReactor>(0);

// 启动 Reactor
slave_reactor->init();
slave_reactor->start();

// 添加会话到 Reactor
auto session = std::make_shared<GameSession>(socket);
slave_reactor->add_session(session);

// 投递异步任务
slave_reactor->post([session]() {
    // 在 Reactor 线程中执行
    session->send_welcome_message();
});

// 查找会话
auto found_session = slave_reactor->find_session(session_id);
if (found_session) {
    SHIELD_LOG_INFO << "找到会话: " << session_id;
}

// 获取统计信息
auto stats = slave_reactor->get_statistics();
SHIELD_LOG_INFO << "会话数量: " << stats.session_count.load();
SHIELD_LOG_INFO << "收到字节: " << stats.bytes_received.load();
SHIELD_LOG_INFO << "发送字节: " << stats.bytes_sent.load();
```

## 💬 Session 会话管理

Session 类管理单个客户端连接的生命周期和数据传输。

### 类定义

```cpp
namespace shield::net {

enum class SessionState {
    CREATED,        // 已创建
    CONNECTING,     // 连接中
    CONNECTED,      // 已连接
    DISCONNECTING,  // 断开连接中
    DISCONNECTED    // 已断开
};

struct SessionConfig {
    size_t receive_buffer_size = 8192;      // 接收缓冲区大小
    size_t send_buffer_size = 8192;         // 发送缓冲区大小
    std::chrono::seconds idle_timeout{300}; // 空闲超时
    bool enable_keepalive = true;           // 是否启用 keepalive
    bool enable_nodelay = true;             // 是否禁用 Nagle 算法
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(boost::asio::ip::tcp::socket socket, 
            const SessionConfig& config = SessionConfig{});
    virtual ~Session();
    
    // 会话控制
    void start();
    void stop();
    void close();
    
    // 状态查询
    SessionState get_state() const;
    bool is_connected() const;
    uint64_t get_session_id() const;
    
    // 网络信息
    std::string get_local_endpoint() const;
    std::string get_remote_endpoint() const;
    boost::asio::ip::tcp::socket& get_socket();
    
    // 数据发送
    void send(const std::string& data);
    void send(const std::vector<uint8_t>& data);
    void send(const void* data, size_t size);
    
    // 异步发送
    void async_send(const std::string& data, 
                   std::function<void(boost::system::error_code, size_t)> callback = nullptr);
    
    // 事件回调设置
    void set_message_handler(std::function<void(const std::vector<uint8_t>&)> handler);
    void set_connect_handler(std::function<void()> handler);
    void set_disconnect_handler(std::function<void()> handler);
    void set_error_handler(std::function<void(const boost::system::error_code&)> handler);
    
    // 统计信息
    struct Statistics {
        std::atomic<uint64_t> bytes_received{0};
        std::atomic<uint64_t> bytes_sent{0};
        std::atomic<uint64_t> messages_received{0};
        std::atomic<uint64_t> messages_sent{0};
        std::chrono::system_clock::time_point connect_time;
        std::chrono::system_clock::time_point last_activity;
    };
    
    const Statistics& get_statistics() const;

protected:
    // 子类可重写的虚函数
    virtual void on_connect();
    virtual void on_disconnect();
    virtual void on_message(const std::vector<uint8_t>& data);
    virtual void on_error(const boost::system::error_code& error);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// 会话工厂接口
class ISessionFactory {
public:
    virtual ~ISessionFactory() = default;
    virtual std::shared_ptr<Session> create_session(boost::asio::ip::tcp::socket socket) = 0;
};

} // namespace shield::net
```

### 使用示例

```cpp
// 自定义游戏会话类
class GameSession : public shield::net::Session {
public:
    GameSession(boost::asio::ip::tcp::socket socket) 
        : Session(std::move(socket)) {}

protected:
    void on_connect() override {
        SHIELD_LOG_INFO << "玩家连接: " << get_remote_endpoint();
        
        // 发送欢迎消息
        nlohmann::json welcome = {
            {"type", "welcome"},
            {"server", "Shield Game Server"},
            {"version", "1.0"}
        };
        send(welcome.dump());
    }
    
    void on_message(const std::vector<uint8_t>& data) override {
        try {
            // 解析 JSON 消息
            std::string message_str(data.begin(), data.end());
            auto json_msg = nlohmann::json::parse(message_str);
            
            std::string msg_type = json_msg["type"];
            
            if (msg_type == "ping") {
                handle_ping(json_msg);
            } else if (msg_type == "game_action") {
                handle_game_action(json_msg);
            } else {
                SHIELD_LOG_WARN << "未知消息类型: " << msg_type;
            }
            
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "消息解析失败: " << e.what();
        }
    }
    
    void on_disconnect() override {
        SHIELD_LOG_INFO << "玩家断开: " << get_remote_endpoint();
        // 清理玩家资源...
    }
    
    void on_error(const boost::system::error_code& error) override {
        SHIELD_LOG_ERROR << "会话错误: " << error.message();
    }

private:
    void handle_ping(const nlohmann::json& msg) {
        nlohmann::json pong = {
            {"type", "pong"},
            {"timestamp", std::time(nullptr)}
        };
        send(pong.dump());
    }
    
    void handle_game_action(const nlohmann::json& msg) {
        // 处理游戏动作...
        std::string action = msg["action"];
        SHIELD_LOG_INFO << "游戏动作: " << action;
    }
};

// 游戏会话工厂
class GameSessionFactory : public shield::net::ISessionFactory {
public:
    std::shared_ptr<Session> create_session(boost::asio::ip::tcp::socket socket) override {
        return std::make_shared<GameSession>(std::move(socket));
    }
};

// 使用示例
auto session_factory = std::make_unique<GameSessionFactory>();
master_reactor->set_session_factory(std::move(session_factory));
```

## 🖥️ TCPServer TCP 服务器

TCPServer 是对 MasterReactor 和 SlaveReactor 的高级封装。

### 类定义

```cpp
namespace shield::net {

struct TCPServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    size_t worker_threads = 4;
    size_t max_connections = 10000;
    std::chrono::seconds session_timeout{300};
    SessionConfig session_config;
};

class TCPServer : public core::Component {
public:
    explicit TCPServer(const TCPServerConfig& config);
    virtual ~TCPServer();
    
    // 组件生命周期
    void on_init() override;
    void on_start() override;
    void on_stop() override;
    
    // 服务器控制
    void set_session_factory(std::unique_ptr<ISessionFactory> factory);
    void set_message_handler(std::function<void(std::shared_ptr<Session>, const std::vector<uint8_t>&)> handler);
    
    // 连接管理
    std::vector<std::shared_ptr<Session>> get_all_sessions();
    std::shared_ptr<Session> find_session(uint64_t session_id);
    void close_session(uint64_t session_id);
    void broadcast(const std::string& message);
    
    // 服务器信息
    std::string get_listen_address() const;
    uint16_t get_listen_port() const;
    
    // 统计信息
    struct Statistics {
        std::atomic<size_t> current_connections{0};
        std::atomic<uint64_t> total_connections{0};
        std::atomic<uint64_t> total_bytes_received{0};
        std::atomic<uint64_t> total_bytes_sent{0};
        std::atomic<uint64_t> total_messages{0};
    };
    
    const Statistics& get_statistics() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::net
```

### 使用示例

```cpp
// 创建 TCP 服务器
shield::net::TCPServerConfig config;
config.host = "0.0.0.0";
config.port = 8080;
config.worker_threads = 8;
config.max_connections = 50000;

auto tcp_server = std::make_unique<shield::net::TCPServer>(config);

// 设置会话工厂
auto session_factory = std::make_unique<GameSessionFactory>();
tcp_server->set_session_factory(std::move(session_factory));

// 设置消息处理器
tcp_server->set_message_handler([](std::shared_ptr<Session> session, const std::vector<uint8_t>& data) {
    // 处理客户端消息
    std::string message(data.begin(), data.end());
    SHIELD_LOG_INFO << "收到消息: " << message;
    
    // 回应客户端
    session->send("ACK: " + message);
});

// 启动服务器
tcp_server->init();
tcp_server->start();

SHIELD_LOG_INFO << "TCP 服务器启动完成: " 
                << tcp_server->get_listen_address() << ":" 
                << tcp_server->get_listen_port();

// 广播消息给所有连接
tcp_server->broadcast("服务器公告: 欢迎来到游戏世界！");

// 获取服务器统计信息
auto stats = tcp_server->get_statistics();
SHIELD_LOG_INFO << "当前连接数: " << stats.current_connections.load();
SHIELD_LOG_INFO << "总连接数: " << stats.total_connections.load();
```

## 🧪 测试示例

### 单元测试

```cpp
#define BOOST_TEST_MODULE NetworkTest
#include <boost/test/unit_test.hpp>
#include "shield/net/tcp_server.hpp"

BOOST_AUTO_TEST_SUITE(NetworkTest)

BOOST_AUTO_TEST_CASE(test_tcp_server_startup) {
    // 创建测试服务器
    shield::net::TCPServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;  // 使用随机端口
    
    auto server = std::make_unique<shield::net::TCPServer>(config);
    
    // 设置简单的会话工厂
    class TestSessionFactory : public shield::net::ISessionFactory {
    public:
        std::shared_ptr<shield::net::Session> create_session(
            boost::asio::ip::tcp::socket socket) override {
            return std::make_shared<shield::net::Session>(std::move(socket));
        }
    };
    
    auto factory = std::make_unique<TestSessionFactory>();
    server->set_session_factory(std::move(factory));
    
    // 启动服务器
    server->init();
    server->start();
    
    // 验证服务器启动成功
    BOOST_CHECK(server->get_listen_port() > 0);
    BOOST_CHECK_EQUAL(server->get_listen_address(), "127.0.0.1");
    
    // 停止服务器
    server->stop();
}

BOOST_AUTO_TEST_CASE(test_session_lifecycle) {
    // 模拟 socket
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket(io_context);
    
    // 创建会话
    auto session = std::make_shared<shield::net::Session>(std::move(socket));
    
    // 验证初始状态
    BOOST_CHECK_EQUAL(session->get_state(), shield::net::SessionState::CREATED);
    BOOST_CHECK(!session->is_connected());
    BOOST_CHECK(session->get_session_id() > 0);
}

BOOST_AUTO_TEST_SUITE_END()
```

### 集成测试

```cpp
BOOST_AUTO_TEST_SUITE(NetworkIntegrationTest)

BOOST_AUTO_TEST_CASE(test_client_server_communication) {
    // 启动测试服务器
    shield::net::TCPServerConfig server_config;
    server_config.host = "127.0.0.1";
    server_config.port = 0;
    
    auto server = std::make_unique<shield::net::TCPServer>(server_config);
    
    // 设置回应处理器
    std::string received_message;
    server->set_message_handler([&](std::shared_ptr<shield::net::Session> session, 
                                   const std::vector<uint8_t>& data) {
        received_message = std::string(data.begin(), data.end());
        session->send("echo: " + received_message);
    });
    
    server->init();
    server->start();
    
    // 获取实际端口
    uint16_t server_port = server->get_listen_port();
    
    // 创建客户端连接
    boost::asio::io_context client_io;
    boost::asio::ip::tcp::socket client_socket(client_io);
    
    boost::asio::ip::tcp::endpoint endpoint(
        boost::asio::ip::address::from_string("127.0.0.1"), server_port);
    
    client_socket.connect(endpoint);
    
    // 发送测试消息
    std::string test_message = "Hello, Shield!";
    boost::asio::write(client_socket, boost::asio::buffer(test_message));
    
    // 接收回应
    char reply_buffer[1024];
    size_t reply_length = client_socket.read_some(boost::asio::buffer(reply_buffer));
    std::string reply(reply_buffer, reply_length);
    
    // 验证通信结果
    BOOST_CHECK_EQUAL(received_message, test_message);
    BOOST_CHECK_EQUAL(reply, "echo: " + test_message);
    
    // 清理
    client_socket.close();
    server->stop();
}

BOOST_AUTO_TEST_SUITE_END()
```

## 📚 最佳实践

### 1. 线程模型设计

```cpp
// ✅ 好的线程配置
shield::net::TCPServerConfig config;
config.worker_threads = std::thread::hardware_concurrency();  // CPU 核心数

// 为 CPU 密集型任务分配更多线程
if (is_cpu_intensive_workload) {
    config.worker_threads = std::thread::hardware_concurrency() * 2;
}

// ❌ 不好的线程配置
config.worker_threads = 100;  // 过多线程导致上下文切换开销
```

### 2. 内存管理

```cpp
// ✅ 好的内存管理
class EfficientSession : public shield::net::Session {
private:
    // 重用缓冲区
    std::vector<uint8_t> m_receive_buffer;
    std::vector<uint8_t> m_send_buffer;
    
    void on_message(const std::vector<uint8_t>& data) override {
        // 重用缓冲区而不是频繁分配
        m_receive_buffer.clear();
        m_receive_buffer.reserve(data.size());
        m_receive_buffer.assign(data.begin(), data.end());
        
        // 处理消息...
    }
};

// ❌ 不好的内存管理
class InefficientSession : public shield::net::Session {
    void on_message(const std::vector<uint8_t>& data) override {
        // 频繁分配新内存
        auto* buffer = new uint8_t[data.size()];  // 内存泄漏风险
        std::memcpy(buffer, data.data(), data.size());
        // 忘记释放内存...
    }
};
```

### 3. 错误处理

```cpp
// ✅ 好的错误处理
class RobustSession : public shield::net::Session {
protected:
    void on_error(const boost::system::error_code& error) override {
        if (error == boost::asio::error::eof) {
            SHIELD_LOG_INFO << "客户端正常断开连接";
        } else if (error == boost::asio::error::connection_reset) {
            SHIELD_LOG_WARN << "连接被重置: " << get_remote_endpoint();
        } else {
            SHIELD_LOG_ERROR << "网络错误: " << error.message();
        }
        
        // 清理资源
        cleanup_resources();
    }
    
private:
    void cleanup_resources() {
        // 清理会话相关资源
        // 通知业务逻辑层
        // 更新统计信息
    }
};

// ❌ 不好的错误处理
class BadSession : public shield::net::Session {
protected:
    void on_error(const boost::system::error_code& error) override {
        // 忽略错误！这会导致资源泄漏
    }
};
```

### 4. 性能优化

```cpp
// ✅ 性能优化技巧

// 1. 批量发送消息
class OptimizedSession : public shield::net::Session {
private:
    std::vector<std::string> m_pending_messages;
    std::mutex m_send_mutex;
    
public:
    void queue_message(const std::string& message) {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        m_pending_messages.push_back(message);
        
        // 批量发送
        if (m_pending_messages.size() >= 10) {
            flush_pending_messages();
        }
    }
    
private:
    void flush_pending_messages() {
        if (m_pending_messages.empty()) return;
        
        // 合并所有消息
        std::string combined_message;
        for (const auto& msg : m_pending_messages) {
            combined_message += msg + "\n";
        }
        
        send(combined_message);
        m_pending_messages.clear();
    }
};

// 2. 零拷贝发送
void zero_copy_send(std::shared_ptr<Session> session, std::shared_ptr<std::string> data) {
    // 使用 shared_ptr 避免拷贝
    session->async_send(*data, [data](boost::system::error_code ec, size_t bytes) {
        // data 的生命周期由 shared_ptr 管理
        if (ec) {
            SHIELD_LOG_ERROR << "发送失败: " << ec.message();
        }
    });
}
```

---

网络模块是 Shield 框架的基础，提供了高性能、可扩展的网络通信能力。合理使用这些 API 可以构建出处理大量并发连接的高性能游戏服务器。