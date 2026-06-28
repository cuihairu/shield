# Protocol Routing Design

Shield 的协议层按 `Envelope -> PacketRef -> RouteTable -> BodyCodec` 分层。
核心目标是让网关先看 header 做路由，只有本地业务真正需要读取字段时才解码 body。

## Why This Design

旧的固定 frame 设计把“拆 TCP 字节流”和“解析业务 body”耦合在一起。对于 JSON 这类 route 写在 body 里的协议，这种做法问题不大；但对 xmldef、sproto、protobuf、flatbuffers 这类可用消息 ID 或 schema ID 路由的协议，提前解析 body 会浪费 CPU，也会阻塞纯转发路径。

新的设计把热路径压缩为一个轻量 packet：

```cpp
struct PacketRef {
    uint32_t route_id;
    uint16_t kind;
    uint16_t flags;
    uint32_t seq;
    ByteSpan body;
    ByteSpan raw_frame;
};
```

`route_id` 是运行时主键。字符串 route 只用于配置、schema/catalog、Lua 注册和日志，启动时编译成整数 ID。这样路由表可以用 hash map 或数组索引，转发路径不用比较字符串。

## Layers

### Envelope

Envelope 只负责 TCP 字节流切帧，并尽量从 header 提取路由元数据。它不理解 body 格式。

典型 envelope：

| Envelope | Frame | Route Source | Use Case |
| --- | --- | --- | --- |
| `lenprefix` | `[len][body]` | body | JSON、msgpack 简单网关 |
| `idlen` | `[msg_id][len][body]` | header | xmldef、二进制消息表 |
| `typed_len` | `[type][len][body]` | header | protobuf/fbs/sproto profile |
| `delimiter` | `[body]\n` | body | JSON-RPC、文本协议 |
| `websocket` | WebSocket frame | negotiated/header | Web 客户端 |

当前第一阶段代码实现了 `LenPrefixEnvelope`、`IdLenEnvelope`、`TypeLenEnvelope` 和 `DelimiterEnvelope`。

### PacketRef

`PacketRef` 是热路径结构。它只保存 header 可得信息和 body/raw frame 视图：

```text
socket bytes
  -> Envelope.feed()
  -> PacketRef(route_id, flags, seq, body, raw_frame)
```

如果 route 在 header 中，例如 `idlen` 的 `msg_id`，网关可以马上查表决定：

```text
route_id -> target_service + codec_id + schema_id + policy
```

如果目标是远端服务或另一个节点，并且策略是 `ForwardRaw`，网关直接转发 `raw_frame` 或按内部转发协议重包，不需要解析 body。

### RouteTable

`RouteTable` 是运行时路由索引：

```cpp
struct RouteEntry {
    uint32_t route_id;
    uint32_t target_service;
    uint16_t codec_id;
    uint16_t schema_id;
    PacketKind kind;
    std::string debug_name;
    RoutePolicy policy;
};
```

`RoutePolicy` 决定是否本地解码、原样转发或丢弃：

| Action | Meaning |
| --- | --- |
| `DecodeLocal` | 交给对应 `BodyCodec` 解码后进入 Lua/C++ handler |
| `ForwardRaw` | 不解 body，直接转发 |
| `Drop` | 丢弃或按安全策略拒绝 |

### BodyCodec

`BodyCodec` 只在需要读取 body 时执行：

```text
PacketRef.body + RouteEntry(schema_id, codec_id)
  -> BodyCodec.decode()
  -> handler
```

支持的 codec 应作为可插拔实现：

| Codec | Schema Source | Route Mapping |
| --- | --- | --- |
| `json` | none/schema optional | body route 或 header route |
| `msgpack` | none/schema optional | body route 或 header route |
| `protobuf` | descriptor set | service/method 或 message id |
| `fbs` | flatbuffers schema/bfbs | table id/schema id |
| `sproto` | `.sproto` | tag/protocol id |
| `xmldef` | Shield 通用 XML definition/catalog | msg id |
| `raw` | none | no decode, byte passthrough |

第一阶段代码提供 `RawBodyCodec`、`JsonBodyCodec` 和若干 passthrough codec 名称。msgpack/protobuf/fbs/sproto/xmldef 可先参与 header-route 和 lazy decode 流程，后续再替换为真正的 schema decoder。

## Protocol Profiles

完整协议由 envelope、catalog/schema、codec 和 routing policy 组合：

```yaml
protocols:
  profiles:
    json.simple:
      envelope:
        type: lenprefix
        length_bytes: 4
        endian: big
      body:
        codec: json
      routing:
        source: body.route
        lazy_decode: false

    xmldef.default:
      envelope:
        type: idlen
        route_id_bytes: 2
        length_bytes: 2
        endian: little
      body:
        codec: xmldef
        catalog: conf/messages.xml
      routing:
        source: header.route_id
        lazy_decode: true

    game.protobuf:
      envelope:
        type: idlen
        route_id_bytes: 4
        length_bytes: 4
        endian: big
      body:
        codec: protobuf
        descriptor: conf/messages.desc
      routing:
        source: header.route_id
        lazy_decode: true

    game.fbs:
      envelope:
        type: idlen
        route_id_bytes: 4
        length_bytes: 4
        endian: little
      body:
        codec: fbs
        schema: conf/messages.bfbs
      routing:
        source: header.route_id
        lazy_decode: true
```

`xmldef` 只参考“XML catalog 生成 msg_id 路由表”的思路，不照搬任何具体引擎的 entity/base/cell 语义。第一阶段只读取通用 `<message>` / `<route>` 元数据：

```xml
<protocol name="arena">
  <message id="0x1001"
           name="player.move"
           target_service="10"
           action="forward_raw"
           codec_id="1"
           schema_id="33"
           lazy_decode="true" />

  <route id="4098"
         name="auth.login"
         target="1"
         action="decode"
         schema="34"
         lazy_decode="false" />
</protocol>
```

字段级 XML schema 和二进制 body decode 后续由 `xmldef` 的具体 `BodyCodec` 实现补齐，路由阶段不依赖字段解析。

## Forwarding Example

在 header-route 协议中，client 包可以先到网关或代理服务。若 header 的 `msg_id` 映射到远端目标服务，网关不需要解 body：

```text
client tcp bytes
  -> idlen envelope extracts route_id
  -> RouteTable finds target_service
  -> policy = ForwardRaw
  -> forward raw frame/body to target service
```

只有目标是当前服务的 handler 时，才执行：

```text
BodyCodec(xmldef).decode(body, schema_id)
  -> Lua/C++ handler
```

这就是 header/body 分离的主要收益：转发路径不为业务字段解析付费。

## Compatibility

现有 `FrameDecoder` 和 `Frame` 暂时保留，用于当前 TCP session 兼容路径。新的协议模型先作为 `shield_transport` 的增量能力存在，后续 gateway 可以按配置选择旧 `FrameDecoder` 或新 `Envelope` profile。

推荐迁移顺序：

1. 保留旧 TCP frame 行为。
2. 新增 profile 配置和 `Envelope` 实例化。
3. 对 header-route 协议启用 `RouteTable` 和 `ForwardRaw`。
4. 分别补齐 JSON、msgpack、protobuf、fbs、sproto、xmldef 的 `BodyCodec`。
5. 将 Lua gateway handler 从字符串 route 注册编译到整数 `route_id`。
