# Starter 系统设计

本文定义重构后的 C++ 启动编排系统。Starter 只负责把已确定的运行时模块按依赖顺序初始化、启动和关闭，不提供 DI、插件、注解、条件装配或框架事件总线。

## 定位

Starter 是 bootstrap 内部机制，不是用户 API。

它负责：

- 固定模块启动顺序。
- 把配置解析后的模块配置传给对应模块。
- 在失败时快速停止并回滚已启动模块。
- 在关闭时按反向顺序释放资源。
- 把 Lua VM 和 Lua API 绑定初始化集中到 `ScriptStarter`。

它不负责：

- DI/IoC 容器。
- 注解或条件装配。
- Lua 插件加载。
- 生命周期事件总线。
- 服务发现、metrics、health 或管理端点。
- Lua Service 的业务生命周期。Lua Service 由 `shield_core` 和 `shield_lua` 按服务语义管理。

## 核心对象

### BootstrapContext

`BootstrapContext` 是 bootstrap 阶段的显式上下文。它不是 service locator，不提供按类型全局查询；每个 Starter 只读取自己声明的依赖字段。

```cpp
struct BootstrapContext {
  RuntimeOptions options;
  Config config;

  std::shared_ptr<LogRuntime> log;
  std::shared_ptr<ShieldCore> core;
  std::shared_ptr<DataRuntime> data;
  std::shared_ptr<NetRuntime> net;
  std::shared_ptr<TransportRegistry> transport;
  std::shared_ptr<LuaRuntime> lua;
};
```

规则：

- 只有 bootstrap 拥有 `BootstrapContext`。
- Starter 之间不通过全局单例找依赖。
- 模块如果需要依赖，必须在构造或 `start()` 参数中显式接收。
- `shield_core` 不能从 `BootstrapContext` 反查 Lua、net、data、config 或 log。

### IStarter

```cpp
class IStarter {
public:
  virtual std::string_view name() const = 0;
  virtual std::span<const std::string_view> dependencies() const = 0;

  virtual Result<void> configure(BootstrapContext& ctx) = 0;
  virtual Result<void> start(BootstrapContext& ctx) = 0;
  virtual void stop(BootstrapContext& ctx) noexcept = 0;

  virtual ~IStarter() = default;
};
```

规则：

- Starter 列表由 `shield_bootstrap` 静态注册。
- 不从动态库、Lua 脚本或配置中发现 Starter。
- `configure()` 只做配置校验和轻量准备，不启动线程、不绑定端口。
- `start()` 可以分配资源、启动线程、创建 runtime 对象。
- `stop()` 必须 noexcept，失败只能记录日志。

## 标准 Starter

| Starter | 产物 | 依赖 | 说明 |
| --- | --- | --- | --- |
| `ConfigStarter` | `Config` | 无 | 解析 CLI、加载 YAML、环境变量展开、校验 core schema |
| `LogStarter` | `LogRuntime` | `ConfigStarter` | 初始化日志 sink 和日志级别 |
| `CoreStarter` | `ShieldCore` | `ConfigStarter`, `LogStarter` | 创建 service registry、message router、timer scheduler、CAF adapter |
| `DataStarter` | `DataRuntime` | `ConfigStarter`, `LogStarter` | 按配置创建 DB/Redis 原始访问能力，可未启用 |
| `TransportStarter` | `TransportRegistry` | `ConfigStarter`, `LogStarter` | 注册内置 transport 和用户显式链接的 C++ transport |
| `NetStarter` | `NetRuntime` | `ConfigStarter`, `LogStarter`, `TransportStarter` | 创建 listener/session 管理，但默认不开始 accept |
| `ScriptStarter` | `LuaRuntime` | `ConfigStarter`, `LogStarter`, `CoreStarter`, `DataStarter`, `NetStarter` | 创建 Lua VM 策略，注册 `shield.*` API |
| `ServiceStarter` | configured services | `CoreStarter`, `ScriptStarter` | spawn YAML 声明的 Lua/C++ services |
| `AcceptStarter` | active listeners | `NetStarter`, `ServiceStarter` | 服务已 ready 后开启网络 accept |

可选模块如 `ClusterStarter`、`OpsStarter`、`GlobalStarter` 必须在独立官方模块中声明，不能进入 `shield_core` 或默认 Starter 列表。

## 启动顺序

```txt
parse command line
-> ConfigStarter.configure/start
-> LogStarter.configure/start
-> CoreStarter.configure/start
-> DataStarter.configure/start
-> TransportStarter.configure/start
-> NetStarter.configure/start
-> ScriptStarter.configure/start
-> ServiceStarter.configure/start
-> AcceptStarter.configure/start
-> enter event loop
```

关键约束：

- 网络 listener 可以提前创建，但必须等 `ServiceStarter` 完成后再 accept。
- Lua API 绑定只在 `ScriptStarter` 中发生。
- YAML 声明的 Lua 服务只在 `ServiceStarter` 中 spawn。
- 启动失败时按已成功启动 Starter 的反向顺序 stop。

## ScriptStarter

`ScriptStarter` 是 Lua VM 生命周期起点，负责把 C++ runtime 能力绑定成 `shield.*` API。

职责：

- 创建 `LuaRuntime`、VM pool 或 VM factory。
- 加载标准 Lua bootstrap chunk。
- 注册基础 API：`shield.log.*`、`shield.config`、`shield.now`。
- 注册 core API：`shield.spawn`、`shield.exit`、`shield.self`、`shield.names`、`shield.query`、`shield.register`、`shield.unregister`、`shield.send`、`shield.call`、`shield.call_timeout`。
- 注册 timer API：`shield.timer`、`shield.timer_once`、`shield.cancel_timer`、`shield.sleep`、`shield.fork`。
- 注册已启用模块 API：`shield.db.*`、`shield.redis.*`、gateway session API。
- 为未启用模块注册明确的 `module_unavailable` 错误，而不是静默 nil。

非职责：

- 不加载插件脚本。
- 不发布 lifecycle event。
- 不创建业务 service。
- 不决定服务启动顺序。
- 不通过全局 ApplicationContext 取依赖。

## Lua VM 策略

默认策略：

- 每个 service 一个 Lua state。
- service 退出后释放对应 Lua state。
- 共享只读标准库和 C++ binding 描述，但不共享 Lua 全局业务状态。
- 禁止跨 service 直接传递 Lua object 指针。
- 服务间数据必须经过 MessagePayload 序列化。

配置示例：

```yaml
lua:
  vm:
    mode: per_service
    max_vms: 10000
    max_memory_mb: 64
  sandbox:
    allow_os: false
    allow_io: false
```

## 错误处理

Starter 错误必须结构化：

```cpp
struct StarterError {
  std::string starter;
  ErrorCode code;
  std::string message;
  std::vector<std::string> context;
};
```

失败策略：

- 配置错误：拒绝启动。
- core 初始化失败：拒绝启动。
- data 启用且连接失败：拒绝启动，除非该 datasource 标记为 optional。
- net 端口绑定失败：拒绝启动。
- Lua API 绑定失败：拒绝启动。
- 单个配置 service 启动失败：按 `actors[].required` 决定继续或失败，默认 required。

## 关闭顺序

```txt
stop accepting network connections
-> stop configured services
-> stop Lua runtime
-> stop net runtime
-> stop transport registry
-> stop data runtime
-> stop core runtime
-> flush log
```

规则：

- 停止网络 accept 后，已有 session 进入 draining。
- 服务停止时先发送 `on_exit(reason)`，超时后强制释放。
- `on_exit` 中不能执行会挂起的 `shield.call`。
- `ScriptStarter.stop()` 只能释放 VM，不承担业务保存。

## 与旧系统的取舍

以下旧设计直接删除，不保留兼容层：

- `ApplicationContext` 作为全局组件注册中心。
- DI/IoC 容器。
- 注解、条件装配。
- Lua 插件系统和 `shield.plugin.*`。
- C++/Lua 生命周期事件总线。
- 通过配置动态发现 Starter。

需要扩展 runtime 时，只允许两种方式：

- 在官方模块中新增明确 Starter，并显式加入 bootstrap 静态列表。
- 用户在 C++ 程序入口显式注册 transport、C++ service 或 optional module。
