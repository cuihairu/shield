# 核心设计理念

Shield 是一个受 [Skynet](https://github.com/cloudwu/skynet) 启发的、Actor 模型的、Lua 优先的游戏服务器运行时，使用 [CAF](https://github.com/actor-framework/actor-framework) 作为 Actor 传输基础。

## 设计原则

### 1. core / extensions 严格分离

`shield_core`（静态库）包含运行时启动路径上的所有模块：生命周期管理、Actor 系统、Lua VM、服务语义、网关、协议、服务发现、配置、日志。`shield_extensions`（静态库）包含可选功能：DI 容器、Prometheus 指标、健康检查、注解/条件注册、插件系统。

核心启动路径不依赖扩展库。`shield` 可执行文件链接两者。

### 2. CAF 传输 + Shield 语义

CAF 处理 Actor 的底层机制：spawn、send、request、schedule、序列化、分布式连接。Shield 在 CAF 之上提供游戏服务器语义：

- **命名服务**: 通过字符串名称查找和调用 Actor，而非 CAF 的 typed actor handle
- **send / call 双模式**: 异步消息 + 同步请求-响应
- **uniqueservice**: 单例服务保证
- **ServiceContext**: thread-local 上下文，让自由函数 API 无需全局状态

### 3. Lua 优先

业务逻辑全部用 Lua 编写。C++ 负责运行时和传输层。Lua 脚本通过 `shield.*` 全局表访问运行时 API，无需了解 CAF 细节：

```lua
shield.send("player_manager", "get_info", { player_id = "123" })
local result = shield.call("room_manager", "create", { max = "4" }, 3000)
```

脚本入口点为 `on_init()` + `on_message(msg)`。

### 4. 协议无关

TCP、HTTP、WebSocket、UDP 四种协议统一通过 `GatewayRequest` → `MiddlewareChain` → `GatewayRequestDispatcher` 管道分发到 Lua Actor。业务脚本不感知传输协议差异。

### 5. 开箱即用

内置网关、服务发现（Static/Redis/Nacos/Consul/Etcd）、健康检查、运行时诊断、调试控制台、模板系统。提供单节点和多节点配置模板，一键构建脚本（`build.sh` / `build.bat`），多阶段 Dockerfile。

## 关键抽象

### ApplicationContext

全局单例，管理所有 Bean 的生命周期（`init_all()` → `start_all()` → `stop_all()`）。只负责生命周期和查找，不包含业务逻辑。

### StarterManager

按顺序执行 Starter：ScriptStarter → ActorStarter → ServiceStarter → GatewayStarter → [MetricsStarter]。每个 Starter 负责注册自己的 Bean 到 ApplicationContext。

### ServiceContext

thread-local 上下文，通过 RAII `Guard` 在消息处理时自动设置/清除。保存当前 Actor 的 `self` 指针和 `ServiceContext` 引用，让 `shield::service::*` 自由函数无需显式传参。

### GatewayRequest / GatewayResponse

跨协议统一消息模型。所有协议（TCP/HTTP/WS）的请求都转换为 `GatewayRequest`，经过中间件链处理后分发给 Lua Actor。

### MiddlewareChain

闭包链式执行，从后向前构建。内置 `logging_middleware`、`cors_middleware`、`auth_middleware`，支持自定义扩展。

## 架构对比

| 维度 | Skynet | Shield |
|------|--------|--------|
| 语言 | C + Lua | C++20 + Lua |
| Actor 传输 | 自研 | CAF |
| 网络协议 | TCP | TCP + HTTP + WebSocket + UDP |
| 配置 | .config 文件 | YAML |
| 服务发现 | 手动 | 多后端（Static/Redis/Nacos/Consul/Etcd） |
| 指标 | 无 | Prometheus（可选） |
| 热重载 | Lua 热重载 | Lua 热重载 |
| 跨平台 | Linux only | Windows + macOS + Linux |

Shield 不覆盖 Skynet 的以下能力：harbor 集群、snax 框架、sharedata 机制。详见 [Skynet 对比](skynet-comparison.md)。
