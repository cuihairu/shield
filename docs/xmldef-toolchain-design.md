# Xmldef Toolchain Design

> Status: draft / deferred tooling design.
>
> 本文描述 `xmldef` 从 XML 契约到 descriptor、客户端代码生成插件、服务端 runtime binding 的完整设计草案。它不是当前 runtime 已实现能力；当前实现只支持 `xmldef` route catalog 加载，不支持真实 body schema decode、descriptor compiler、Unity/U3D generator 或 Lua binding 编译。

## Why This Exists

`xmldef` 如果只做成服务端一个 `BodyCodec`，最终一定退化成：

- 服务端持有 XML
- 客户端手写 route id 和字段结构
- 各端各自维护一套协议桩

这不符合 `KBEngine` / `BigWorld` 这类 XML 契约系统的真正价值。  
`xmldef` 的核心不只是 wire format，而是：

- 方法契约
- 字段契约
- route / schema 映射
- 服务端 handler binding
- 客户端代码生成

因此，`xmldef` 必须按 **toolchain + descriptor + runtime** 三层来做，而不是只在 `shield_transport` 里补一个 decoder。

这里有一个关键澄清：`xmldef` 更像 `ContractSource/Descriptor` 的来源，而不是天然等于某一种唯一的 `PayloadCodec`。  
也就是说，`xmldef` 定义的是方法、类型、route、binding 语义；它后续既可以驱动 native binary codec，也可以驱动 typed-json、typed-msgpack 这类其他 payload 表示。

## Scope

本文只讨论 `xmldef` 这条链路：

```text
xmldef xml
  -> compiler
  -> canonical IR
  -> descriptor package
  -> generator plugins
  -> runtime registry / binding
```

不覆盖：

- 通用 descriptor Merkle patch 细节
- mapper SQL codegen
- 多版本 schema runtime 热更新实现
- 客户端完整网络 SDK 细节

这些主题仍以 [schema-design.md](schema-design.md) 为总纲。

如果需要继续落到更硬的实现规格，请进一步阅读：

- [Xmldef Descriptor Spec](xmldef-descriptor-spec.md)
- [Xmldef Unity Generator Spec](xmldef-unity-generator-spec.md)
- [Xmldef Phase 1 Implementation Plan](xmldef-phase1-implementation.md)
- [Xmldef Compiler / Runtime MVP](xmldef-compiler-runtime-mvp.md)

## Design Goals

1. `xmldef/xml` 是唯一真相源。
2. 运行时不直接在热路径解析 XML。
3. 服务端和客户端都消费同一份 compiled descriptor。
4. Unity / U3D / Unreal / Web 等前端通过 generator plugin 消费 descriptor，而不是手写协议。
5. Lua handler binding 在启动时编译和校验，运行时不做字符串反射乱找。
6. `shield_transport` 只负责协议执行，不负责编译器和客户端代码生成。
7. toolchain 和 runtime 通过稳定 descriptor 契约解耦。

## Layering

### 1. Schema Source Layer

输入是 `xmldef/xml`：

```text
protocol/
  manifest.xml
  common/
    types.xml
  login/
    messages.xml
  avatar/
    messages.xml
```

这层负责：

- method / message / field 定义
- route id / name / direction 定义
- 可选 entity / component 语义
- 代码生成 hint

这层不负责：

- Lua method lookup
- socket framing
- 具体客户端语言输出

### 2. Compiler Layer

编译器把 XML 编译成 canonical IR，再导出 descriptor：

```text
xml
  -> xsd / semantic validation
  -> canonical IR
  -> descriptor.bin
  -> descriptor.debug.json
  -> route constants
  -> optional generated stubs
```

这层负责：

- 校验 XML 契约
- 统一 route / method / type 编号
- 建立 `route_id <-> method schema` 映射
- 产出多端共享的 descriptor package

### 3. Runtime Layer

runtime 只消费 compiled descriptor：

```text
descriptor.bin
  -> DescriptorRegistry
  -> XmldefSchemaRegistry
  -> XmldefBodyCodec
  -> Lua/C++ binding dispatcher
```

这层负责：

- body encode / decode
- route lookup
- schema lookup
- handler dispatch

这层不负责：

- 重新解析 XML
- 重新生成客户端代码
- 运行时猜测 Lua handler 名称

### 4. Generator Plugin Layer

每个前端语言或引擎通过 generator plugin 消费 descriptor：

```text
descriptor.bin / descriptor.debug.json
  -> unity generator plugin
  -> unreal generator plugin
  -> web/typescript generator plugin
  -> lua stub generator plugin
```

这层负责：

- 输出目标语言 DTO
- 输出 route 常量
- 输出 send/call API
- 输出 handler interface / stub

## Canonical IR

`xmldef` 不应直接生成 Unity/C# 或 Lua 代码；中间必须有一个 canonical IR。

最小 IR 建议包含：

- `package_id`
- `package_version`
- `namespaces`
- `types`
- `messages`
- `methods`
- `route_table`
- `direction` (`client_to_server` / `server_to_client` / `bidirectional`)
- `schema_id`
- `field definitions`
- `codegen hints`

其中 `method` 应至少包含：

```text
method {
  method_id
  route_id
  full_name          // e.g. Avatar.move
  request_type
  response_type?     // optional for RPC-style methods
  direction
  target_hint?
  binding_hint?
}
```

## Descriptor Package

`xmldef` 最终编译产物不是 XML 本身，而是 descriptor package。

建议最小产物：

- `descriptor.bin`
- `descriptor.debug.json`
- `route_constants.json`
- `manifest.json`

其中：

- `descriptor.bin` 是 runtime 主消费物
- `descriptor.debug.json` 给 generator、工具和调试使用
- `route_constants.json` 方便轻量 generator 或脚本工具消费

## Runtime Model

### Fixed Pipeline Integration

`xmldef` 在当前固定协议骨架中仍然只占固定槽位：

```text
Ingress:
  Envelope
  -> RouteExtractor(header.route_id)
  -> RoutePolicy
  -> XmldefBodyCodec.decode
  -> binding dispatcher
  -> Lua/C++ handler

Egress:
  Lua/C++ business message
  -> RouteResolver
  -> XmldefBodyCodec.encode
  -> Envelope.encode
```

也就是说：

- `xmldef` 不是单独的一条 gateway middleware chain
- `xmldef` 是固定 pipeline 中的 schema-aware codec + binding model

### Descriptor Registry

runtime 启动时加载 `descriptor.bin`，建立：

- `route_id -> method descriptor`
- `method full name -> route_id`
- `schema_id -> type descriptor`
- `route_id -> request/response schema`

这个 registry 属于 runtime shared state，不属于 Lua service 自己。

### XmldefBodyCodec

`XmldefBodyCodec` 的职责是：

- 根据 `route_id` 找到 method schema
- decode bytes -> structured business message
- encode structured business message -> bytes

它不负责：

- 动态查 Lua method
- 直接决定业务 handler

### Binding Dispatcher

`xmldef` 参考 `KBEngine/BigWorld` 时，确实需要“method -> script function”能力。  
但最干净的实现不是在热路径里动态字符串反射，而是：

```text
descriptor methods
  + lua module binding config
  -> compile binding registry at startup
  -> cache route_id -> lua function
```

运行时只做缓存命中。

## Lua Binding Model

### Explicit Binding Table

推荐 Lua service 显式声明 binding 表：

```lua
local M = {}

M.xmldef = {
  bindings = {
    ["Avatar.move"] = "on_avatar_move",
    ["Login.reqAuth"] = "on_login_req_auth",
  }
}

function M.on_avatar_move(session, msg)
end

function M.on_login_req_auth(session, msg)
end

return M
```

启动时编译为：

```text
route_id -> cached lua function
```

### Startup Validation

必须做强校验：

- XML 里声明了 method
- Lua binding table 没映射
- 或映射到了一个不存在的函数
- 启动直接失败

不能把这类错误拖到运行时第一包才报。

### Optional Generated Stub

toolchain 可以顺手生成 Lua stub：

```lua
local M = {}

M.xmldef = {
  bindings = {
    ["Avatar.move"] = "on_avatar_move",
  }
}

function M.on_avatar_move(session, msg)
  error("TODO")
end

return M
```

这不是 runtime 必需品，但能显著降低接入成本。

## Client Generator Plugin Model

### Why Plugin

Unity / U3D、Unreal、Web 的输出形态完全不同：

- C# 需要 DTO + client facade + handler interface
- Unreal 需要 C++ types + wrappers
- Web 需要 TS types + codec + client API

因此不能把“客户端代码生成”写死在 runtime 或 core CMake target 里。  
它必须是 **descriptor consumer plugin**。

### Generator Plugin Contract

建议定义一个独立 generator plugin contract：

```text
input:
  descriptor.bin
  descriptor.debug.json
  generator config

output:
  generated source files
  optional docs
  optional project metadata
```

plugin 最小接口建议是：

- `name`
- `version`
- `target` (`unity-csharp`, `unreal-cpp`, `typescript`, `lua-stub`)
- `generate(descriptor, output_dir, options)`

这个 plugin contract 是 **tooling plugin**，不是 runtime `shield_plugin`。

原因很简单：

- 它是开发期工具
- 不是运行时后端能力
- 不应该进入 `shield_plugin` host ABI

### Unity / U3D Plugin

Unity generator 最小输出建议：

- `RouteIds.cs`
- `Messages/*.cs`
- `XmldefCodec.cs`
- `GatewayClient.cs`
- `IXmldefHandler.cs`
- optional `partial` extension stubs

示例：

```csharp
namespace Game.Protocol.Avatar {
    public struct MoveRequest {
        public int EntityId;
        public float X;
        public float Y;
        public float Z;
    }
}

namespace Game.Protocol {
    public static class RouteIds {
        public const ushort AvatarMove = 0x1001;
    }
}

public interface IXmldefHandler {
    void OnAvatarMove(Game.Protocol.Avatar.MoveRequest msg);
}
```

目标是让业务代码写成：

```csharp
client.SendAvatarMove(new MoveRequest { ... });
```

而不是：

```csharp
socket.Send(rawBytes);
```

### Unreal / TS / Lua

其他 generator 原则相同：

- Unreal: 生成 C++ DTO / route constants / facade
- TS: 生成 type definitions / codec wrappers / gateway client
- Lua: 生成 binding stub / route constants / optional send helper

## Service-side Runtime Helpers

为避免 Lua 手拼 route/schema，服务端也应有辅助层：

- `session:send(message_table)` 由 `RouteResolver` 处理
- 可选生成 `xmldef.send.AvatarMove(session, msg)` helper
- 可选生成 `shield.xmldef.routes.AvatarMove`

这样 Lua 层面对的是契约对象，不是裸 route id。

## Delivery Phases

### Phase 1: Compiler Boundary

- 定义 `xmldef -> canonical IR`
- 定义 descriptor package 结构
- 明确 generator plugin contract
- 文档冻结目标输入输出

### Phase 2: Generator First

- 先做 Unity / U3D C# generator
- 生成 route constants + DTO + client facade
- 不要求服务端 runtime 已完成所有 schema 执行

原因：

- 没有 generator，`xmldef` 的价值只完成了一半
- 客户端体验决定了契约系统是否值得维护

### Phase 3: Runtime Execution

- 实现 `XmldefBodyCodec`
- 实现 `DescriptorRegistry`
- 实现 Lua binding registry 编译
- 实现 startup validation

### Phase 4: Generated Service Stubs

- 生成 Lua handler stub
- 生成 send helper
- 生成 route constants

## Non-goals

以下不是本文目标：

- 让 `shield_transport` 在热路径直接读 XML
- 让 runtime 依赖 Unity/U3D plugin
- 让 generator 反向依赖运行时 session/socket API
- 让 Lua 热路径每次收包动态字符串查找 handler
- 让客户端长期手写 route id 和 bytes 编码

## Current Gap vs This Design

当前仓库状态与本文目标之间的差距是：

- 已有：`xmldef` route catalog 加载
- 未有：真实 body schema decode
- 未有：descriptor compiler
- 未有：generator plugin contract
- 未有：Unity/U3D codegen
- 未有：Lua binding registry 编译与启动期校验

因此，后续实现顺序不应是“先把 `xmldef` BodyCodec 写完就结束”，而应是：

1. 先冻结 compiler / descriptor / generator 边界
2. 再实现 Unity/U3D generator
3. 再补服务端 runtime codec + binding

这样 `xmldef` 才是完整的契约系统，而不是只有服务端能看的半成品协议。
