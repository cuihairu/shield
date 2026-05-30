# 网关设计

Shield 网关统一处理 TCP、HTTP、WebSocket、UDP 四种协议，通过中间件管道和统一分发模型将请求路由到 Lua Actor。

## 统一分发模型

所有协议的请求都经过相同的处理管道：

```
TCP message ──┐
HTTP request ─┼→ GatewayRequest → MiddlewareChain → GatewayRequestDispatcher → LuaActor
WS message  ──┘
```

### GatewayRequest / GatewayResponse

```cpp
struct GatewayRequest {
    uint64_t session_id = 0;
    std::string path;              // /api/game/action 等
    std::string method;            // GET/POST/TCP/WS
    std::string body;              // JSON payload
    std::unordered_map<std::string, std::string> headers;
    ProtocolType protocol = ProtocolType::TCP;
};

struct GatewayResponse {
    bool success = true;
    int status_code = 200;
    std::string body;              // JSON response
    std::unordered_map<std::string, std::string> headers;
    static GatewayResponse ok(std::string body);
    static GatewayResponse error(int code, std::string body);
};
```

### GatewayRequestDispatcher

统一分发器，将三种协议的消息汇入同一管道：

1. 创建 `GatewayRequest`（携带 session_id、protocol、body）
2. 执行 `MiddlewareChain`（日志、CORS、认证等）
3. 根据协议选择对应 Lua 脚本
4. 使用 `caf::scoped_actor` 发送同步请求到 Lua Actor
5. 将 Lua 返回值写入 `GatewayResponse`

## 中间件管道

```cpp
class MiddlewareChain {
public:
    void use(Middleware mw);        // 添加中间件
    void execute(GatewayRequest& req, GatewayResponse& resp);  // 链式执行
};
```

中间件签名：

```cpp
using Middleware = std::function<void(GatewayRequest&, GatewayResponse&, std::function<void()>&)>;
```

### 内置中间件

| 中间件 | 用途 |
|--------|------|
| `logging_middleware()` | 请求日志（method、path、耗时） |
| `cors_middleware()` | CORS 头注入 |
| `auth_middleware(validator)` | 认证校验，失败时短路返回 |

执行顺序：logging → cors → [auth] → 最终处理。

## 网络架构

### TCP

`MasterReactor` → `SlaveReactor` 池 → `Session` → `BinaryProtocol`（4 字节长度前缀）

- MasterReactor 只负责 accept，分发连接到 SlaveReactor
- SlaveReactor 负责 I/O 读写
- Session 管理连接生命周期

### HTTP

`BeastHttpServer`（Boost.Beast）→ `HttpRouter` → `GatewayRequestDispatcher` → Lua Actor

支持 method + regex pattern 路由。

### WebSocket

`WebSocketProtocolHandler`（RFC 6455）→ 消息提取 `type` 字段路由 → Lua Actor

独立端口，配置中可指定 `lua_script` 处理 WebSocket 消息。

### UDP

`UdpReactor` → `UdpSession` → `UdpProtocolHandler` → Lua Actor

独立端口，适用于实时游戏场景。

## 内置 HTTP 端点

| 路径 | 方法 | 用途 |
|------|------|------|
| `/health` | GET | 基础健康检查 |
| `/health/detailed` | GET | 详细状态（服务列表） |
| `/status` | GET | 运行时状态（actor 数量/详情） |
| `/status/config` | GET | 配置重载范围 |
| `/login` | POST | 登录模板 |
| `/api/game/action` | POST | 游戏动作路由 |

## 游戏网关模板

`GameGateway` 提供三个预置场景：

### 登录模板

```cpp
GameGateway::setup_login_route(router, "scripts/gateway_login.lua");
```

HTTP POST `/login` → Lua 脚本验证 → 创建 session → 返回 token。

### 会话模板

```cpp
GameGateway::setup_session_handler(ws_handler, "scripts/gateway_session.lua");
```

WebSocket 连接 → 认证 → 绑定 player actor → 心跳。

### 消息分发模板

```cpp
GameGateway::setup_message_dispatch(dispatcher, "scripts/session_handler.lua");
```

TCP/WS 消息 → 路由到对应 actor → 回写响应。

## 配置示例

```yaml
gateway:
  listener:
    host: "0.0.0.0"
    port: 8080          # TCP 端口
  http:
    enabled: true
    port: 8082          # HTTP 端口
    backend: "beast"
  websocket:
    enabled: true
    port: 8083          # WebSocket 端口
    path: "/ws"
    lua_script: "scripts/session_handler.lua"
  udp:
    enabled: true
    port: 8084          # UDP 端口
```
