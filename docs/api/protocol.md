# Protocol 协议模块 API 文档

协议模块负责处理各种网络协议的解析、处理和响应，支持 HTTP、WebSocket 和自定义 TCP 协议。提供统一的协议处理接口和灵活的扩展机制。

## 📋 模块概览

协议模块包含以下主要类：

- `IProtocolHandler`: 协议处理器接口
- `HttpProtocolHandler`: HTTP 协议处理器
- `WebSocketProtocolHandler`: WebSocket 协议处理器
- `ProtocolRouter`: 协议路由器

## 🌐 IProtocolHandler 协议处理器接口

所有协议处理器的基础接口，定义了统一的协议处理规范。

### 接口定义

```cpp
namespace shield::protocol {

enum class ProtocolType {
    TCP,        // 原始 TCP
    HTTP,       // HTTP/1.1
    WEBSOCKET,  // WebSocket
    CUSTOM      // 自定义协议
};

struct ProtocolMessage {
    ProtocolType type;                                      // 协议类型
    std::string method;                                     // 方法/动作
    std::string path;                                       // 路径/目标
    std::unordered_map<std::string, std::string> headers;  // 头部字段
    std::vector<uint8_t> body;                             // 消息体
    std::chrono::system_clock::time_point timestamp;       // 时间戳
    
    // 便捷方法
    std::string get_header(const std::string& name, const std::string& default_value = "") const;
    void set_header(const std::string& name, const std::string& value);
    std::string get_body_as_string() const;
    void set_body(const std::string& data);
};

struct ProtocolResponse {
    int status_code = 200;                                  // 状态码
    std::string status_message = "OK";                     // 状态消息
    std::unordered_map<std::string, std::string> headers;  // 响应头
    std::vector<uint8_t> body;                             // 响应体
    
    // 便捷方法
    void set_header(const std::string& name, const std::string& value);
    void set_body(const std::string& data);
    void set_json_body(const nlohmann::json& json);
};

class IProtocolHandler {
public:
    virtual ~IProtocolHandler() = default;
    
    // 协议信息
    virtual ProtocolType get_protocol_type() const = 0;
    virtual std::string get_protocol_name() const = 0;
    
    // 消息处理
    virtual bool can_handle(const std::vector<uint8_t>& data) = 0;
    virtual std::optional<ProtocolMessage> parse_message(const std::vector<uint8_t>& data) = 0;
    virtual std::vector<uint8_t> serialize_response(const ProtocolResponse& response) = 0;
    
    // 异步处理接口
    virtual void handle_message_async(
        const ProtocolMessage& message,
        std::function<void(const ProtocolResponse&)> callback
    ) = 0;
    
    // 生命周期
    virtual void initialize() {}
    virtual void shutdown() {}
};

} // namespace shield::protocol
```

## 🌍 HttpProtocolHandler HTTP 协议处理器

处理 HTTP/1.1 协议的请求和响应。

### 类定义

```cpp
namespace shield::protocol {

struct HttpHandlerConfig {
    size_t max_request_size = 1024 * 1024;        // 最大请求大小 (1MB)
    size_t max_header_count = 100;                // 最大头部数量
    std::chrono::seconds request_timeout{30};     // 请求超时
    bool enable_keep_alive = true;                // 启用 Keep-Alive
    bool enable_compression = false;              // 启用压缩
    std::string server_name = "Shield/1.0";       // 服务器名称
};

// HTTP 路由处理器
using HttpRouteHandler = std::function<void(const ProtocolMessage&, std::function<void(const ProtocolResponse&)>)>;

class HttpProtocolHandler : public IProtocolHandler {
public:
    explicit HttpProtocolHandler(const HttpHandlerConfig& config = HttpHandlerConfig{});
    virtual ~HttpProtocolHandler();
    
    // IProtocolHandler 接口实现
    ProtocolType get_protocol_type() const override;
    std::string get_protocol_name() const override;
    bool can_handle(const std::vector<uint8_t>& data) override;
    std::optional<ProtocolMessage> parse_message(const std::vector<uint8_t>& data) override;
    std::vector<uint8_t> serialize_response(const ProtocolResponse& response) override;
    void handle_message_async(const ProtocolMessage& message, 
                             std::function<void(const ProtocolResponse&)> callback) override;
    
    // 路由管理
    void add_route(const std::string& method, const std::string& path, HttpRouteHandler handler);
    void add_get_route(const std::string& path, HttpRouteHandler handler);
    void add_post_route(const std::string& path, HttpRouteHandler handler);
    void add_put_route(const std::string& path, HttpRouteHandler handler);
    void add_delete_route(const std::string& path, HttpRouteHandler handler);
    
    // 中间件支持
    using Middleware = std::function<bool(const ProtocolMessage&, ProtocolResponse&)>;
    void add_middleware(Middleware middleware);
    
    // 静态文件服务
    void set_static_file_handler(const std::string& root_path);
    void add_static_route(const std::string& route_prefix, const std::string& file_path);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::protocol
```

### 使用示例

```cpp
// 创建 HTTP 处理器
shield::protocol::HttpHandlerConfig config;
config.max_request_size = 2 * 1024 * 1024;  // 2MB
config.enable_compression = true;
config.server_name = "Shield Game Server/1.0";

auto http_handler = std::make_unique<shield::protocol::HttpProtocolHandler>(config);

// 添加 API 路由
http_handler->add_get_route("/api/health", [](const auto& request, auto callback) {
    shield::protocol::ProtocolResponse response;
    response.set_json_body({
        {"status", "ok"},
        {"timestamp", std::time(nullptr)},
        {"server", "shield"}
    });
    callback(response);
});

http_handler->add_post_route("/api/login", [](const auto& request, auto callback) {
    try {
        // 解析登录请求
        auto json_body = nlohmann::json::parse(request.get_body_as_string());
        std::string username = json_body["username"];
        std::string password = json_body["password"];
        
        // 验证用户凭据
        if (validate_user(username, password)) {
            std::string token = generate_jwt_token(username);
            
            shield::protocol::ProtocolResponse response;
            response.set_json_body({
                {"success", true},
                {"token", token},
                {"expires_in", 3600}
            });
            callback(response);
        } else {
            shield::protocol::ProtocolResponse response;
            response.status_code = 401;
            response.status_message = "Unauthorized";
            response.set_json_body({
                {"success", false},
                {"error", "Invalid credentials"}
            });
            callback(response);
        }
    } catch (const std::exception& e) {
        shield::protocol::ProtocolResponse response;
        response.status_code = 400;
        response.status_message = "Bad Request";
        response.set_json_body({
            {"success", false},
            {"error", "Invalid JSON format"}
        });
        callback(response);
    }
});

// 添加游戏 API 路由
http_handler->add_post_route("/api/game/action", [](const auto& request, auto callback) {
    auto json_body = nlohmann::json::parse(request.get_body_as_string());
    
    std::string action = json_body["action"];
    std::string player_id = json_body["player_id"];
    
    // 转发给 Actor 系统处理
    auto actor_system = shield::core::ServiceLocator::get<shield::actor::DistributedActorSystem>();
    auto player_actor = actor_system->find_actor("player_" + player_id);
    
    if (player_actor) {
        // 异步调用 Actor
        // 这里应该实现异步回调机制
        shield::protocol::ProtocolResponse response;
        response.set_json_body({
            {"success", true},
            {"message", "Action processed"}
        });
        callback(response);
    } else {
        shield::protocol::ProtocolResponse response;
        response.status_code = 404;
        response.set_json_body({
            {"success", false},
            {"error", "Player not found"}
        });
        callback(response);
    }
});

// 添加中间件 - 认证检查
http_handler->add_middleware([](const auto& request, auto& response) -> bool {
    std::string path = request.path;
    
    // 公开路径不需要认证
    if (path == "/api/health" || path == "/api/login") {
        return true;  // 继续处理
    }
    
    // 检查 Authorization 头
    std::string auth_header = request.get_header("Authorization");
    if (auth_header.empty() || !auth_header.starts_with("Bearer ")) {
        response.status_code = 401;
        response.set_json_body({
            {"error", "Missing or invalid authorization header"}
        });
        return false;  // 停止处理
    }
    
    // 验证 JWT Token
    std::string token = auth_header.substr(7);  // 移除 "Bearer "
    if (!validate_jwt_token(token)) {
        response.status_code = 401;
        response.set_json_body({
            {"error", "Invalid or expired token"}
        });
        return false;
    }
    
    return true;  // 继续处理
});

// 设置静态文件服务
http_handler->set_static_file_handler("public/");
http_handler->add_static_route("/admin", "admin/index.html");
```

## 🔌 WebSocketProtocolHandler WebSocket 协议处理器

处理 WebSocket 协议的连接、消息和断开连接。

### 类定义

```cpp
namespace shield::protocol {

struct WebSocketConfig {
    size_t max_frame_size = 64 * 1024;              // 最大帧大小 (64KB)
    std::chrono::seconds ping_interval{30};         // Ping 间隔
    std::chrono::seconds pong_timeout{5};           // Pong 超时
    bool enable_compression = false;                // 启用压缩
    std::vector<std::string> supported_protocols;   // 支持的子协议
};

enum class WebSocketFrameType {
    TEXT,       // 文本帧
    BINARY,     // 二进制帧
    PING,       // Ping 帧
    PONG,       // Pong 帧
    CLOSE       // 关闭帧
};

struct WebSocketFrame {
    WebSocketFrameType type;
    std::vector<uint8_t> payload;
    bool is_final = true;
    
    std::string get_text_payload() const;
    void set_text_payload(const std::string& text);
};

// WebSocket 事件处理器
using WebSocketMessageHandler = std::function<void(const WebSocketFrame&)>;
using WebSocketConnectHandler = std::function<void(const std::string& protocol)>;
using WebSocketDisconnectHandler = std::function<void(uint16_t code, const std::string& reason)>;

class WebSocketProtocolHandler : public IProtocolHandler {
public:
    explicit WebSocketProtocolHandler(const WebSocketConfig& config = WebSocketConfig{});
    virtual ~WebSocketProtocolHandler();
    
    // IProtocolHandler 接口实现
    ProtocolType get_protocol_type() const override;
    std::string get_protocol_name() const override;
    bool can_handle(const std::vector<uint8_t>& data) override;
    std::optional<ProtocolMessage> parse_message(const std::vector<uint8_t>& data) override;
    std::vector<uint8_t> serialize_response(const ProtocolResponse& response) override;
    void handle_message_async(const ProtocolMessage& message, 
                             std::function<void(const ProtocolResponse&)> callback) override;
    
    // WebSocket 特定方法
    bool is_handshake_request(const ProtocolMessage& message);
    ProtocolResponse create_handshake_response(const ProtocolMessage& request);
    
    // 帧处理
    std::vector<uint8_t> create_frame(const WebSocketFrame& frame);
    std::optional<WebSocketFrame> parse_frame(const std::vector<uint8_t>& data);
    
    // 事件处理器设置
    void set_message_handler(WebSocketMessageHandler handler);
    void set_connect_handler(WebSocketConnectHandler handler);
    void set_disconnect_handler(WebSocketDisconnectHandler handler);
    
    // 连接管理
    void send_ping(const std::string& data = "");
    void send_pong(const std::string& data = "");
    void close_connection(uint16_t code = 1000, const std::string& reason = "");

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::protocol
```

### 使用示例

```cpp
// 创建 WebSocket 处理器
shield::protocol::WebSocketConfig config;
config.max_frame_size = 128 * 1024;  // 128KB
config.ping_interval = std::chrono::seconds(30);
config.supported_protocols = {"shield-game-v1", "shield-chat-v1"};

auto ws_handler = std::make_unique<shield::protocol::WebSocketProtocolHandler>(config);

// 设置连接处理器
ws_handler->set_connect_handler([](const std::string& protocol) {
    SHIELD_LOG_INFO << "WebSocket 连接建立，协议: " << protocol;
    
    // 发送欢迎消息
    shield::protocol::WebSocketFrame welcome_frame;
    welcome_frame.type = shield::protocol::WebSocketFrameType::TEXT;
    welcome_frame.set_text_payload(nlohmann::json({
        {"type", "welcome"},
        {"protocol", protocol},
        {"server_time", std::time(nullptr)}
    }).dump());
    
    // 这里需要会话引用来发送帧
    // session->send(ws_handler->create_frame(welcome_frame));
});

// 设置消息处理器
ws_handler->set_message_handler([](const shield::protocol::WebSocketFrame& frame) {
    if (frame.type == shield::protocol::WebSocketFrameType::TEXT) {
        try {
            std::string message_text = frame.get_text_payload();
            auto json_msg = nlohmann::json::parse(message_text);
            
            std::string msg_type = json_msg["type"];
            
            if (msg_type == "chat") {
                handle_chat_message(json_msg);
            } else if (msg_type == "game_command") {
                handle_game_command(json_msg);
            } else if (msg_type == "heartbeat") {
                handle_heartbeat(json_msg);
            } else {
                SHIELD_LOG_WARN << "未知 WebSocket 消息类型: " << msg_type;
            }
            
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "WebSocket 消息解析失败: " << e.what();
        }
    } else if (frame.type == shield::protocol::WebSocketFrameType::BINARY) {
        // 处理二进制消息
        handle_binary_message(frame.payload);
    }
});

// 设置断开连接处理器
ws_handler->set_disconnect_handler([](uint16_t code, const std::string& reason) {
    SHIELD_LOG_INFO << "WebSocket 连接断开，代码: " << code << ", 原因: " << reason;
    
    // 清理连接相关资源
    cleanup_connection_resources();
});

// 游戏消息处理示例
void handle_game_command(const nlohmann::json& message) {
    std::string command = message["command"];
    std::string player_id = message["player_id"];
    
    if (command == "move") {
        float x = message["x"];
        float y = message["y"];
        
        // 转发给玩家 Actor
        auto actor_system = shield::core::ServiceLocator::get<shield::actor::DistributedActorSystem>();
        auto player_actor = actor_system->find_actor("player_" + player_id);
        
        if (player_actor) {
            shield::actor::LuaMessage lua_msg;
            lua_msg.type = "move";
            lua_msg.data = {
                {"x", std::to_string(x)},
                {"y", std::to_string(y)}
            };
            
            // 异步发送给 Actor (这里需要实现回调机制)
            // caf::anon_send(player_actor, lua_msg);
        }
    } else if (command == "attack") {
        std::string target_id = message["target_id"];
        // 处理攻击命令...
    }
}

// 聊天消息处理示例
void handle_chat_message(const nlohmann::json& message) {
    std::string sender = message["sender"];
    std::string content = message["content"];
    std::string channel = message.value("channel", "global");
    
    // 广播聊天消息
    nlohmann::json broadcast_msg = {
        {"type", "chat_broadcast"},
        {"sender", sender},
        {"content", content},
        {"channel", channel},
        {"timestamp", std::time(nullptr)}
    };
    
    // 创建广播帧
    shield::protocol::WebSocketFrame broadcast_frame;
    broadcast_frame.type = shield::protocol::WebSocketFrameType::TEXT;
    broadcast_frame.set_text_payload(broadcast_msg.dump());
    
    // 广播给所有连接 (这里需要会话管理器)
    // session_manager->broadcast_to_channel(channel, ws_handler->create_frame(broadcast_frame));
}
```

## 🚦 ProtocolRouter 协议路由器

协议路由器负责根据数据内容自动识别协议类型并路由到相应的处理器。

### 类定义

```cpp
namespace shield::protocol {

class ProtocolRouter {
public:
    ProtocolRouter();
    virtual ~ProtocolRouter();
    
    // 协议处理器管理
    void register_handler(std::unique_ptr<IProtocolHandler> handler);
    void unregister_handler(ProtocolType type);
    IProtocolHandler* get_handler(ProtocolType type);
    
    // 自动路由
    std::optional<ProtocolType> detect_protocol(const std::vector<uint8_t>& data);
    void route_message(const std::vector<uint8_t>& data, 
                      std::function<void(const ProtocolResponse&)> callback);
    
    // 统计信息
    struct Statistics {
        std::atomic<uint64_t> total_messages{0};
        std::atomic<uint64_t> http_messages{0};
        std::atomic<uint64_t> websocket_messages{0};
        std::atomic<uint64_t> tcp_messages{0};
        std::atomic<uint64_t> unknown_messages{0};
    };
    
    const Statistics& get_statistics() const;
    void reset_statistics();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::protocol
```

### 使用示例

```cpp
// 创建协议路由器
auto protocol_router = std::make_unique<shield::protocol::ProtocolRouter>();

// 注册协议处理器
auto http_handler = std::make_unique<shield::protocol::HttpProtocolHandler>();
auto ws_handler = std::make_unique<shield::protocol::WebSocketProtocolHandler>();

// 配置 HTTP 路由
http_handler->add_get_route("/api/status", [](const auto& req, auto callback) {
    shield::protocol::ProtocolResponse response;
    response.set_json_body({{"status", "running"}});
    callback(response);
});

// 配置 WebSocket 事件
ws_handler->set_message_handler([](const auto& frame) {
    // 处理 WebSocket 消息
});

// 注册到路由器
protocol_router->register_handler(std::move(http_handler));
protocol_router->register_handler(std::move(ws_handler));

// 在网络会话中使用路由器
class ProtocolAwareSession : public shield::net::Session {
public:
    ProtocolAwareSession(boost::asio::ip::tcp::socket socket, 
                        shield::protocol::ProtocolRouter& router)
        : Session(std::move(socket)), m_router(router) {}

protected:
    void on_message(const std::vector<uint8_t>& data) override {
        // 自动协议检测和路由
        m_router.route_message(data, [this](const shield::protocol::ProtocolResponse& response) {
            // 发送响应
            auto response_data = serialize_response(response);
            send(response_data);
        });
    }

private:
    shield::protocol::ProtocolRouter& m_router;
    
    std::vector<uint8_t> serialize_response(const shield::protocol::ProtocolResponse& response) {
        // 根据协议类型序列化响应
        auto protocol_type = detect_response_protocol(response);
        auto handler = m_router.get_handler(protocol_type);
        return handler ? handler->serialize_response(response) : std::vector<uint8_t>{};
    }
};
```

## 🧪 测试示例

### 单元测试

```cpp
#define BOOST_TEST_MODULE ProtocolTest
#include <boost/test/unit_test.hpp>
#include "shield/protocol/http_protocol_handler.hpp"

BOOST_AUTO_TEST_SUITE(ProtocolTest)

BOOST_AUTO_TEST_CASE(test_http_request_parsing) {
    shield::protocol::HttpProtocolHandler handler;
    
    // 构造 HTTP 请求
    std::string http_request = 
        "GET /api/test HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "User-Agent: TestClient/1.0\r\n"
        "Accept: application/json\r\n"
        "\r\n";
    
    std::vector<uint8_t> request_data(http_request.begin(), http_request.end());
    
    // 测试协议检测
    BOOST_CHECK(handler.can_handle(request_data));
    
    // 测试消息解析
    auto parsed_message = handler.parse_message(request_data);
    BOOST_CHECK(parsed_message.has_value());
    
    BOOST_CHECK_EQUAL(parsed_message->method, "GET");
    BOOST_CHECK_EQUAL(parsed_message->path, "/api/test");
    BOOST_CHECK_EQUAL(parsed_message->get_header("Host"), "localhost:8080");
    BOOST_CHECK_EQUAL(parsed_message->get_header("User-Agent"), "TestClient/1.0");
}

BOOST_AUTO_TEST_CASE(test_http_response_serialization) {
    shield::protocol::HttpProtocolHandler handler;
    
    // 创建响应
    shield::protocol::ProtocolResponse response;
    response.status_code = 200;
    response.status_message = "OK";
    response.set_header("Content-Type", "application/json");
    response.set_json_body({{"message", "Hello, World!"}});
    
    // 序列化响应
    auto response_data = handler.serialize_response(response);
    BOOST_CHECK(!response_data.empty());
    
    // 验证响应格式
    std::string response_str(response_data.begin(), response_data.end());
    BOOST_CHECK(response_str.find("HTTP/1.1 200 OK") != std::string::npos);
    BOOST_CHECK(response_str.find("Content-Type: application/json") != std::string::npos);
    BOOST_CHECK(response_str.find("\"message\":\"Hello, World!\"") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
```

### 集成测试

```cpp
BOOST_AUTO_TEST_SUITE(ProtocolIntegrationTest)

BOOST_AUTO_TEST_CASE(test_http_websocket_upgrade) {
    // 测试 HTTP 到 WebSocket 的协议升级
    shield::protocol::HttpProtocolHandler http_handler;
    shield::protocol::WebSocketProtocolHandler ws_handler;
    
    // WebSocket 握手请求
    std::string upgrade_request = 
        "GET /websocket HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    
    std::vector<uint8_t> request_data(upgrade_request.begin(), upgrade_request.end());
    
    // HTTP 处理器解析请求
    auto parsed_message = http_handler.parse_message(request_data);
    BOOST_CHECK(parsed_message.has_value());
    
    // 检查是否为 WebSocket 握手
    BOOST_CHECK(ws_handler.is_handshake_request(*parsed_message));
    
    // 创建握手响应
    auto handshake_response = ws_handler.create_handshake_response(*parsed_message);
    BOOST_CHECK_EQUAL(handshake_response.status_code, 101);
    BOOST_CHECK(handshake_response.headers.find("Upgrade") != handshake_response.headers.end());
}

BOOST_AUTO_TEST_SUITE_END()
```

## 📚 最佳实践

### 1. 协议设计

```cpp
// ✅ 好的协议设计
class GameProtocolHandler : public shield::protocol::IProtocolHandler {
public:
    // 清晰的协议标识
    bool can_handle(const std::vector<uint8_t>& data) override {
        if (data.size() < 4) return false;
        
        // 检查魔数
        uint32_t magic = *reinterpret_cast<const uint32_t*>(data.data());
        return magic == GAME_PROTOCOL_MAGIC;
    }
    
    // 结构化的消息格式
    std::optional<ProtocolMessage> parse_message(const std::vector<uint8_t>& data) override {
        if (data.size() < sizeof(GameMessageHeader)) {
            return std::nullopt;
        }
        
        const auto* header = reinterpret_cast<const GameMessageHeader*>(data.data());
        
        // 验证消息完整性
        if (data.size() < sizeof(GameMessageHeader) + header->body_size) {
            return std::nullopt;
        }
        
        // 构造协议消息
        ProtocolMessage message;
        message.type = ProtocolType::CUSTOM;
        message.method = std::to_string(header->message_type);
        // ... 填充其他字段
        
        return message;
    }

private:
    static constexpr uint32_t GAME_PROTOCOL_MAGIC = 0x47414D45; // "GAME"
    
    struct GameMessageHeader {
        uint32_t magic;
        uint16_t version;
        uint16_t message_type;
        uint32_t body_size;
        uint32_t sequence_id;
    };
};
```

### 2. 错误处理

```cpp
// ✅ 健壮的错误处理
void handle_message_async(const ProtocolMessage& message, 
                         std::function<void(const ProtocolResponse&)> callback) override {
    try {
        // 验证消息格式
        if (!validate_message(message)) {
            ProtocolResponse error_response;
            error_response.status_code = 400;
            error_response.set_json_body({
                {"error", "Invalid message format"}
            });
            callback(error_response);
            return;
        }
        
        // 处理消息
        process_message_internal(message, [callback](const ProtocolResponse& response) {
            // 确保回调总是被调用
            callback(response);
        });
        
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "消息处理异常: " << e.what();
        
        ProtocolResponse error_response;
        error_response.status_code = 500;
        error_response.set_json_body({
            {"error", "Internal server error"}
        });
        callback(error_response);
    }
}
```

### 3. 性能优化

```cpp
// ✅ 性能优化技巧

// 1. 消息池化
class OptimizedProtocolHandler : public IProtocolHandler {
private:
    // 重用消息对象
    thread_local static ProtocolMessage s_message_buffer;
    thread_local static ProtocolResponse s_response_buffer;
    
public:
    std::optional<ProtocolMessage> parse_message(const std::vector<uint8_t>& data) override {
        // 重用缓冲区
        s_message_buffer.headers.clear();
        s_message_buffer.body.clear();
        
        // 解析到缓冲区...
        
        return s_message_buffer;
    }
};

// 2. 零拷贝序列化
std::vector<uint8_t> serialize_response(const ProtocolResponse& response) override {
    // 预计算响应大小
    size_t total_size = calculate_response_size(response);
    
    std::vector<uint8_t> buffer;
    buffer.reserve(total_size);  // 避免重新分配
    
    // 直接序列化到缓冲区
    serialize_headers_to_buffer(response, buffer);
    serialize_body_to_buffer(response, buffer);
    
    return buffer;
}
```

---

协议模块为 Shield 框架提供了灵活、高性能的协议处理能力。通过统一的接口设计，可以轻松支持多种网络协议，满足不同类型游戏的需求。