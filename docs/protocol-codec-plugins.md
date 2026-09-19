# Protocol Codec Plugins

本文定义网络协议 `BodyCodec` 的插件化边界。目标是让核心 `shield_transport` 保持稳定、轻量、可审计，同时让 protobuf、xmldef-native、flatbuffers、msgpack 等协议能力按需启用。

## Decision

默认核心只内置最小协议能力：

| Codec | 是否默认内置 | 原因 |
| --- | --- | --- |
| `raw` | yes | 兼容旧 frame path、代理/转发和最小字节串调试。 |
| `json` | yes | Lua table 语义天然匹配，配置和排障成本最低，无额外协议 runtime。 |
| `msgpack` | no | 已有 `protocol.msgpack` 插件；核心不再内置，需通过 `body.provider` 启用。 |
| `protobuf` | no | 需要 descriptor、DynamicMessage、版本兼容和 schema 映射，必须隔离到插件。 |
| `xmldef-native` | no | 属于 descriptor/toolchain/runtime 三层系统，不应直接塞进核心。 |
| `flatbuffers` | no | 需要 fbs/schema runtime，必须隔离到插件。 |

核心只内置 `raw` 和 `json`。`msgpack` 已迁移至 `protocol.msgpack` 插件，需通过 `body.provider` 显式启用；`protobuf`/`flatbuffers` 分别通过 `protocol.protobuf` / `protocol.flatbuffers` 插件启用；`xmldef` 仍是占位 codec 名称。

## Non-Goals

- 不引入按包自动探测协议类型。
- 不允许同一条 session 在业务包中途切换 codec。
- 不让插件直接继承或实现 host 内部 C++ `BodyCodec` 类。
- 不跨 DLL/SO 边界传递 `nlohmann::json`、STL 容器、异常或 C++ 对象所有权。
- 不把 protobuf、xmldef 的 schema 编译器硬链接进 `shield_transport`。

## Runtime Model

核心仍然只拥有固定 pipeline：

```text
Envelope -> RouteExtractor -> RoutePolicy -> BodyCodec -> Lua/C++ handler
Lua/C++ message -> RouteResolver -> BodyCodec -> Envelope
```

插件只填充 `BodyCodec` 语义，不改变 pipeline 形状。插件可参与：

- 按插件收到的 `route_name`（host 编译期解析好的最终 schema 类型名）找到 schema。
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

  bindings:
    protocol.protobuf: protocol.protobuf
```

actor 绑定协议时显式引用 provider：

```yaml
actors:
  - name: gateway
    script: scripts/auth.lua
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
| `route_name` | decode/encode | host 编译期解析好的最终 schema 类型名（见下节）。 |
| `payload` | decode | inbound body bytes。 |
| `message_json` | encode | canonical JSON message。 |
| `out_message_json` | decode | 解码后的 canonical JSON message。 |
| `out_payload` | encode | 编码后的 body bytes。 |

错误规则：

- schema 缺失：返回 `protocol.schema_not_found`。
- payload 不合法：返回 `protocol.decode_failed`。
- message 不符合 schema：返回 `protocol.encode_failed`。
- provider 未配置或不可用：返回 `protocol.codec_unavailable`。

## Schema 寻址收敛

### 决策

**寻址决策上收到 host 编译期，插件降为纯「名字 → 类型」单级查找。**

插件 ABI 只暴露一个寻址键：

```text
route_name = request_schema（显式覆盖，非空时）
           ∨ route name（同名约定，默认）
```

host 在 descriptor → `RouteEntry` 编译期完成上述解析（产物为
`RouteEntry.schema_name`），decode/encode 调用插件时直接传入最终
类型名。`decode_args_v1` / `encode_args_v1` 不再携带 `route_id` /
`codec_id` / `schema_id`。

### 为什么这么设计

1. **多路径寻址是复杂度源，不是能力。** 此前存在三级寻址链
   （`schema_id` 显式映射 → `route_id` 映射 → `route_name` 同名约定），
   由插件静默 fallback：三种配置方式并存但只生效一种，配多了优先级
   覆盖不报错，排查问题时必须先弄清「哪一级生效了」。三级逻辑还在
   每家有 schema 的插件里各重复实现一份。

2. **纸面灵活性没有真实用户。** 复盘发现 JSON routes 路径上的
   `schema_id` 键从未被解析过——`RpcDescriptor` 根本没有这个字段，
   文档示例里的 `schema_id: 1` 是摆设。「灵活」只存在于插件配置的
   `messages` 映射里，而它的场景（schema 类型名与路由名不一致）
   由 `request_schema` 一个字段就能覆盖。

3. **边界最小面原则。** `route_id` / `codec_id` / `schema_id` 都是
   host 内部的路由与注册表概念：route 分派在 `RouteTable` 完成、
   codec 分派在 `BodyCodecRegistry` 完成——分派完成后插件拿到的
   请求已经确定了，这些 id 对插件没有业务意义（错误日志用
   `route_name` 可读性更好）。插件 ABI 应该只暴露插件真正需要的
   信息：最终 schema 类型名和字节。

4. **保留 `request_schema` 不是技术债，是有意的单点扩展位。**
   同名约定（route name = schema 类型全名）覆盖绝大多数路由；
   当 schema 命名无法跟随路由命名演进时（protobuf 包名前缀、多个
   路由复用同一 message），`request_schema` 是唯一的显式逃生门。
   一条规则 + 一个逃生门，除此之外没有第三种可能——出错面唯一。

5. **`response_schema` 死字段直接删除，而不是兑现它。** 它曾在
   descriptor 声明但 ABI 与管线均未消费。req/resp 不同类型名的
   表达（方向不对称 schema）需要 ABI 扩展与出站寻址设计，属于
   Phase 2；现状 s2c 独立路由声明已覆盖主要场景。保留一个不生效
   的字段只会让配置者误以为它有语义。

### 插件侧的义务

codec 插件不得自行实现寻址 fallback，也不得读取 `route_name`
以外的路由上下文去做 schema 选择。`schema not found` 是配置错误，
在插件边界直接返回 `protocol.schema_not_found`，由启动期与
运行期统一暴露。

## Core Adapter

核心侧应新增一个很薄的适配器，例如 `ExternalBodyCodec`：

```text
ExternalBodyCodec
  -> holds provider binding name
  -> resolves shield.protocol.codec.v1 through PluginHost
  -> converts DecodedBody <-> ABI args/results
  -> preserves BodyCodec interface for ProtocolPipeline
```

这样 `ProtocolPipeline` 不需要知道 protobuf/xmldef 的具体实现，也不需要链接第三方 runtime。

## Protobuf First

protobuf 是第一个落地插件，范围必须保持最小：

1. 只支持 binary protobuf payload，不实现 gRPC。
2. descriptor 输入先支持 `FileDescriptorSet` 文件。
3. route → schema 映射由同名约定决定；例外用路由的 `request_schema` 显式覆盖。
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
        action: decode
        lazy_decode: false
```

## Other Protocols

| Protocol | Target Form | Notes |
| --- | --- | --- |
| `msgpack` | bundled optional plugin | 已落地 `protocol.msgpack` provider，核心已移除内置路径。 |
| `xmldef-native` | descriptor/runtime plugin | catalog 路由加载可留在核心；字段级 decode/encode 走插件。 |
| `flatbuffers` | runtime codec plugin | 已落地 `protocol.flatbuffers` provider，支持 `.fbs` 文本 schema 和 JSON 桥接。 |

## Implementation Order

当前推荐顺序和状态：

1. 已冻结本文和 [Protocol Routing Design](protocol-routing-design.md) 的插件口径。
2. 已新增 `shield.protocol.codec.v1` ABI 头文件和 `ExternalBodyCodec` 适配器。
3. 核心内置 `raw/json` 工厂；`msgpack` 等插件 codec 名在 `create_body_codec` 中保留 `PassthroughBodyCodec` 占位（仅供直接单测）。`build_protocol_pipeline_from_json` 对非内置 codec 在缺少 `body.provider` 或 provider 解析失败时直接构建报错（bootstrap 启动期 probe 即失败），不再静默安装运行时才爆炸的占位 codec。
4. 已让 `body.provider` 触发插件 codec 路径；codec vtable 在监听器启动时解析一次并被 pipeline 工厂捕获（插件不可热卸载，shutdown 先拆 listener/session 再关 PluginHost，vtable 不会悬空），并覆盖 provider 缺失/codec 不匹配/无 provider 的构建期报错测试。
5. 已实现 protobuf 插件的 `FileDescriptorSet`、schema/route 映射、decode、encode 和真实 descriptor round-trip 测试。
6. 已实现 inbound decode → Lua：codec 插件解码出的 canonical JSON message 作为 `ClientIngress.decoded_request` 随行到目标 VM，直接成为编译绑定 `handler(ctx, client, request)` 的 request table（无 codec 插件解码时按 descriptor 契约由目标 VM 解码或传原始字节），并有 fake provider pipeline 的端到端测试覆盖。Lua egress 自动编码（Lua table → 插件 encode → wire bytes）尚未实现，业务侧仍需显式编码后经 `shield.client_rpc` 出站。
7. 已在 CI 中启用 `SHIELD_BUILD_PLUGIN_PROTOBUF=ON` / `SHIELD_BUILD_PLUGIN_MSGPACK=ON`，protobuf 插件的 `test_protocol_protobuf_plugin` 和 `test_protocol_msgpack_plugin` 在 Ubuntu/macOS/Windows 三平台 CI 中通过。
8. 已新增 `protocol.msgpack` provider 和插件 ABI round-trip 测试；已移除核心内置 `MsgpackBodyCodec`，`msgpack` 现为纯插件 codec。
10. 已新增 `protocol.flatbuffers` provider，支持 `.fbs` 文本 schema 加载、decode/encode 和 JSON 桥接。

## Known Limitations

- codec 按监听器锁定（`actors[].network.protocol.body`），不支持 per-route / per-session 协商（Non-Goal）。
- Lua egress 不会自动调用插件 encode；只有 inbound decode 的结果会作为 Lua table 交付。
- flatbuffers 插件使用 `.fbs` 文本 schema，运行时通过 `Parser::Parse` 加载并编译。
