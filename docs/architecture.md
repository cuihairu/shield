# Shield 架构设计

Shield 是一个 Skynet 启发的、Actor 模型的、Lua 优先的游戏服务器运行时，使用 CAF 作为 Actor 传输基础。

## 架构核心理念

1. **core / extensions 分离**: 核心运行时（shield_core）与可选扩展（shield_extensions）严格分离
2. **CAF 传输 + Shield 语义**: CAF 处理 Actor 机制，Shield 提供游戏服务器语义
3. **Lua 优先**: 业务逻辑全部用 Lua 编写，C++ 负责运行时和传输
4. **协议无关**: TCP/HTTP/WebSocket/UDP 统一通过中间件管道分发
5. **开箱即用**: 内置网关、服务发现、指标、健康检查

## 架构层次图

```
┌──────────────────────────────────────────────┐
│              Lua 业务脚本                     │
│  on_init() / on_message() / shield.* API     │
├──────────────────────────────────────────────┤
│           服务层 (shield/service/)            │
│  send / call / query / timeout / fork         │
├──────────────────────────────────────────────┤
│         ServiceContext (thread-local)         │
├──────────────────────────────────────────────┤
│            CAF Actor System                  │
│  spawn / send / request / schedule           │
├──────────────────────────────────────────────┤
│       DistributedActorSystem                 │
│  registry / discovery / routing              │
├──────────────────────────────────────────────┤
│     Gateway / Middleware / Protocol          │
│  TCP / UDP / HTTP / WebSocket                │
│  MiddlewareChain (logging / cors / auth)      │
├──────────────────────────────────────────────┤
│     RuntimeDiagnostics / Metrics / Health    │
│  /health / /status / /metrics                │
└──────────────────────────────────────────────┘
```

## 库分离

```
shield_core (静态库)        shield_extensions (静态库)
├── core/  (生命周期)       ├── di/  (依赖注入)
├── actor/ (CAF Actor)      ├── metrics/  (Prometheus)
├── script/ (Lua VM 池)     ├── health/  (健康检查)
├── service/ (Skynet 语义)  ├── annotations/
├── gateway/ (网关/中间件)   ├── conditions/
├── protocol/ (协议适配)     ├── plugin/
├── net/ (网络 I/O)          └── extension_context
├── discovery/ (服务发现)
├── config/ (配置管理)
├── log/ (日志)
├── serialization/
└── cli/ / commands/
```

shield 可执行文件链接 `shield_core` + `shield_extensions`。核心启动路径不依赖扩展。

## 启动流程

```
ServerCommand::run()
  → ConfigManager.load_config()
  → StarterManager
      1. ScriptStarter     — 创建 LuaVMPool，预加载脚本
      2. ActorStarter      — 创建 CAF actor_system, DistributedActorSystem
      3. ServiceStarter    — 创建 ServiceContext, DebugConsole
      4. GatewayStarter    — 创建 GatewayService（TCP/HTTP/WS/UDP）
      [可选] MetricsStarter — Prometheus 指标
  → ApplicationContext.init_all() / start_all()
  → FileWatcher 监控配置变更
  → 等待 Ctrl+C
  → stop_all()
```

## 服务层 (shield/service/)

在 CAF 之上提供 Skynet 风格的服务 API：

| API | 用途 | CAF 底层 |
|-----|------|----------|
| `service::send(name, type, payload)` | 异步消息 | `caf::anon_send()` |
| `service::call(name, type, payload, timeout)` | 同步调用 | `self->request()` |
| `service::query(name)` | 服务查找 | `DistributedActorSystem::find_actor()` |
| `service::uniqueservice(name)` | 单例服务 | `query()` + spawn |
| `service::timeout(ms, callback)` | 定时器 | `self->schedule()` |
| `service::fork(func, name)` | 创建 Actor | `caf_system.spawn()` |

ServiceContext 通过 thread-local + RAII Guard 在消息处理时自动设置/清除。

## Lua 集成 (shield/script/)

Lua 业务脚本通过 `shield.*` 全局表访问运行时 API：

```lua
-- 消息传递
shield.send("player_manager", "get_info", { player_id = "123" })
local result = shield.call("player_manager", "get_info", { id = "123" }, 3000)

-- 定时器
shield.timeout(1000, function() log_info("1s elapsed") end)

-- 服务发现
local handle = shield.query("room_manager")
local services = shield.list_services()

-- 节点信息
local info = shield.self()
local node = shield.node_id()
```

脚本入口点：`on_init()` + `on_message(msg)`，使用 `shield_service.lua` 基类可简化模板编写。

## 网关层 (shield/gateway/)

### 中间件管道

所有协议的请求都经过 `MiddlewareChain`：

```
请求 → logging_middleware → cors_middleware → [auth_middleware] → 最终处理
```

### 统一分发

`GatewayRequestDispatcher` 将三种协议统一到同一管道：

```
TCP message ──┐
HTTP request ─┼→ GatewayRequest → MiddlewareChain → dispatch() → LuaActor
WS message  ──┘
```

### 内置 HTTP 端点

| 路径 | 用途 |
|------|------|
| `GET /health` | 基础健康检查 |
| `GET /health/detailed` | 详细状态（服务列表） |
| `GET /status` | 运行时状态（actor 数量/详情） |
| `GET /status/config` | 配置重载范围 |
| `POST /login` | 登录模板 |
| `POST /api/game/action` | 游戏动作路由 |

### 网络架构

- **TCP**: MasterReactor → SlaveReactor 池 → Session → BinaryProtocol (4字节长度前缀)
- **HTTP**: BeastHttpServer (Boost.Beast) → HttpRouter → LuaActor
- **WebSocket**: WebSocketProtocolHandler (RFC 6455) → LuaActor
- **UDP**: UdpReactor → UdpSession → UdpProtocolHandler

## 服务发现 (shield/discovery/)

统一接口 `IServiceDiscovery`，多后端实现：

- **Static**: 开发用，配置文件静态列表
- **Redis**: 基于 Redis 的注册/发现
- **Nacos / Consul / Etcd**: 生产级服务发现

## 可观测性

- **Metrics**: Prometheus 集成（可选，通过 `--enable-metrics` 启用）
- **Health**: HealthCheckRegistry + 内置指标（磁盘、数据库、应用）
- **Diagnostics**: RuntimeDiagnostics 提供 HTTP 端点查询运行时状态
- **Console**: TCP 13000 端口的调试控制台（`list`/`info`/`send`/`call`/`nodes`）
- **Logging**: Boost.Log 多目标（控制台/文件），支持动态级别调整

## 模板系统

- `templates/single-node/` — 单节点全功能服务器模板
- `templates/multi-node/` — 多节点模板（gateway.yaml + logic.yaml）
- `templates/REFERENCE_LAYOUT.md` — gateway + logic + storage 三层参考布局
- `build.sh` / `build.bat` — 一键构建脚本
- `Dockerfile` — 多阶段生产构建

## 适用场景

- **MMO 游戏后端**: Actor 模型天然适合玩家/房间/场景管理
- **实时竞技**: CAF 高性能消息传递 + Lua 快速迭代
- **社交游戏**: Lua 热重载支持频繁业务更新
- **跨平台**: Windows/macOS/Linux 全平台构建和运行
