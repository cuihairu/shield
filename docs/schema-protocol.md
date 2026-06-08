# Schema Protocol

Shield 的 schema protocol 负责把协议定义、二进制编码和 RPC 异步语义拆开。

## 设计目标

1. XML 只作为协议定义源，不进入热路径。
2. 运行时加载 schema，构建 message descriptor。
3. 传输层保持紧凑二进制编码，避免依赖 protobuf。
4. RPC 返回 `future` / `promise` 风格结果，供不同语言做各自封装。
5. 方向与语义分离，避免把 `oneway` 当成方向。

## 核心概念

- `direction`: `c2s`、`s2c`、`bidi`
- `kind`: `rpc`、`event`、`command`、`stream`
- `correlation_id`: RPC 请求响应配对
- `stream_id` / `sequence`: 流式消息分片和重组
- `schema_hash`: 客户端和服务端握手时校验 schema 一致性

## XML 示例

```xml
<schema>
  <message name="LoginRequest" id="1001" kind="rpc" direction="c2s" timeout_ms="3000">
    <field name="username" id="1" type="string" required="true"/>
    <field name="password" id="2" type="string" required="true"/>
  </message>

  <message name="LoginReply" id="1002" kind="rpc" direction="s2c">
    <field name="token" id="1" type="string"/>
  </message>

  <message name="ChatBroadcast" id="2001" kind="event" direction="s2c">
    <field name="channel" id="1" type="string"/>
    <field name="content" id="2" type="string"/>
  </message>
</schema>
```

## C++ API

```cpp
shield::protocol::SchemaRegistry registry;
registry.load_from_xml_file("protocol.xml");

auto task = pending.create<std::string>(request_id, std::chrono::seconds(3));
task->on_ok([](const std::string& value) {});
task->on_err([](const shield::protocol::RpcError& error) {});
```

## RPC 语义

RPC 通过 `correlation_id` 关联请求和响应。协议层只关心结果是否成功，以及失败原因；`on_ok` / `on_err` 是 SDK 层封装。这样 C++、C#、TS、Lua 都可以映射成自己的异步接口。

## 前端插件边界

前端引擎插件不应该依赖 Shield 服务端插件系统。更合理的形态是：

- Unity: C# package
- Unreal: UE module
- Godot: GDExtension
- Web: TypeScript package
- Native: C++ SDK

这些包共享同一份 schema 和编码规则，只负责接入各自引擎的生命周期和网络层。

## 当前实现

- `include/shield/protocol/schema_protocol.hpp`
- `src/protocol/schema_protocol.cpp`
- `tests/unit/protocol/test_schema_protocol.cpp`

