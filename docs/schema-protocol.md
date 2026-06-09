# Schema Protocol

Shield 的 schema protocol 是一套定义驱动的协议契约系统。目标是让游戏服务器和客户端共享同一份 XML 契约，运行时加载编译后的 descriptor，不要求业务开发者在协议变更后重新编译 C++ 协议桩。

本文描述目标设计。当前仓库中的 `include/shield/protocol/schema_protocol.hpp`、`src/protocol/schema_protocol.cpp` 和 `tests/unit/protocol/test_schema_protocol.cpp` 是早期实验实现，尚未完整覆盖本文的目录化 XML、descriptor package、Merkle 增量和客户端插件模型。

源码目录、C++ namespace、工具链 target、生成物目录和实现迁移规则见 [Schema Implementation Layout](schema-implementation.md)。

## 设计目标

1. XML 作为协议契约源文件，支持 XSD 和语义强校验。
2. 运行时加载 `descriptor.bin`，不直接解析 XML 进入热路径。
3. 客户端运行时只依赖 `descriptor.bin` 即可动态编码、解码和分发。
4. 开发期同时生成可读 descriptor、强类型 wrapper 和文档，避免客户端开发者面对二进制文件。
5. Payload 编码尽量遵循 Protobuf wire format 规则，但服务语义由 Shield 自己定义。
6. `send`、`call`、`stream` 与方向解耦，方向只描述谁可以发起。
7. 通过 Merkle tree 支持精确变更定位和后续 descriptor 热更新。
8. 数据库 mapper 共享类型系统，但不和 RPC DTO 直接耦合。

## 核心分层

```text
XML contract files
  -> XSD validation
  -> semantic validation
  -> canonical IR
  -> descriptor.bin
  -> descriptor.debug.json
  -> optional typed wrappers
  -> docs
```

运行时只消费 `descriptor.bin`。`descriptor.debug.json`、语言绑定代码和文档是开发期体验产物，不是协议执行的必要条件。

## 文件组织

协议文件按业务域组织，而不是放在一个巨大的 XML 里：

```text
protocol/
  manifest.xml
  common/
    types.xml
    errors.xml
  account/
    types.xml
    services.xml
    mappers.xml
  player/
    types.xml
    services.xml
    mappers.xml
  room/
    types.xml
    services.xml
    mappers.xml
```

`manifest.xml` 是唯一入口：

```xml
<protocol-manifest name="game" version="1.8.3">
  <module name="common" path="common"/>
  <module name="account" path="account"/>
  <module name="player" path="player"/>
  <module name="room" path="room"/>
</protocol-manifest>
```

依赖方向必须保持简单：

- `types.xml` 可以引用 `common` 类型。
- `services.xml` 可以引用类型和错误集。
- `mappers.xml` 可以引用类型和实体映射。
- `services.xml` 不直接依赖 `mappers.xml`。
- `mappers.xml` 不直接依赖 `services.xml`。
- 不允许循环依赖。

## 类型系统

`types.xml` 定义可复用数据结构，是 RPC、客户端 SDK 和 mapper 的公共类型来源。它只描述数据，不描述行为。

```xml
<types namespace="player">
  <enum name="PlayerState">
    <item name="Offline" value="0"/>
    <item name="Online" value="1"/>
    <item name="InRoom" value="2"/>
  </enum>

  <struct name="PlayerProfile">
    <field name="player_id" id="1" type="string"/>
    <field name="nickname" id="2" type="string"/>
    <field name="level" id="3" type="int32" default="1"/>
    <field name="state" id="4" type="player.PlayerState"/>
  </struct>
</types>
```

第一版建议支持：

- scalar: `bool`、`int32`、`int64`、`uint32`、`uint64`、`sint32`、`sint64`、`float`、`double`、`string`、`bytes`
- `enum`
- `struct`
- `list<T>`
- `map<K,V>`
- `optional`
- 默认值
- `deprecated`

`required` 可以作为校验规则存在，但不要成为 wire format 层的 required 字段。默认应按 optional 设计，避免协议演进困难。

更详细的 `types.xml` 语法、XSD 范围、语义校验、兼容性校验和运行时数据校验见 [Schema Types](schema-types.md)。

## 错误模型

错误是一等契约，不能只是字符串。公共错误结构放在 `common/errors.xml`：

```xml
<types namespace="common">
  <enum name="ErrorCategory">
    <item name="Transport" value="1"/>
    <item name="Timeout" value="2"/>
    <item name="Validation" value="3"/>
    <item name="Auth" value="4"/>
    <item name="Business" value="5"/>
    <item name="Internal" value="6"/>
    <item name="Stream" value="7"/>
  </enum>

  <struct name="ShieldError">
    <field name="code" id="1" type="int32"/>
    <field name="name" id="2" type="string"/>
    <field name="category" id="3" type="common.ErrorCategory"/>
    <field name="message" id="4" type="string"/>
    <field name="retryable" id="5" type="bool" default="false"/>
    <field name="details" id="6" type="map<string,string>" optional="true"/>
  </struct>

  <error-set name="CommonErrors">
    <error code="1001" name="TIMEOUT" category="Timeout" retryable="true"/>
    <error code="1002" name="SCHEMA_MISMATCH" category="Validation"/>
    <error code="1003" name="UNAUTHORIZED" category="Auth"/>
    <error code="1004" name="INTERNAL_ERROR" category="Internal"/>
  </error-set>
</types>
```

业务模块可以定义自己的 `error-set`。`call` 的 `err` 和 `stream` 的 `on_err` 都统一使用 `common.ShieldError`。

## 服务契约

服务层只定义交互契约，不定义业务流程。

语义使用三种基础操作：

- `send`: 单向发送，不创建 pending request，不期待业务响应。
- `call`: 请求响应，必须有 `ok` 或 `err`，支持超时。
- `stream`: 持续数据流，第一版只支持单向 item 流。

方向独立于语义：

- `c2s`: client to server
- `s2c`: server to client
- `bidi`: 双方都可以发起
- `s2s`: 后续可用于服务间契约

示例：

```xml
<services namespace="player">
  <service name="PlayerService" id="100">
    <send name="ReportInput"
          id="1"
          message="player.ReportInput"
          direction="c2s"/>

    <call name="GetProfile"
          id="2"
          request="player.GetProfileRequest"
          response="player.GetProfileReply"
          errors="player.PlayerErrors"
          direction="c2s"
          timeout_ms="3000"/>

    <stream name="TailLogs"
            id="3"
            request="player.TailLogsRequest"
            item="common.LogEntry"
            errors="common.CommonErrors"
            direction="c2s"
            timeout_ms="10000"/>
  </service>
</services>
```

`stream` 第一版必须限制资源：

- 最大并发 stream 数量
- 最大 item 大小
- 最大持续时间
- 最大缓冲数量
- `cancel` 幂等
- `close` 和 `error` 是终态

## Mapper 契约

数据库能力参考 MyBatis 的 mapper 思路，但第一版不做重 ORM。核心是显式 SQL、参数绑定、结果映射和事务边界。

```xml
<mappers namespace="player">
  <entity name="PlayerEntity" table="player">
    <field name="player_id" column="player_id" type="string" id="1"/>
    <field name="nickname" column="nickname" type="string" id="2"/>
    <field name="level" column="level" type="int32" id="3"/>
  </entity>

  <mapper name="PlayerMapper">
    <select name="SelectProfile"
            id="1"
            paramType="player.GetProfileRequest"
            resultType="player.PlayerProfile">
      SELECT player_id, nickname, level
      FROM player
      WHERE player_id = #{player_id}
    </select>
  </mapper>
</mappers>
```

RPC DTO、持久化 entity 和对外 view 可以共享基础类型，但不能默认等同。数据库结构变化不应直接污染客户端协议。

更详细的 mapper XML、参数绑定、结果映射、事务、缓存提示和 server-only descriptor profile 见 [Schema Mapper](schema-mapper.md)。

## Descriptor Package

`descriptor.bin` 是运行时主产物。它应该拆成两类内容：

```text
DescriptorPackage
  meta
  schema
```

`schema` 是 canonical descriptor，参与内容 hash 和 Merkle tree。`meta` 是构建信息，不参与 schema content hash。

建议的 metadata：

- `schema_name`
- `schema_version`
- `schema_root_hash`
- `compiled_at_unix_ms`
- `compiler_version`
- `source_revision`
- `source_dirty`
- `build_profile`
- `module_count`
- `compatibility_level`

`compiled_at_unix_ms` 必须保留，方便排查客户端拿到的是哪一份协议。但它不能参与 `schema_root_hash`，否则同一份协议每次重新编译都会变成不同 hash。

更详细的运行时包结构、canonical IR、Merkle 节点和兼容检查规则见 [Schema Descriptor](schema-descriptor.md)。

## Merkle Tree

Merkle tree 应该基于 canonical descriptor，而不是原始 XML 文件。XML 的格式化、注释、属性顺序和文件拆分不应影响协议内容 hash。

推荐树结构：

```text
root
  common
    types
      common.ShieldError
    errors
      common.CommonErrors
  player
    types
      player.PlayerProfile
    services
      player.PlayerService.GetProfile
    mappers
      player.PlayerMapper.SelectProfile
```

叶子节点是语义单元：

- 一个 enum
- 一个 struct
- 一个 error-set
- 一个 service method
- 一个 mapper method

握手时客户端发送 `schema_version`、`schema_root_hash` 和模块 hash 摘要。服务端返回：

- `accepted`
- `patch_available`
- `incompatible`
- `upgrade_required`

Merkle 只能说明哪里变了，不能单独判断是否可以热更新。兼容性规则必须独立存在。

## 兼容规则

安全热更新：

- 新增 optional 字段
- 新增 enum 值，且旧端可以忽略
- 新增 service method
- 新增 error code
- 新增文档和注释

条件热更新：

- 修改 timeout
- 修改 route 或 auth policy
- 修改默认值
- 废弃字段但不删除

不安全热更新：

- 修改已有 field id
- 修改已有 field type
- 删除字段
- 复用 field id
- 修改 service id 或 method id
- 修改 `call` response 类型
- 修改 `stream` item 基础结构
- 修改错误码含义
- 收紧 validation 规则导致旧请求非法

运行时需要支持多版本 descriptor 共存：

- 新请求使用 active descriptor。
- 已发出的 `call` 使用发起时的 descriptor decode。
- 已打开的 `stream` 使用打开时的 descriptor decode。
- 旧 descriptor 延迟回收。

## Wire Format

Payload 编码尽量遵循 Protobuf wire format 规则：

- `tag = (field_number << 3) | wire_type`
- varint
- zigzag
- fixed32 / fixed64
- length-delimited
- packed repeated
- map as repeated entry message
- unknown field skip

Shield 自己定义的是 envelope 和服务语义，不重新发明字段编码纪律。

Envelope 第一版建议保留可扩展头：

```text
Frame
  frame_length      uint32   # TCP only, WebSocket/UDP 可省略外层长度
  magic             uint16
  version           uint8
  header_len        uint8
  msg_kind          uint8
  flags             uint16
  service_id        uint16
  method_id         uint16
  correlation_id    uint64
  sequence          uint32
  payload_length    uint32
  payload           bytes
```

`correlation_id` 同时服务于 `call` 和 `stream`。错误详情不放在 header 中，统一由 `common.ShieldError` payload 承载。

`msg_kind` 第一版：

- `SEND`
- `CALL_REQ`
- `CALL_OK`
- `CALL_ERR`
- `STREAM_OPEN`
- `STREAM_ITEM`
- `STREAM_CLOSE`
- `STREAM_ERR`
- `STREAM_CANCEL`
- `HANDSHAKE`
- `HANDSHAKE_ACK`
- `HEARTBEAT`
- `HEARTBEAT_ACK`

## Client Runtime

客户端运行时最小依赖：

- `shield-client-runtime`
- `descriptor.bin`
- 连接配置

有了 `descriptor.bin`，客户端可以动态完成：

- 编码
- 解码
- 字段校验
- `send`
- `call`
- `stream`
- pending call 管理
- event/item 分发
- schema 兼容检查

但开发者不应该直接面对 `descriptor.bin`。默认开发包应该包含：

- `descriptor.bin`
- `descriptor.debug.json`
- generated typed wrappers
- generated docs

强类型 wrapper 只是开发体验增强，底层仍走 descriptor runtime。

TypeScript 示例：

```ts
export class PlayerServiceClient {
  constructor(private readonly client: ShieldClient) {}

  getProfile(req: GetProfileRequest): Promise<GetProfileReply> {
    return this.client.call("player.PlayerService", "GetProfile", req);
  }
}
```

C# 示例：

```csharp
public sealed class PlayerServiceClient {
    private readonly ShieldClient client;

    public Task<GetProfileReply> GetProfileAsync(GetProfileRequest req) {
        return client.CallAsync<GetProfileRequest, GetProfileReply>(
            "player.PlayerService",
            "GetProfile",
            req);
    }
}
```

如果热更新后的 descriptor 与生成代码版本不一致，runtime 应给出明确 warning。只要目标 method 和 type 仍兼容，wrapper 可以继续工作；不兼容时应返回清晰的 schema mismatch 错误。

## Client Plugins

客户端插件独立于服务端插件系统。第一批建议聚焦：

- TypeScript/Web package
- Unity C# package
- Native C++ client core

平台插件只处理各自生态的生命周期和线程模型：

- Unity: C# package、main-thread dispatch、`Task<T>` API。
- Web: npm package、`Promise<T>` API、browser 和 Node 支持。
- Native C++: `future` / callback API。

Unreal 和 Godot 可以在基础模型稳定后补充。

## Tooling Outputs

`shield protoc` 读取 `protocol/manifest.xml`，输出：

```text
build/protocol/
  descriptor.bin
  descriptor.debug.json
  merkle.json
  docs/
  generated/
    ts/
    csharp/
    cpp/
    lua/
```

校验步骤：

1. 读取 manifest。
2. 加载所有模块 XML。
3. XSD 校验。
4. 语义校验。
5. 构建 canonical IR。
6. 校验 ID 稳定性和兼容性。
7. 生成 descriptor package。
8. 生成可选语言绑定和文档。

显式 ID 规则：

- `service id` 必须显式声明。
- `method id` 必须显式声明。
- `field id` 必须显式声明。
- 发布后不能复用。

## Handshake

客户端握手请求至少包含：

- `client_name`
- `client_version`
- `client_runtime_version`
- `schema_version`
- `schema_root_hash`
- `module_hashes`
- `compiled_at_unix_ms`

服务端返回：

- `accepted`
- `server_schema_version`
- `server_schema_root_hash`
- `server_compiled_at_unix_ms`
- `compatibility_result`
- `changed_paths`
- `patch_available`

后续支持 descriptor patch 时，patch chunk 必须包含 hash 校验。生产环境应增加签名和回滚保护。

## 实施阶段

Phase 1:

- 目录化 XML 规范
- XSD + 语义校验
- canonical IR
- `descriptor.bin`
- `descriptor.debug.json`
- `send` / `call`
- `common.ShieldError`
- `SchemaStarter`
- `shield_protoc`
- 基础 validation rule，不包含通用 `pattern`

Phase 2:

- Protobuf-compatible payload 编码
- `stream`
- Merkle root 和 module hash
- TypeScript 和 Unity C# wrapper
- Markdown/HTML 文档生成
- `pattern` validation，前提是固定安全正则语义

Phase 3:

- descriptor patch
- 多版本 descriptor 共存
- client hot update
- mapper contract
- Native C++ client core
- mapper runtime bundled extension

Phase 4:

- patch 签名
- 灰度和回滚保护
- Unreal/Godot 插件
- 更完整的 mapper runtime
