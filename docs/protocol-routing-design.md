# Protocol Routing Design

Shield 的网关协议层不是一条可任意拼接的动态 middleware chain，而是一条固定骨架的 protocol pipeline。
协议适配点按 `Envelope -> RouteExtractor -> RoutePolicy -> BodyCodec` 分层，Lua 边界固定在 `DecodeLocal` 之后。

当前 Phase 1 的配置入口是 `actors[].network.protocol`。启用后，TCP session 会使用 `ProtocolPipeline`；未启用时继续使用 legacy frame path，并调用 `on_client_message(session, payload)`。在 protocol path 中，只有 `DecodeLocal` 结果进入 Lua 的 `on_client_message(session, message)`；`ForwardRaw` 和 `Drop` 留在 C++ 数据面。

当前实现已经把出站方向也收敛到同一套固定骨架：protocol-enabled session 上，Lua 的 `session:send(payload)` 不再直接拼 socket frame，而是统一走 `RouteResolver -> BodyCodec.encode -> Envelope.encode -> socket write`。
这里也要保持 codec 边界干净：`raw` codec 发送字节串；`json` / `msgpack` 这类 structured codec 发送业务消息对象，不接受未建模 raw payload 旁路出站。

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

## Fixed Pipeline

目标运行时流向如下：

```text
Ingress:
  socket bytes
    -> Envelope decode
    -> RouteExtractor
    -> RouteTable / RoutePolicy
       -> Drop / Reject: stop in C++
       -> ForwardRaw: stop in C++ forwarding path
       -> DecodeLocal: BodyCodec decode
    -> Lua business handler

Egress:
  Lua business message
    -> RouteResolver
    -> BodyCodec encode
    -> Envelope encode
    -> socket write
```

这套设计的约束是：

- pipeline 形状固定，不允许按业务任意插 middleware 节点。
- 协议差异体现在固定槽位的实现差异，不体现在链路结构差异。
- Lua 只处理业务消息，不处理 undecoded frame/body。
- forward/drop/reject 都不会泄漏到脚本层。

## Component Model

协议 profile 由几类固定部件组合而成：

| Slot | Ingress Role | Egress Role | Typical Implementations |
| --- | --- | --- | --- |
| `Envelope` | 切帧、组帧，必要时从 header 取基础字段 | 封帧 | `lenprefix`、`idlen`、`typed_len`、`delimiter` |
| `RouteExtractor` | 从 header 或已允许读取的 body 区域提取 route key | 不参与 | `header.route_id`、`body.route` |
| `RoutePolicy` | 根据 route key 决定 `DecodeLocal` / `ForwardRaw` / `Drop` | 通常不参与 | `RouteTable`、default action、unknown route action |
| `BodyCodec` | 仅在 `DecodeLocal` 时 decode | 对业务消息 encode | `json`、`msgpack`、`protobuf`、`sproto`、`xmldef`、`raw` |
| `RouteResolver` | 不参与 | 从业务消息解析出 route 或 route_id | `message.route`、static route、schema/message id mapping |

因此，`json`、`msgpack`、`protobuf`、`sproto`、`xmldef` 这些都不是“任意 pipeline 节点链”，而是固定骨架里 `BodyCodec` 槽位的实现；必要时再配合对应的 `Envelope`、`RouteExtractor` 和 `RouteResolver`。

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

### RouteExtractor

`RouteExtractor` 负责把 frame 映射到 route key。它可以来自 header，也可以来自经过显式允许的 body 路径。

| Source | Meaning | Typical Protocols |
| --- | --- | --- |
| `header.route_id` | route 直接来自 header 字段 | `idlen`、`typed_len`、xmldef/protobuf/sproto/fbs |
| `body.route` | route 需要从 body 中提取 | `json`、`msgpack`、文本协议 |

当 route 来自 body 时，允许发生“最小必要读取”，但这不等于把整个 undecoded payload 暴露给 Lua。body route extract 仍属于 C++ protocol pipeline。

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

如果策略是 `ForwardRaw`，协议层会保留 `raw_frame` 并标记 dispatch action，不解析 body。该结果停留在 C++ forwarding path，不进入 Lua；真正跨服务或跨节点转发应收敛到 transport/cluster 数据面。

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
| `ForwardRaw` | 不解 body，保留 `raw_frame` 供 C++ 转发路径使用，不进入 Lua |
| `Drop` | 丢弃或按安全策略拒绝；默认不应因为 drop 直接断开 session |

### BodyCodec

`BodyCodec` 只在需要读取 body 时执行：

```text
PacketRef.body + RouteEntry(schema_id, codec_id)
  -> BodyCodec.decode()
  -> handler
```

当前 Phase 1 的 actor 配置只允许一个 `body.codec` 实例。`RouteEntry.codec_id` 仍保留为路由元数据和后续扩展位，用于标识包契约；但本地 `DecodeLocal` 仍使用 actor 的单一 `body.codec` 实现，而不是按 route 动态切换多个 codec。

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

第一阶段代码提供 `RawBodyCodec`、`JsonBodyCodec`、`MsgpackBodyCodec` 和若干占位 codec 名称。当前只有 `raw`、`json` 和 `msgpack` 允许稳定进入 `DecodeLocal` 路径：

- `raw` 把 body 作为字节串交给 Lua
- `json` 解码后把业务消息作为 Lua table 交给 Lua；若 body 为 `{route=..., payload=...}` 形态，则进入 Lua 的是 `payload`
- `msgpack` 与 `json` 保持同一业务语义，只是线上 body 表示改为 MessagePack 二进制；若 body 为 `{route=..., payload=...}` 形态，则进入 Lua 的仍然是 `payload`
- `protobuf`、`fbs`、`sproto`、`xmldef` 这些尚未实现真实 decoder 的 codec，当前不能用于 `DecodeLocal`

这条约束的目的很直接：没有真实解码语义的 body，不能越过 transport/Lua 边界冒充“业务消息”。

### RouteResolver

出站方向不再复用入站 `RouteExtractor` 的语义，而是由 `RouteResolver` 负责根据业务消息决定 route：

```text
business message
  -> route / route_id resolve
  -> BodyCodec.encode()
  -> Envelope.encode()
```

常见解析方式：

- 业务消息里自带 `route`
- 由 message type 或 schema id 映射到 route_id
- 由发送目标 service 的静态 profile 决定固定 route

这样进站和出站方向都固定，但职责不同，避免把“读包头”和“写包头”混成一个抽象。

## Actor Network Protocol

完整协议由 envelope、route extract/resolve 规则、catalog/schema、body codec 和 routing policy 组合。当前实现不读取全局 `protocols.profiles`，而是从绑定 TCP listener 的 actor 上读取 `network.protocol`：

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

兼容性说明：

- `routing.default_action` 接受 `forward_raw`，也接受兼容别名 `forward`，启动时会归一化到 `ForwardRaw` 语义。
- `protocol.envelope.max_frame_size` 未显式设置时，继承 listener 级 `network.max_frame_size`。

## Profile Examples

从部件模型看，不同协议只是固定槽位上的不同实现：

| Profile | Envelope | RouteExtractor | BodyCodec | RouteResolver |
| --- | --- | --- | --- | --- |
| `json.simple` | `lenprefix` | `body.route` | `json` | message field route |
| `msgpack.simple` | `lenprefix` | `body.route` | `msgpack` | message field route |
| `xmldef.default` | `idlen` | `header.route_id` | `xmldef` | schema/catalog 映射 |
| `game.protobuf` | `idlen` / `typed_len` | `header.route_id` | `protobuf` | descriptor/message id 映射 |
| `game.sproto` | `idlen` / `typed_len` | `header.route_id` | `sproto` | protocol id/tag 映射 |
| `raw.proxy` | `idlen` / `typed_len` | `header.route_id` | `raw` | static route / route_id |

这也是为什么协议实现的最小要求通常就是两件事：

1. 入站：按固定槽位完成 extract + decode。
2. 出站：按固定槽位完成 resolve + encode。

不需要为每种协议单独发明一条新的业务 pipeline。

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
  -> C++ forwarding path uses raw_frame
  -> no Lua callback
```

只有目标是当前服务的 handler 时，才执行：

```text
BodyCodec(xmldef).decode(body, schema_id)
  -> Lua/C++ handler
```

这就是 header/body 分离的主要收益：转发路径不为业务字段解析付费。

但需要区分两类“转发”：

- **协议转发**：`ForwardRaw`，用于 transport/proxy 数据面
- **业务转发**：`shield.send/call`，用于 service 间协作

长期默认设计应是：

- 进入业务层前，如果能纯转发，就留在 C++ `ForwardRaw` 路径
- 一旦进入业务层，就转换为逻辑消息，再通过 `shield.send/call` 跨 service / 跨节点协作

不要把客户端 raw frame 的长期跨服流转当成默认业务架构。

## Compatibility

现有 `FrameDecoder` 和 `Frame` 保留为未配置 `actors[].network.protocol` 时的兼容路径。配置 `network.protocol` 后使用新的 `Envelope` profile。

推荐迁移顺序：

1. 保留旧 TCP frame 行为。
2. 以 `actors[].network.protocol` 作为唯一运行时配置入口。
3. 对 header-route 协议启用 `RouteTable` 和 `ForwardRaw` 标记。
4. 收敛到统一回调语义：只有 `DecodeLocal` 后的业务消息进入 Lua。
5. 未实现真实 decoder 的 codec 不允许走 `DecodeLocal`。
6. 后续再分别补齐 protobuf、fbs、sproto、xmldef 的真实 `BodyCodec`。
