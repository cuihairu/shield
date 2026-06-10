# Skynet 对比

Shield 参考 Skynet 的服务模型，但目标不是复制 Skynet，也不是扩展成通用分布式框架。

完整运行时语义见 [运行时语义决策稿](./runtime-semantics.md)。

## 相同点

- 服务是基本运行单元。
- Lua 承载主要业务逻辑。
- `send` 表示异步消息。
- `call` 表示请求-响应。
- 定时器和轻量服务生命周期是核心能力。

## 不同点

| 维度 | Skynet | Shield 重构目标 |
| --- | --- | --- |
| 运行时语言 | C + Lua | C++20 + Lua |
| Actor 基础 | 自研 | CAF 内部实现 |
| 用户 API | `skynet.*` | `shield.*` |
| 配置 | Lua 配置 | YAML 声明式配置 |
| 平台 | 以 Unix-like 为主 | Windows / macOS / Linux |
| 当前范围 | 单进程 + 集群能力 | 先聚焦单节点 runtime |

## API 映射

```lua
-- Skynet
skynet.send(address, "lua", msg_type, ...)
local response = skynet.call(address, "lua", msg_type, ...)

-- Shield target
shield.send("service_name", method, data)
local ok, response = shield.call_timeout(timeout_ms, "service_name", method, data)
```

```lua
-- Skynet
skynet.timeout(100, function() ... end)

-- Shield target
shield.timer_once(100, function() ... end)
```

## CAF 与 Skynet 语义的关系

Shield 的目标不是重新实现 Skynet 的底层 actor runtime，而是用 CAF 承接机制层，再补齐 Skynet-like 服务语义。

| 层级 | Skynet | CAF | Shield 决策 |
| --- | --- | --- | --- |
| actor 调度 | Skynet 自研 | CAF scheduler | 使用 CAF |
| actor/message | Skynet service/message | CAF actor/message | 使用 CAF，隐藏 handle |
| async send | `skynet.send` | `send` / `anon_send` | 封装成 `shield.send` |
| sync call | `skynet.call` + Lua coroutine yield | `request` + continuation | 封装成 coroutine-aware `shield.call` |
| timer | `skynet.timeout` | `schedule` / delayed send | 封装成 `shield.timer_once` / `shield.timer` |
| service name | handle/name service | actor handle / registry 机制 | Shield 自己维护 service registry |
| Lua service | Lua script service | CAF 不负责 Lua | `shield_lua` 实现 |
| remote actor | harbor/cluster | middleman publish/connect | 不进 core，未来单独设计 |

## ID 与集群地址差异

Skynet 的 actor address 可以采用高位 cluster/harbor id、低位本地 handle 的紧凑模型。Shield 保留这个思想，但不把 bit layout 作为 public `ServiceId` 契约。

Shield 决策：

```cpp
using ServiceId = uint64_t; // 本地 service id
using NodeId = uint32_t;    // 节点 id

struct ServiceAddress {
  NodeId node;
  ServiceId id;
};
```

不同节点允许出现相同的本地 `ServiceId`，全局路由身份由 `{node_id, node_epoch, service_id}` 确定。这样可以避免 public API 被固定 bit layout 绑死，也能处理节点重启后的旧消息误投递问题。

## 关键差异

CAF 覆盖的是机制，不覆盖 Skynet 的 Lua 服务体验。

最关键的差异是 `call`：

```text
Skynet call:
  发送请求 → session 绑定当前 Lua coroutine → yield → response 到达 → resume

CAF request:
  发送请求 → continuation / await / receive → C++ actor 处理 response

Shield call:
  使用 CAF request 机制
  但对 Lua 暴露 Skynet-like coroutine 挂起语义
```

因此 `shield.call` 不能直接暴露 CAF request，也不能在 Lua 里做线程阻塞式等待。正确设计是挂起当前 Lua coroutine，同时保持 actor runtime 可继续调度其他消息。

## Shield 不覆盖

- Skynet harbor 集群模型。
- snax 框架。
- sharedata / stm 等共享数据机制。
- 内置服务发现框架。
- Prometheus / health check / plugin system 作为 core。

当前重构优先把单节点服务、消息、网络、Lua API 做薄做稳。

## 参考

- Skynet source: https://github.com/cloudwu/skynet/blob/master/lualib/skynet.lua
- CAF `event_based_actor`: https://www.actor-framework.org/static/doxygen/0.18.7/classcaf_1_1event__based__actor
- CAF requester/request: https://www.actor-framework.org/static/doxygen/0.18.7/classcaf_1_1mixin_1_1requester
