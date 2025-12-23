# 网络层

Shield 网络层基于 Boost.Asio 实现，支持 TCP/UDP/HTTP/WebSocket 协议。

## Master/Slave Reactor

采用主从 Reactor 模式：

- **MasterReactor**: 负责 accept 连接
- **SlaveReactor**: 处理已建立连接的 IO

### NetworkConfig

```cpp
#include <shield/net/network_config.hpp>

shield::net::NetworkConfig config;
config.master_reactor_threads = 1;
config.slave_reactor_threads = 4;
```

## TCP Session

```cpp
#include <shield/net/session.hpp>

class MySession : public shield::net::Session {
protected:
    void on_message(const std::vector<uint8_t>& data) override {
        // 处理消息
    }
};
```

## UDP

### UdpSession

```cpp
#include <shield/net/udp_session.hpp>

shield::net::UdpSession session(endpoint, io_context);
session.start();
```

### UdpReactor

```cpp
#include <shield/net/udp_reactor.hpp>

shield::net::UdpReactor reactor(8080, 2);
reactor.start();
```

## 协议处理

### BinaryProtocol

```cpp
#include <shield/protocol/binary_protocol.hpp>

shield::protocol::BinaryProtocol protocol;
auto encoded = protocol.encode(message);
auto decoded = protocol.decode(encoded);
```

### HTTP

```cpp
#include <shield/protocol/http_handler.hpp>

shield::protocol::HttpHandler handler;
handler.register_route("/api/player", [](const auto& req) {
    // 处理请求
});
```

### WebSocket

```cpp
#include <shield/protocol/websocket_handler.hpp>

shield::protocol::WebSocketHandler handler;
handler.on_message([](const std::string& msg) {
    // 处理 WebSocket 消息
});
```

## 网关

```cpp
#include <shield/gateway/gateway_service.hpp>

shield::gateway::GatewayConfig config;
config.port = 8080;
config.protocol = "tcp";

shield::gateway::GatewayService gateway(config);
gateway.start();
```
