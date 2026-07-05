# Xmldef Unity Generator Spec

> Status: draft / deferred tooling spec.
>
> 本文定义 Unity / U3D 侧 `xmldef` generator plugin 的输入、输出、接口、代码组织和约束。目标是让客户端团队拿到同一份 descriptor 后，能稳定生成 C# DTO、route 常量、gateway facade 和 handler stub，而不是手写协议。

## Scope

本文只定义 Unity/U3D generator plugin：

- 输入 descriptor 契约
- generator plugin 接口
- 输出文件集合
- 生成代码的命名与布局
- runtime facade 边界

不定义：

- Unity 网络传输实现细节
- Web / Unreal generator
- 服务端 runtime `XmldefBodyCodec`
- 编辑器 UI 体验

如果当前关注的是先冻结哪部分 descriptor 输出、runtime 最小执行面和 compiler MVP 范围，请结合 [Xmldef Phase 1 Implementation Plan](xmldef-phase1-implementation.md) 一起阅读。
如果当前关注的是服务端 runtime 和 compiler 模块边界，请结合 [Xmldef Compiler / Runtime MVP](xmldef-compiler-runtime-mvp.md) 一起阅读。

## Inputs

generator 最小输入：

- `descriptor.bin`
- `descriptor.debug.json`
- `route_constants.json`
- generator options

推荐 generator 主要消费 `descriptor.debug.json`，`descriptor.bin` 作为一致性校验来源。

### Required Descriptor Fields

Unity generator 依赖以下 descriptor 字段：

- package id
- package version
- code namespace
- method full name
- route id
- schema id
- direction
- request type
- response type
- item type
- field layout
- enum definitions

缺任何一项都应直接 fail fast。

## Plugin Contract

### Positioning

这是 **tooling plugin**，不是 runtime `shield_plugin`。

原因：

- 它运行在开发期，不运行在服务端 host
- 它面向 Unity 项目产物生成
- 它不应接入 `shield_plugin` 的实例/binding/ABI 生命周期

### Minimal Interface

最小接口建议：

```text
generator_plugin {
  name: string
  version: string
  target: "unity-csharp"
  generate(input, output_dir, options) -> result
}
```

`input` 至少包含：

- descriptor paths
- package metadata

`result` 至少包含：

- generated file list
- warnings
- errors

### Generator Options

推荐最小选项：

- `namespace_override`
- `output_root`
- `runtime_namespace`
- `dto_style` (`class` / `struct`)
- `emit_handler_interface` (`true/false`)
- `emit_partial_stubs` (`true/false`)
- `emit_route_constants` (`true/false`)

## Output Layout

推荐输出目录：

```text
generated/csharp/
  Game.Protocol/
    RouteIds.cs
    XmldefCodec.cs
    GatewayClient.cs
    IXmldefHandler.cs
    Messages/
      Avatar/
        MoveRequest.cs
      Login/
        ReqAuth.cs
    Handlers/
      XmldefHandlerStub.cs
```

生成器必须保证：

- 文件路径稳定
- 命名空间稳定
- 同一 descriptor 重复生成结果可重复

## Generated Artifacts

### 1. RouteIds.cs

作用：

- 提供稳定 route 常量
- 避免业务层手写 magic number

示意：

```csharp
namespace Game.Protocol {
    public static class RouteIds {
        public const uint AvatarMove = 0x1001;
        public const uint LoginReqAuth = 0x1002;
    }
}
```

### 2. DTO Types

每个 request / response / item type 生成 C# DTO。

示意：

```csharp
namespace Game.Protocol.Avatar {
    public sealed class MoveRequest {
        public int EntityId { get; set; }
        public float X { get; set; }
        public float Y { get; set; }
        public float Z { get; set; }
    }
}
```

规则：

- 默认生成 `class`，除非 generator option 指定 `struct`
- 字段名从 schema 名字稳定映射
- enum 生成独立 C# enum
- list/map 映射到稳定的 .NET 集合类型

### 3. XmldefCodec.cs

作用：

- 包装 descriptor 驱动的 encode/decode
- 对上层隐藏 raw route/schema 操作

最小接口建议：

```csharp
public interface IXmldefCodec {
    ArraySegment<byte> Encode(uint routeId, object message);
    object Decode(uint routeId, ReadOnlySpan<byte> payload);
}
```

如果客户端 runtime 最终是 descriptor-driven 动态 codec，这个文件也可以只是 typed facade。

### 4. GatewayClient.cs

作用：

- 提供 typed send/call API
- 避免业务层直接操作 route id

示意：

```csharp
public sealed class GatewayClient {
    public void SendAvatarMove(Game.Protocol.Avatar.MoveRequest req) { ... }
    public Task<Game.Protocol.Login.ReqAuthReply> CallLoginReqAuthAsync(
        Game.Protocol.Login.ReqAuthRequest req) { ... }
}
```

规则：

- `send` -> 同步或 fire-and-forget API
- `call` -> `Task<T>`
- `stream` -> `IAsyncEnumerable<T>` 或 callback wrapper，后续可扩展

### 5. IXmldefHandler.cs

作用：

- 给客户端 inbound dispatch 提供强类型 handler 接口

示意：

```csharp
public interface IXmldefHandler {
    void OnAvatarMove(Game.Protocol.Avatar.MoveRequest msg);
    void OnLoginNotice(Game.Protocol.Login.Notice msg);
}
```

如果 method direction 不允许客户端接收，该 method 不应进入 handler interface。

### 6. Optional Partial Stubs

为了降低接入成本，可以生成：

```csharp
public partial class GatewayClient {
}
```

或：

```csharp
public partial class XmldefHandlerBase : IXmldefHandler {
    public virtual void OnAvatarMove(MoveRequest msg) {}
}
```

生成器不能覆盖用户手写 partial 文件。

## Naming Rules

### Namespace

默认命名空间来自 descriptor 的 `code_namespace`。

例如：

```text
code_namespace = "Game.Protocol"
```

生成：

- `Game.Protocol`
- `Game.Protocol.Avatar`
- `Game.Protocol.Login`

### Method Name Mapping

`Avatar.move` 建议映射为：

- route constant: `AvatarMove`
- send method: `SendAvatarMove`
- handler method: `OnAvatarMove`

### Field Name Mapping

schema 若使用 `snake_case`，生成器可映射到 PascalCase：

- `entity_id` -> `EntityId`
- `room_id` -> `RoomId`

该规则必须稳定且文档化，不能同一项目里变化。

## Direction Mapping

generator 必须遵守 descriptor `direction`：

- `client_to_server`: 生成发送 API，不生成客户端入站 handler
- `server_to_client`: 生成客户端 handler，不生成主动发送 API
- `bidirectional`: 两者都生成
- `server_to_server`: Unity generator 默认忽略

## Threading Boundary

Unity/U3D 的关键约束之一是主线程模型。

因此 generator 生成的客户端 facade 应明确分层：

- transport / socket callback 线程
- main-thread dispatch adapter
- user handler

建议不要把“强制切主线程”写死在 DTO 或 codec 中，而是在 facade 层预留：

```csharp
public interface IMainThreadDispatcher {
    void Post(Action action);
}
```

是否切主线程由 Unity runtime adapter 决定，不由 descriptor 或 DTO 决定。

## Versioning

生成代码应保留 descriptor 元信息：

- package id
- package version
- schema root hash

建议生成：

```csharp
public static class XmldefSchemaInfo {
    public const string PackageId = "game";
    public const string Version = "1.0.0";
    public const string SchemaRootHash = "...";
}
```

用于握手、自检和日志。

## Failure Rules

generator 必须在这些情况 fail fast：

- descriptor 缺少 required fields
- route id 重复
- method 名映射后冲突
- field 名映射后冲突
- unsupported type 出现
- server-only 类型被要求导出到客户端

不能静默跳过。

## Non-goals

本文明确不做：

- 直接生成 socket 库实现
- 直接生成 Unity Editor 面板
- 让业务层直接面对 `route_id`
- 让客户端手写二进制 encode/decode
- 把 Unity generator 接入服务端 plugin host

## Recommended Delivery Order

实现顺序建议：

1. 先冻结 [Xmldef Descriptor Spec](xmldef-descriptor-spec.md)
2. 再实现最小 Unity generator
3. 先产出 `RouteIds.cs` + DTO + `GatewayClient.cs`
4. 再补 handler interface / partial stubs
5. 最后再做更复杂的主线程调度适配

原因：

- route constants + DTO + facade 已能显著消灭手写协议
- 主线程策略和具体网络库适配可后置

## Success Criteria

如果实现正确，Unity 业务代码应接近：

```csharp
client.SendAvatarMove(new MoveRequest {
    EntityId = 7,
    X = 10,
    Y = 20,
    Z = 30,
});
```

而不是：

```csharp
var bytes = ManualEncode(...);
socket.Send(bytes);
```

这就是 `xmldef` generator 的最低价值线。
