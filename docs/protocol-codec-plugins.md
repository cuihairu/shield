# Protocol Codec Plugins

本文定义网络协议 `BodyCodec` 的插件化边界。目标是让核心 `shield_transport` 保持稳定、轻量、可审计，同时让 protobuf、sproto、xmldef-native、flatbuffers、msgpack 等协议能力按需启用。

## Decision

默认核心只内置最小协议能力：

| Codec | 是否默认内置 | 原因 |
| --- | --- | --- |
| `raw` | yes | 兼容旧 frame path、代理/转发和最小字节串调试。 |
| `json` | yes | Lua table 语义天然匹配，配置和排障成本最低，无额外协议 runtime。 |
| `msgpack` | no target | 已有可选插件形态；当前核心内置仍作为过渡兼容路径保留。 |
| `protobuf` | no | 需要 descriptor、DynamicMessage、版本兼容和 schema 映射，必须隔离到插件。 |
| `sproto` | no | 需要独立 runtime/compiler 适配，必须隔离到插件。 |
| `xmldef-native` | no | 属于 descriptor/toolchain/runtime 三层系统，不应直接塞进核心。 |
| `flatbuffers` | no | 需要 bfbs/schema runtime，必须隔离到插件。 |

过渡期说明：当前代码里 `msgpack` 仍是可用内置 codec，同时也提供 `protocol.msgpack` 可选插件用于验证插件化路径；`protobuf` 通过 `protocol.protobuf` 插件启用；`sproto/xmldef/fbs` 仍是占位 codec 名称。后续实现应收敛到本页口径：核心只保证 `raw/json`，其他协议通过显式插件启用。迁移完成前，文档和测试必须区分“当前实现快照”和“目标架构口径”。

## Non-Goals

- 不引入按包自动探测协议类型。
- 不允许同一条 session 在业务包中途切换 codec。
- 不让插件直接继承或实现 host 内部 C++ `BodyCodec` 类。
- 不跨 DLL/SO 边界传递 `nlohmann::json`、STL 容器、异常或 C++ 对象所有权。
- 不把 protobuf、sproto、xmldef 的 schema 编译器硬链接进 `shield_transport`。

## Runtime Model

核心仍然只拥有固定 pipeline：

```text
Envelope -> RouteExtractor -> RoutePolicy -> BodyCodec -> Lua/C++ handler
Lua/C++ message -> RouteResolver -> BodyCodec -> Envelope
```

插件只填充 `BodyCodec` 语义，不改变 pipeline 形状。插件可参与：

- 根据 `RouteEntry.schema_id` / `route_id` 找到 schema。
- 把 inbound payload bytes 解码成 canonical JSON message。
- 把 outbound canonical JSON message 编码成 payload bytes。
- 可选从 payload 提取 body route key，但 header-route 协议优先使用 `Envelope` 提取的 route id。

插件不参与：

- TCP 切帧和 socket I/O。
- session 生命周期和 backpressure。
- Lua gateway 调度。
- `ForwardRaw` 数据面。

## Plugin Interface

建议新增 C ABI interface：`shield.protocol.codec.v1`。

接口命名遵循插件系统 v1 的 provider 模型：

```yaml
provides:
  - interface: shield.protocol.codec.v1
    capabilities: [protobuf]
```

主配置显式启用实例与 binding：

```yaml
plugins:
  instances:
    - id: protocol.protobuf
      package: protocol.protobuf
      required: true
      config:
        descriptor_set: conf/game.pb
        route_map: conf/protobuf-routes.json

  bindings:
    protocol.protobuf: protocol.protobuf
```

actor 绑定协议时显式引用 provider：

```yaml
actors:
  - name: gateway
    script: scripts/gateway.lua
    network:
      tcp: "0.0.0.0:8001"
      protocol:
        name: game.protobuf
        envelope:
          type: idlen
          route_id_bytes: 4
          length_bytes: 4
          endian: big
        body:
          codec: protobuf
          provider: protocol.protobuf
        routing:
          source: header.route_id
          unknown_route_action: drop
        routes:
          - id: 1001
            name: auth.LoginRequest
            schema_id: 1
            action: decode
            lazy_decode: false
```

`provider` 是 binding 逻辑名，不是 plugin instance id。这样部署可以替换 `protocol.protobuf` 指向的具体实例，而不修改 actor 业务配置。

## ABI Shape

ABI 应保持 C-compatible，所有复杂数据使用 bytes 或 JSON string 表达：

```c
typedef struct shield_protocol_codec_v1 {
    uint32_t struct_size;
    const char* codec_name;
    const char* version;
    void* user_data;

    int (*decode)(const struct shield_protocol_codec_v1* self,
                  const shield_protocol_decode_args_v1* args,
                  shield_protocol_decode_result_v1* out,
                  shield_error_v1* err);

    int (*encode)(const struct shield_protocol_codec_v1* self,
                  const shield_protocol_encode_args_v1* args,
                  shield_protocol_encode_result_v1* out,
                  shield_error_v1* err);

    void (*free_decode_result)(
        const struct shield_protocol_codec_v1* self,
        shield_protocol_decode_result_v1* result);

    void (*free_encode_result)(
        const struct shield_protocol_codec_v1* self,
        shield_protocol_encode_result_v1* result);
} shield_protocol_codec_v1;
```

最小参数语义：

| 字段 | 方向 | 说明 |
| --- | --- | --- |
| `route_id` | decode/encode | 当前 route id。 |
| `schema_id` | decode/encode | `RouteEntry.schema_id`，由路由表或 catalog 给出。 |
| `route_name` | decode/encode | 调试名或 descriptor message name。 |
| `payload` | decode | inbound body bytes。 |
| `message_json` | encode | canonical JSON message。 |
| `out_message_json` | decode | 解码后的 canonical JSON message。 |
| `out_payload` | encode | 编码后的 body bytes。 |

错误规则：

- schema 缺失：返回 `protocol.schema_not_found`。
- payload 不合法：返回 `protocol.decode_failed`。
- message 不符合 schema：返回 `protocol.encode_failed`。
- provider 未配置或不可用：返回 `protocol.codec_unavailable`。

## Core Adapter

核心侧应新增一个很薄的适配器，例如 `ExternalBodyCodec`：

```text
ExternalBodyCodec
  -> holds provider binding name
  -> resolves shield.protocol.codec.v1 through PluginHost
  -> converts DecodedBody <-> ABI args/results
  -> preserves BodyCodec interface for ProtocolPipeline
```

这样 `ProtocolPipeline` 不需要知道 protobuf/sproto/xmldef 的具体实现，也不需要链接第三方 runtime。

## Protobuf First

protobuf 是第一个落地插件，范围必须保持最小：

1. 只支持 binary protobuf payload，不实现 gRPC。
2. descriptor 输入先支持 `FileDescriptorSet` 文件。
3. route 映射先由显式 `route_map` 或 `routes[].schema_id/name` 决定。
4. inbound decode 输出 canonical JSON message，交给 Lua table。
5. outbound encode 接受 Lua table/canonical JSON message，生成 protobuf bytes。
6. 不做 per-message codec 切换，不做自动反射路由发现，不做服务方法 RPC 语义。

protobuf 插件的最小配置：

```yaml
plugins:
  instances:
    - id: protocol.protobuf.game
      package: protocol.protobuf
      config:
        descriptor_set: conf/game.pb
        messages:
          - schema_id: 1
            name: auth.LoginRequest
          - schema_id: 2
            name: auth.LoginResponse

  bindings:
    protocol.protobuf: protocol.protobuf.game
```

协议 profile 的最小配置：

```yaml
network:
  protocol:
    name: game.protobuf
    envelope:
      type: idlen
      route_id_bytes: 4
      length_bytes: 4
      endian: big
    body:
      codec: protobuf
      provider: protocol.protobuf
    routing:
      source: header.route_id
    routes:
      - id: 1001
        name: auth.LoginRequest
        schema_id: 1
        action: decode
        lazy_decode: false
```

## Other Protocols

| Protocol | Target Form | Notes |
| --- | --- | --- |
| `msgpack` | bundled optional plugin | 已落地 `protocol.msgpack` provider，复用当前 JSON 语义；核心内置路径待后续迁移/兼容策略收敛。 |
| `sproto` | runtime codec plugin | 需要 `.sproto` loader/compiler adapter 和 package envelope 适配。 |
| `xmldef-native` | descriptor/runtime plugin | catalog 路由加载可留在核心；字段级 decode/encode 走插件。 |
| `flatbuffers` | runtime codec plugin | 需要 bfbs/schema loader 和 JSON bridge。 |

## Implementation Order

当前推荐顺序和状态：

1. 已冻结本文和 [Protocol Routing Design](protocol-routing-design.md) 的插件口径。
2. 已新增 `shield.protocol.codec.v1` ABI 头文件和 `ExternalBodyCodec` 适配器。
3. 已保留 builtin `raw/json` 工厂；`msgpack` 暂作为过渡兼容内置 codec。
4. 已让 `body.provider` 触发插件 codec 路径，并覆盖 provider 缺失/codec 不匹配测试。
5. 已实现 protobuf 插件的 `FileDescriptorSet`、schema/route 映射、decode、encode 和真实 descriptor round-trip 测试。
6. 已补 Lua ingress/egress 测试：protobuf codec session 的 `session:send(table)`，以及 fake provider pipeline decode 后进入 Lua 并 echo 出站。
7. 下一步在可用 vcpkg/protobuf 环境中跑通 `test_protocol_protobuf_plugin` 和 Lua gateway 测试。
8. 已新增 `protocol.msgpack` provider 和插件 ABI round-trip 测试；下一步决定是否移除核心内置 msgpack 或保留为兼容构建开关。
