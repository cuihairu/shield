# Protocol Routing Design

Shield 的网关协议层按 `Envelope -> PacketRef -> RouteTable -> BodyCodec` 分层。
核心目标是让网关先看 header 做路由，只有本地业务真正需要读取字段时才解码 body。

当前 Phase 1 的配置入口是 `actors[].network.protocol`。启用该配置后，TCP session 会使用 `ProtocolPipeline`，并把路由结果转发给 Lua gateway 的 `on_client_packet(session, packet, payload)`；未启用时继续使用 legacy frame path，并调用 `on_client_message(session, payload)`。

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

当前支持的 envelope：

| Envelope | Frame | Route Source | Use Case |
| --- | --- | --- | --- |
| `lenprefix` | `[len][body]` | body | JSON、msgpack 简单网关 |
| `idlen` | `[msg_id][len][body]` | header | xmldef、二进制消息表 |
| `typed_len` | `[type][len][body]` | header | protobuf/fbs/sproto profile |
| `delimiter` | `[body]\n` | body | JSON-RPC、文本协议 |

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

如果策略是 `ForwardRaw`，协议层会保留 `raw_frame` 并标记 dispatch action，不解析 body。当前 Phase 1 只把该结果交给 Lua gateway；真正跨服务或跨节点转发由 gateway 或后续 transport/cluster 能力实现。

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
| `ForwardRaw` | 不解 body，保留 `raw_frame` 供转发路径使用 |
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

## Actor Network Protocol

完整协议由 envelope、catalog/schema、codec 和 routing policy 组合。当前实现不读取全局 `protocols.profiles`，而是从绑定 TCP listener 的 actor 上读取 `network.protocol`：

```yaml
actors:
  - name: gateway
    script: scripts/gateway.lua
    instances: 1
    network:
      tcp: "0.0.0.0:8001"
      protocol:
        name: json.simple
        envelope:
          type: lenprefix
          length_bytes: 4
          endian: big
          max_frame_size: 65536
        body:
          codec: json
        routing:
          source: body.route
          decode_body_route: true
          decode_before_dispatch: false
          unknown_route_action: drop
        routes:
          - id: 1001
            name: login
            target_service: 1
            action: decode
            lazy_decode: false

  - name: xmldef_gateway
    script: scripts/gateway.lua
    instances: 1
    network:
      tcp: "0.0.0.0:8002"
      protocol:
        name: xmldef.default
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
          default_action: forward_raw
```

等价的 profile 片段如下，便于讨论协议本身：

```yaml
json.simple:
  envelope:
    type: lenprefix
    length_bytes: 4
    endian: big
  body:
    codec: json
  routing:
    source: body.route
    decode_body_route: true

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
  -> RouteTable finds route metadata
  -> policy = ForwardRaw
  -> Lua gateway receives packet_info + payload without body decode
```

只有目标是当前服务的 handler 时，才执行：

```text
BodyCodec(xmldef).decode(body, schema_id)
  -> Lua/C++ handler
```

这就是 header/body 分离的主要收益：转发路径不为业务字段解析付费。

## Compatibility

现有 `FrameDecoder` 和 `Frame` 保留为未配置 `actors[].network.protocol` 时的兼容路径。配置 `network.protocol` 后使用新的 `Envelope` profile。

推荐迁移顺序：

1. 保留旧 TCP frame 行为。
2. 以 `actors[].network.protocol` 作为唯一运行时配置入口。
3. 对 header-route 协议启用 `RouteTable` 和 `ForwardRaw` 标记。
4. Lua gateway 使用 `on_client_packet(session, packet, payload)` 接收协议路由结果。
5. 后续再分别补齐 msgpack、protobuf、fbs、sproto、xmldef 的真实 `BodyCodec`。
