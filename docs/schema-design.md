# Schema Design

> Status: deferred extension design.
>
> The current refactor focuses on the Lua-first single-node runtime. Schema
> protocol, descriptor packages, client SDK plugins, and generated tooling are
> not part of the core runtime boundary unless reintroduced by a future roadmap
> decision. This page consolidates the former schema-protocol / schema-types /
> schema-descriptor / schema-mapper / schema-implementation pages.

Shield 的 schema protocol 是一套定义驱动的协议契约系统。目标是让游戏服务器和客户端共享同一份 XML 契约，运行时加载编译后的 descriptor，不要求业务开发者在协议变更后重新编译 C++ 协议桩。

本文描述目标设计。早期重构中曾存在 `include/shield/protocol/schema_protocol.hpp`、`src/protocol/schema_protocol.cpp`、`tests/unit/protocol/test_schema_protocol.cpp` 等实验实现，但在当前重构中已移除；本文定义的目录化 XML、descriptor package、Merkle 增量和客户端插件模型仍属于未实现方向。

当前 Lua runtime 提供的原始 DB/Redis 访问能力由插件通过 `shield.database.*` 命名空间暴露（见 [Lua API](lua-api.md)）；本文描述的 XML mapper 解析、descriptor 生成、强类型代码生成、动态 SQL 节点和 entity CRUD helper 均为 deferred 设计，未在当前代码库实现。整体设计是否进入核心运行时边界，需要由架构演进路线（见 [Architecture](architecture.md)）显式决策。

## 设计目标

1. XML 作为协议契约源文件，支持 XSD 和语义强校验。
2. 类型定义可读、可验证、可生成；XML 结构由 XSD 强校验；类型引用、字段 ID、兼容性由语义校验保证。
3. 运行时加载 `descriptor.bin`，不直接解析 XML 进入热路径。
4. 客户端运行时只依赖 `descriptor.bin` 即可动态编码、解码和分发。
5. 开发期同时生成可读 descriptor、强类型 wrapper 和文档，避免客户端开发者面对二进制文件。
6. Payload 编码尽量遵循 Protobuf wire format 规则，但服务语义由 Shield 自己定义。
7. `send`、`call`、`stream` 与方向解耦，方向只描述谁可以发起。
8. 通过 Merkle tree 支持精确变更定位和后续 descriptor 热更新。
9. 数据库 mapper 共享类型系统，但不和 RPC DTO 直接耦合；RPC DTO、持久化 entity 和对外 view 可以共享基础类型，但边界清晰。

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

一个模块可以拆多个 mapper 文件，manifest 支持显式 include：

```xml
<module name="player" path="player">
  <types file="types.xml"/>
  <services file="services.xml"/>
  <mappers file="mappers.xml"/>
  <mappers file="inventory_mappers.xml"/>
</module>
```

依赖方向必须保持简单：

- `types.xml` 可以引用 `common` 类型。
- `services.xml` 可以引用类型和错误集。
- `mappers.xml` 可以引用类型和实体映射。
- `services.xml` 不直接依赖 `mappers.xml`。
- `mappers.xml` 不直接依赖 `services.xml`。
- 不允许循环依赖。

## 类型系统 (types.xml)

`types.xml` 定义可复用数据结构，是 RPC、客户端 SDK、mapper、错误模型和文档生成的公共类型来源。它只描述数据，不描述行为。类型全名为 `namespace.name`，例如 `player.PlayerProfile`。

### 基础类型

第一版内置 scalar：

- `bool`
- `int32`、`int64`（普通 varint 编码）
- `uint32`、`uint64`
- `sint32`、`sint64`（zigzag 编码）
- `float`、`double`
- `string`（必须是 UTF-8）
- `bytes`（不做字符集假设）

### Enum

```xml
<enum name="PlayerState">
  <item name="Offline" value="0"/>
  <item name="Online" value="1"/>
  <item name="InRoom" value="2"/>
</enum>
```

规则：

- enum name 在 namespace 内唯一。
- enum value 在 enum 内唯一。
- enum item name 在 enum 内唯一。
- 发布后的 value 不能复用。
- 删除 enum item 时必须标记 `deprecated="true"` 或进入 reserved。
- 客户端必须能处理未知 enum value。

保留值：

```xml
<enum name="PlayerState">
  <reserved value="3"/>
  <reserved range="100-199"/>
  <item name="Offline" value="0"/>
  <item name="Online" value="1"/>
</enum>
```

### Struct 与字段

```xml
<struct name="PlayerProfile">
  <field name="player_id" id="1" type="string"/>
  <field name="nickname" id="2" type="string"/>
  <field name="level" id="3" type="int32" default="1"/>
  <field name="state" id="4" type="player.PlayerState"/>
</struct>
```

规则：

- struct name 在 namespace 内唯一。
- field id 在 struct 内唯一。
- field name 在 struct 内唯一。
- field id 发布后不能复用。
- 字段删除后必须保留 field id。
- 字段默认 optional。
- `required` 只能作为 validation 规则，不能成为 wire format required，避免协议演进困难。

保留字段：

```xml
<struct name="PlayerProfile">
  <reserved id="5"/>
  <reserved range="100-199"/>
  <field name="player_id" id="1" type="string"/>
</struct>
```

字段属性：

```xml
<field name="nickname"
       id="2"
       type="string"
       optional="true"
       default="guest"
       deprecated="false"/>
```

建议字段属性：`name`、`id`（wire field number）、`type`、`optional`、`default`、`deprecated`、`since`、`doc`。字段命名建议统一使用 `snake_case`，生成器可以为 C#、TS 转换为目标语言习惯命名。

### 集合类型

列表：

```xml
<field name="items" id="1" type="list<player.ItemInfo>"/>
```

Map：

```xml
<field name="attrs" id="2" type="map<string,string>"/>
```

规则：

- list 元素可以是 scalar、enum、struct。
- map key 第一版只允许 `string`、整数和 enum，不允许 struct 作为 key。
- map value 可以是 scalar、enum、struct。
- map 在 wire format 中按 repeated entry message 编码。

### Alias

Alias 用于表达领域语义，不改变 wire encoding：

```xml
<alias name="PlayerId" type="string"/>
<alias name="RoomId" type="uint64"/>
```

生成器可以用 alias 生成更明确的类型别名。运行时 descriptor 保留 alias 信息用于文档和校验。

### Entity / View / DTO

`entity` 是持久化模型，通常在 mapper 文件中定义，也可以放在 types 文件中集中管理：

```xml
<entity name="PlayerEntity" table="player">
  <field name="player_id" column="player_id" type="string" id="1" primaryKey="true"/>
  <field name="nickname" column="nickname" type="string" id="2"/>
</entity>
```

- entity 可以参与 mapper result mapping。
- entity 默认不暴露给客户端 descriptor。
- entity 不应直接作为公网 RPC response，除非显式 `expose="client"`。

对外协议建议使用 struct 作为 DTO 或 view：

```xml
<struct name="PlayerProfile" expose="client">
  <field name="player_id" id="1" type="string"/>
  <field name="nickname" id="2" type="string"/>
  <field name="level" id="3" type="int32"/>
</struct>
```

类型暴露规则（Client Profile）：

- `expose="client"`: 进入客户端 descriptor。
- `expose="server"`: 只进入服务端 descriptor。
- `expose="both"`: 同时进入。
- 未声明时 struct 默认 server，RPC request/response 引用到的类型自动提升到 client-visible。
- 公共错误类型默认 `expose="both"`。
- 编译器必须防止 client-visible 类型引用 server-only 类型。

## 错误模型

错误是一等契约，不能只是字符串。公共错误结构放在 `common/errors.xml`，但语义上仍属于类型系统：

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

业务模块可以定义自己的 `error-set`：

```xml
<error-set name="PlayerErrors">
  <error code="20001" name="PLAYER_NOT_FOUND" category="Business"/>
  <error code="20002" name="PLAYER_BANNED" category="Business"/>
</error-set>
```

`call` 的 `err` 和 `stream` 的 `on_err` 都统一使用 `common.ShieldError`。运行时校验失败统一返回 `common.ShieldError`，category 为 `Validation`。

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

`stream` 第一版必须限制资源：最大并发 stream 数量、最大 item 大小、最大持续时间、最大缓冲数量、`cancel` 幂等、`close` 和 `error` 是终态。

显式 ID 规则：`service id`、`method id`、`field id` 必须显式声明，发布后不能复用。

## Mapper 契约

数据库能力参考 MyBatis 的 mapper 思路，但第一版不做重 ORM。核心是显式 SQL、参数绑定、结果映射和事务边界。mapper 只进入服务端 descriptor，默认不下发客户端。第一版支持关系型数据库，后续再扩展 Redis、MongoDB 等后端。

### 边界

mapper 负责：SQL statement 定义、参数绑定、结果映射、事务策略、分页约束、批量操作、可选缓存提示、生成服务端接口。

mapper 不负责：客户端协议、服务路由、业务流程编排、自动对象图加载、ActiveRecord 生命周期、跨服务分布式事务。

### Mapper 定义与 statement

```xml
<mappers namespace="player" dialect="mysql">
  <entity name="PlayerEntity" table="player">
    <field name="player_id" column="player_id" type="string" id="1"/>
    <field name="nickname" column="nickname" type="string" id="2"/>
    <field name="level" column="level" type="int32" id="3"/>
  </entity>

  <mapper name="PlayerMapper" id="100">
    <select name="SelectProfile"
            id="1"
            paramType="player.GetProfileRequest"
            resultType="player.PlayerProfile"
            timeout_ms="1000">
      SELECT player_id, nickname, level
      FROM player
      WHERE player_id = #{player_id}
    </select>

    <update name="UpdateNickname"
            id="2"
            paramType="player.UpdateNicknameRequest"
            resultType="common.AffectedRows">
      UPDATE player
      SET nickname = #{nickname}
      WHERE player_id = #{player_id}
    </update>
  </mapper>
</mappers>
```

规则：

- `mapper id` 在模块内唯一。
- `statement id` 在 mapper 内唯一。
- 发布后的 ID 不能复用。
- `paramType` 必须引用 schema type。
- `resultType` 或 `resultMap` 必须显式声明。
- SQL 中只能使用声明式参数绑定，不允许字符串拼接参数。

第一版支持 `select` / `insert` / `update` / `delete`；后续可扩展 `batchInsert` / `batchUpdate` / `upsert` / `callProcedure`。

### 参数绑定

参数使用 `#{name}` 绑定（安全 prepared statement 参数），支持点路径嵌套：

```sql
WHERE player_id = #{player_id}
  AND guild_id = #{filter.guild_id}
```

路径必须能在 `paramType` 中静态解析，编译期校验失败则拒绝生成 descriptor。

`${name}` 原样替换不进入 Phase 1。后续如需支持，只能用于经过白名单校验的标识符场景（如排序字段），需配合 `<bind mode="identifier" allow="..."/>`。

### Result Type 与 Result Map

简单结果直接用 `resultType`，字段名默认按 column name 匹配 struct field name。Phase 1 允许 `snake_case` 到 `camelCase` 映射，但必须通过 mapper 配置显式开启。

复杂映射使用 `resultMap`：

```xml
<resultMap name="PlayerProfileMap" type="player.PlayerProfile">
  <result column="player_id" property="player_id"/>
  <result column="nickname" property="nickname"/>
  <result column="level" property="level"/>
</resultMap>
```

第一版 resultMap 不做自动关联加载；需要 join 时用显式 SQL 返回扁平 DTO。

### 返回形态、分页与动态 SQL

`select` 默认返回单条；列表查询显式声明 `list<...>`。列表查询必须有 `maxRows`，分页查询必须有上限；没有 `WHERE` 的 `update/delete` 默认拒绝，除非显式 `allowFullTable="true"`。

分页由契约显式表达，offset paging 第一版支持，cursor paging 推迟到 Phase 2。

Phase 1 动态 SQL 只支持最小集合：`if`、`where`、`set`、`foreach`（用于 `IN` 列表）。不支持任意表达式语言、include fragment、provider method 或用户自定义 SQL 节点。表达式语言必须受限，不能执行脚本。

### 事务

一个 mapper statement 只能包含一条 SQL。多步事务由 service 层显式编排：

```lua
local db = shield.database.mysql("database.default")
db:transaction(function(tx)
  PlayerMapper:DebitGold(tx, { player_id = from_id, amount = amount })
  PlayerMapper:CreditGold(tx, { player_id = to_id, amount = amount })
end)
```

statement 级事务策略取值：`none`、`required`（没有事务则开启，有则复用）、`requires_new`。Phase 1 不做跨 mapper 自动事务编排。禁止在单个 mapper statement 中编写多条 SQL。

### 缓存提示与方言

缓存先作为 hint，不作为强制语义：

```xml
<select name="SelectProfile" id="1" ... cache="local" cacheKey="player:{player_id}" ttl_ms="30000">
```

缓存策略取值：`none`、`local`、`redis`。Phase 1 只生成缓存元数据，写操作后失效需显式 `invalidateCache`。

SQL 方言在 `mappers` 上显式声明。Phase 1 支持 `mysql` 和 `sqlite`；`postgres` 保留为可声明方言，runtime 支持推迟到 Phase 2。同一 statement 可提供多个方言版本：

```xml
<select name="Now" id="20" resultType="common.Timestamp">
  <sql dialect="mysql">SELECT UNIX_TIMESTAMP() AS value</sql>
  <sql dialect="sqlite">SELECT strftime('%s','now') AS value</sql>
</select>
```

### Generated Server API 与 Runtime Flow

生成器应输出服务端 mapper 接口：

```cpp
class PlayerMapper {
public:
    task<PlayerProfile> SelectProfile(const GetProfileRequest& req);
    task<AffectedRows> UpdateNickname(const UpdateNicknameRequest& req);
};
```

```lua
local profile = PlayerMapper:SelectProfile({ player_id = player_id })
```

运行时流程：

```text
service handler
  -> generated mapper facade
  -> mapper runtime
  -> resolve statement descriptor
  -> bind parameters
  -> prepare statement
  -> execute
  -> map result rows
  -> return typed result or ShieldError
```

mapper runtime 必须记录：mapper name、statement name、SQL hash、duration、affected rows、row count、error code，进入日志和 metrics。

mapper 默认只进入 server profile：

```text
descriptor.server.bin
  types, services, errors, mappers
descriptor.client.bin
  types, services, errors exposed to client（不含 SQL、表名、列名、缓存 key、事务策略）
```

### Mapper 安全规则

编译期校验：

- SQL 参数都能在 `paramType` 中解析。
- `resultType` 字段能被查询列覆盖，除非字段 optional。
- `update/delete` 默认必须有 `WHERE`。
- 列表查询必须有 `maxRows`。
- 动态 SQL 表达式不能执行任意脚本。
- 不允许未声明的原样字符串替换。

运行期限制：statement timeout、最大返回行数、最大参数数量、最大 SQL 长度、连接池资源。

当前文档属于 schema/tooling 草案，不进入当前最小启动路径。若后续恢复 mapper runtime，应基于数据插件 binding 和 `shield.database.v1` / `shield.document.v1` 等接口实现，不恢复 `shield.db.*` 全局 API，也不新增 core 内置 lightweight DB runtime。`entity` 不生成 schema migration 草案；migration 属于独立工具链或应用层责任。

## Descriptor Package

`descriptor.bin` 是运行时主产物。它是 XML 契约经过校验后的 canonical runtime package，不是生成后的业务代码，也不要求重新编译客户端或服务端。

它必须支持：快速加载、跨语言解析、内容 hash 稳定、Merkle 增量比较、多版本共存、开发期可读调试产物。它不负责：业务流程、数据库连接实现、客户端 UI 绑定、语言特定的强类型 wrapper。

### Package Layout

descriptor package 分为多段：

```text
DescriptorPackage
  PackageHeader
  MetaBlock
  SchemaBlock
  MerkleBlock
  StringTable
```

`SchemaBlock` 和 `MerkleBlock` 参与内容 hash。`MetaBlock` 不参与 schema content hash，因为其中包含编译时间、编译器版本和源码版本等构建信息。

### PackageHeader

```text
PackageHeader
  magic             bytes[4]   # "SHD1"
  package_version   uint16
  header_size       uint16
  flags             uint32
  meta_offset       uint32
  meta_size         uint32
  schema_offset     uint32
  schema_size       uint32
  merkle_offset     uint32
  merkle_size       uint32
  string_offset     uint32
  string_size       uint32
  package_crc32     uint32
```

第一版使用 little-endian。跨语言客户端必须显式按 little-endian 读取，不依赖平台字节序。`package_crc32` 只用于快速发现文件损坏；生产环境完整性第一版使用 `schema_root_hash` 和发布流程校验，外部签名机制推迟到 Phase 2。

### MetaBlock

```text
MetaBlock
  schema_name             string_id
  schema_version          string_id
  schema_root_hash        bytes[32]
  compiled_at_unix_ms     uint64
  compiler_version        string_id
  source_revision         string_id
  source_dirty            bool
  build_profile           string_id
  compatibility_level     uint16
  module_count            uint32
```

`compiled_at_unix_ms` 必须保留，方便排查客户端和服务端实际加载了哪份协议。它不参与 `schema_root_hash`。

### StringTable

所有名称、命名空间、文档字符串、默认值字符串都进入 string table，Schema 对象只保存 `string_id`。目的：减小 descriptor 体积、保证 canonical 排序更简单、方便跨语言解析、降低重复字符串带来的 hash 噪音。第一版使用 UTF-8 字符串，长度使用 `uint32`。

### Canonical IR

XML 被编译为 canonical IR 后再写入 `SchemaBlock`，必须消除 XML 文件层面的非语义差异：注释、格式化、属性顺序、include 顺序、文件拆分方式。

排序规则必须固定：

- module 按 `name` 排序
- type 按 fully qualified name 排序
- service 按 `service_id` 排序
- method 按 `method_id` 排序
- field 按 `field_id` 排序
- error 按 `code` 排序
- mapper method 按 `id` 排序

### SchemaBlock

```text
SchemaBlock
  modules[]
  types[]
  services[]
  error_sets[]
  mappers[]
```

#### ModuleDescriptor

```text
ModuleDescriptor
  id                  uint16
  name                string_id
  version             string_id
  hash                bytes[32]
  dependency_ids[]    uint16
```

模块 ID 可以由 manifest 显式声明，也可以由工具分配后写入 lock 文件。`module_id` 由 `protocol.lock` 分配并长期稳定，发布后不能变。

#### TypeDescriptor 与引用

```text
TypeDescriptor
  id                  uint32
  module_id           uint16
  name                string_id
  full_name           string_id
  kind                uint8      # scalar, enum, struct, entity, alias
  flags               uint32
  hash                bytes[32]
```

`type_id` 是 descriptor 内部引用 ID，不进入 wire format。Wire format 只依赖字段编号和字段 wire type。

#### EnumDescriptor / StructDescriptor / TypeRef

```text
EnumDescriptor
  type_id             uint32
  values[]

EnumValue
  name                string_id
  value               int32
  deprecated          bool
```

新增 enum value 通常是兼容变更，但客户端逻辑必须能处理未知 enum 值。

```text
StructDescriptor
  type_id             uint32
  fields[]

FieldDescriptor
  id                  uint32
  name                string_id
  type_ref            TypeRef
  cardinality         uint8      # optional, repeated, map
  default_value       string_id
  validation_rules    ValidationRuleRef[]
  deprecated          bool
  reserved            bool
```

`reserved=true` 用于保留已删除字段 ID，禁止未来复用。

```text
TypeRef
  kind                uint8      # scalar, named, list, map
  scalar              uint8
  named_type_id       uint32
  key_type            TypeRef
  value_type          TypeRef
```

第一版 map key 只允许 `string`、整数和 enum，不允许 struct 作为 key。字段按 `field id` 排序后参与 canonical hash。

#### ServiceDescriptor / MethodDescriptor

```text
ServiceDescriptor
  id                  uint16
  module_id           uint16
  name                string_id
  full_name           string_id
  direction_mask      uint8
  methods[]
  hash                bytes[32]

MethodDescriptor
  id                  uint16
  name                string_id
  kind                uint8      # send, call, stream
  direction           uint8      # c2s, s2c, bidi, s2s
  request_type_id     uint32
  response_type_id    uint32
  item_type_id        uint32
  error_set_id        uint32
  timeout_ms          uint32
  auth_policy_id      uint32
  route_id            uint32
  transport_mask      uint32
  hash                bytes[32]
```

`service_id` 进入 frame header，必须显式声明并长期稳定。方法规则：

- `send` 必须有 `request_type_id`，不能有 `response_type_id` 和 `item_type_id`。
- `call` 必须有 `request_type_id` 和 `response_type_id`。
- `stream` 必须有 `request_type_id` 和 `item_type_id`。
- `method_id` 在 service 内唯一，进入 frame header；发布后不能复用。

#### ErrorSetDescriptor

```text
ErrorSetDescriptor
  id                  uint32
  module_id           uint16
  name                string_id
  full_name           string_id
  errors[]
  hash                bytes[32]

ErrorDescriptor
  code                int32
  name                string_id
  category            uint16
  retryable           bool
  message             string_id
  deprecated          bool
```

错误码含义不能在兼容版本中改变。废弃错误码只能标记 deprecated，不能复用。

#### MapperDescriptor

Mapper descriptor 第一版用于工具生成和服务端运行时绑定，不进入客户端默认 descriptor：

```text
MapperDescriptor
  id                  uint32
  module_id           uint16
  name                string_id
  full_name           string_id
  methods[]
  hash                bytes[32]

MapperMethodDescriptor
  id                  uint16
  name                string_id
  op                  uint8      # select, insert, update, delete
  param_type_id       uint32
  result_type_id      uint32
  transaction_policy  uint8
  sql_id              uint32
  hash                bytes[32]
```

客户端 descriptor 应默认剔除 mapper SQL，工具生成 `descriptor.server.bin` 和 `descriptor.client.bin` 两个 profile。mapper descriptor 不进入 descriptor Phase 1；等 descriptor、type、service 基础稳定后作为 server-only profile 扩展。

### MerkleBlock

Merkle tree 基于 canonical descriptor，而不是原始 XML 文件——XML 的格式化、注释、属性顺序和文件拆分不应影响协议内容 hash。

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

叶子节点是语义单元：一个 enum、一个 struct、一个 error-set、一个 service method、一个 mapper method。

```text
MerkleBlock
  root_hash           bytes[32]
  nodes[]

MerkleNode
  id                  uint32
  parent_id           uint32
  path                string_id
  kind                uint8      # root, module, type, service, method, error_set, mapper
  hash                bytes[32]
  child_ids[]
```

Hash 输入必须包含：node kind、stable identity、canonical payload bytes、child hashes。Hash 输入不能包含：`compiled_at`、`source_revision`、source file path、XML formatting、comments。

Merkle 只能说明哪里变了，不能单独判断是否可以热更新。兼容性规则必须独立存在。

### Descriptor Debug JSON 与 Profiles

`descriptor.debug.json` 是开发期可读产物，应与 `descriptor.bin` 来自同一 canonical IR，用于客户端开发查看结构、IDE 插件、协议 diff、QA 联调工具和线上问题排查。

```json
{
  "schemaName": "game",
  "schemaVersion": "1.8.3",
  "schemaRootHash": "6b2c...",
  "compiledAtUnixMs": 1781012345678,
  "compilerVersion": "shield-protoc 0.3.0",
  "modules": [
    {
      "name": "player",
      "hash": "91ab...",
      "services": [
        {
          "name": "PlayerService",
          "id": 100,
          "methods": [
            {
              "name": "GetProfile",
              "id": 2,
              "kind": "call",
              "request": "player.GetProfileRequest",
              "response": "player.GetProfileReply"
            }
          ]
        }
      ]
    }
  ]
}
```

同一份 XML 可以编译成不同 profile：

- `server`: 包含 services、types、errors、mappers。
- `client`: 包含可暴露给客户端的 services、types、errors，不包含 SQL。
- `docs`: 包含文档注释和展示元信息。
- `debug`: 包含完整可读结构和 source mapping。

Profile 不应改变协议语义 hash。对于 client/server 差异，应分别计算 profile package hash，同时保留共同的 schema root hash。

### Descriptor 设计已定规则

- Hash 算法使用 SHA-256；CRC32 只用于快速损坏检测，不参与语义兼容判断。
- `module_id` 由 `protocol.lock` 分配并长期稳定；manifest 可以显式声明，但不强制。
- `service_id`、`method_id`、`field_id` 必须显式声明。
- `descriptor.bin` 内部编码第一版使用项目自定义 canonical binary encoding，不直接复用 protobuf message encoding。
- `ValidationRule` 第一版只支持 `required`、`min`、`max`、`minLength`、`maxLength`、`minItems`、`maxItems`。

## Validation

Validation 分为四层：XML 结构校验、类型语义校验、兼容性校验、运行时数据校验。

### XML 结构校验（XSD）

XSD 应覆盖结构性规则：

```xml
<xs:element name="types">
  <xs:complexType>
    <xs:choice maxOccurs="unbounded">
      <xs:element name="enum" type="EnumType"/>
      <xs:element name="struct" type="StructType"/>
      <xs:element name="alias" type="AliasType"/>
      <xs:element name="entity" type="EntityType"/>
      <xs:element name="error-set" type="ErrorSetType"/>
    </xs:choice>
    <xs:attribute name="namespace" type="xs:string" use="required"/>
  </xs:complexType>
</xs:element>
```

XSD 只校验：根节点是否合法、必填属性是否存在、属性类型是否正确、子节点顺序和层级是否合法、元素是否出现在允许位置。它不能完成跨文件引用、ID 唯一性和兼容性判断，这些由 `shield protoc` 的语义校验完成。

### 类型语义校验

编译器必须检查：namespace 合法、类型名合法、同 namespace 内类型名唯一、类型引用存在、field id 唯一且不在 reserved 范围、field type 合法、default value 能转换为字段类型、map key 类型合法、enum value 唯一、error code 唯一、expose profile 不泄漏 server-only 类型。

### 兼容性校验

兼容检查基于两个 canonical descriptors，而不是文本 diff。

```text
CompatibilityReport
  result              accepted | patchable | incompatible | upgrade_required
  changes[]

CompatibilityChange
  path                string
  kind                added | removed | modified | deprecated | reserved
  level               safe | conditional | breaking
  reason              string
```

安全变更：新增 optional field、新增 struct、新增 service method、新增 error code、新增 enum value、新增 server-only 类型。

条件变更：修改 timeout、修改 auth policy、修改 route、修改默认值、标记 deprecated、修改 expose profile、新增 validation rule。

破坏性变更：修改 field id、修改 field type、删除未 reserved 的字段、复用 field id 或 reserved id、修改 enum value 含义、修改 service id / method id、修改 call response type、修改 stream item type、修改 error code 含义、删除客户端仍可见类型、将 optional 字段改成 required validation、收紧 validation 规则导致旧请求非法。

### 运行时数据校验与 Validation Rule

运行时按 descriptor 校验：required validation、string 长度、bytes 长度、数值范围、list 长度、map 长度、enum 是否在已知范围、已启用的内置 validation rule。失败统一返回 `common.ShieldError`，category 为 `Validation`。

字段级规则示例：

```xml
<field name="nickname" id="2" type="string">
  <validate required="true" minLength="2" maxLength="16"/>
</field>
<field name="level" id="3" type="int32">
  <validate min="1" max="999"/>
</field>
<field name="items" id="4" type="list<player.ItemInfo>">
  <validate maxItems="200"/>
</field>
```

Phase 1 支持 `required` / `min` / `max` / `minLength` / `maxLength` / `minItems` / `maxItems`。不支持通用 `pattern` 正则、任意脚本表达式、跨字段复杂约束、数据库唯一性校验、远程服务校验。复杂业务规则应放在 service 实现中。通用 `pattern` 固定放到 Phase 2，启用时必须使用 RE2 语义或受限正则子集，避免不同语言 regex 行为不一致和 ReDoS 风险。

## 多版本 Runtime

运行时必须允许 descriptor 多版本短期共存：

```text
DescriptorManager
  active_descriptor
  retained_descriptors[]
  call_descriptor_map[correlation_id]
  stream_descriptor_map[correlation_id]
```

规则：

- 新请求使用 active descriptor。
- 已发出的 `call`（`CALL_OK` / `CALL_ERR`）使用请求创建时记录的 descriptor。
- 已打开的 `stream`（`STREAM_ITEM` / `STREAM_CLOSE` / `STREAM_ERR`）使用打开时记录的 descriptor。
- descriptor 没有 pending call 和 stream 引用后才可回收（旧 descriptor 延迟回收）。

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

Shield 自己定义的是 envelope 和服务语义，不重新发明字段编码纪律。`descriptor.bin` 内部编码第一版使用项目自定义 canonical binary encoding，不直接复用 protobuf message encoding。

Envelope Phase 1 保留可扩展头：

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
- `CALL_REQ`、`CALL_OK`、`CALL_ERR`
- `STREAM_OPEN`、`STREAM_ITEM`、`STREAM_CLOSE`、`STREAM_ERR`、`STREAM_CANCEL`
- `HANDSHAKE`、`HANDSHAKE_ACK`
- `HEARTBEAT`、`HEARTBEAT_ACK`

## Client Runtime 与 SDK

客户端运行时最小依赖：`shield-client-runtime`、`descriptor.bin`、连接配置。有了 `descriptor.bin`，客户端可以动态完成：编码、解码、字段校验、`send` / `call` / `stream`、pending call 管理、event/item 分发、schema 兼容检查。

但开发者不应直接面对 `descriptor.bin`。默认开发包应包含：`descriptor.bin`、`descriptor.debug.json`、generated typed wrappers、generated docs。强类型 wrapper 只是开发体验增强，底层仍走 descriptor runtime。

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

热更新后的 descriptor 与生成代码版本不一致时，runtime 应给出明确 warning。只要目标 method 和 type 仍兼容，wrapper 可继续工作；不兼容时返回清晰的 schema mismatch 错误。

客户端插件独立于服务端插件系统。第一批聚焦：TypeScript/Web package（npm package、`Promise<T>` API、browser 和 Node 支持）、Unity C# package（main-thread dispatch、`Task<T>` API）、Native C++ client core（`future` / callback API）。Unreal 和 Godot 可在基础模型稳定后补充。平台插件只处理各自生态的生命周期和线程模型；客户端 SDK 不依赖服务端运行时或插件系统。

## Handshake

客户端握手请求至少包含：`client_name`、`client_version`、`client_runtime_version`、`schema_version`、`schema_root_hash`、`module_hashes`、`compiled_at_unix_ms`。

服务端返回：`accepted`、`server_schema_version`、`server_schema_root_hash`、`server_compiled_at_unix_ms`、`compatibility_result`、`changed_paths`、`patch_available`（取值 `accepted` / `patch_available` / `incompatible` / `upgrade_required`）。

后续支持 descriptor patch 时，patch chunk 必须包含 hash 校验。生产环境应增加签名和回滚保护。

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

## 实现布局 (Implementation Layout)

本节定义 schema protocol 从“概念可行”到“可以开始实现”的工程约束：runtime 源码放在哪里、compiler 工具放在哪里、namespace 如何拆、哪些内容属于未来 schema runtime、哪些必须留在 optional extension、client SDK 如何隔离、generated 文件落在哪里。

### 库边界

```text
future schema runtime
  descriptor runtime
  wire codec
  rpc pending/stream runtime
  descriptor loader

future schema extensions
  optional mapper runtime integration if it depends on database stack
  optional tooling integrations

shield_protoc
  XML parser
  XSD validation
  semantic validation
  compatibility checker
  descriptor generator
  code/doc generators

client SDKs
  cpp client core
  web/typescript package
  unity/csharp package
```

规则：

- 运行时解码、descriptor registry、`send/call/stream` 分发属于未来 schema runtime，不属于当前 refactor core。
- XML 编译器不进入当前 runtime 热路径。
- mapper descriptor 和 mapper runtime 均按后续扩展处理，不能进入当前最小启动路径。
- 客户端 SDK 不依赖服务端运行时或插件系统。
- `shield_protoc` 默认随开发构建启用，并通过 `SHIELD_BUILD_TOOLS=OFF` 关闭。
- Merkle root、module hash 和 node hash 第一版使用 SHA-256；CRC32 只用于快速损坏检测。

### 推荐源码目录

```text
include/shield/protocol/
  schema_protocol.hpp              # 兼容 facade，逐步变薄
  schema/
    descriptor.hpp
    descriptor_package.hpp
    descriptor_registry.hpp
    type_descriptor.hpp
    service_descriptor.hpp
    error_descriptor.hpp
    merkle_tree.hpp
    compatibility.hpp
    validation.hpp
  wire/
    frame.hpp
    frame_codec.hpp
    protobuf_wire.hpp
    payload_codec.hpp
  rpc/
    pending_call.hpp
    pending_call_registry.hpp
    stream.hpp
    stream_registry.hpp

src/protocol/
  schema_protocol.cpp              # 兼容 facade 实现
  schema/
    descriptor_package.cpp
    descriptor_registry.cpp
    merkle_tree.cpp
    compatibility.cpp
    validation.cpp
  wire/
    frame_codec.cpp
    protobuf_wire.cpp
    payload_codec.cpp
  rpc/
    pending_call_registry.cpp
    stream_registry.cpp
```

Mapper 运行时：

```text
include/shield/data/mapper/
  mapper_descriptor.hpp
  mapper_registry.hpp
  mapper_runtime.hpp
  sql_binder.hpp
  result_mapper.hpp
  transaction_policy.hpp

src/data/mapper/
  mapper_registry.cpp
  mapper_runtime.cpp
  sql_binder.cpp
  result_mapper.cpp
```

编译器工具链：

```text
tools/schema_compiler/
  CMakeLists.txt
  main.cpp
  compiler/
    manifest_loader.hpp
    xml_loader.hpp
    xsd_validator.hpp
    semantic_validator.hpp
    canonical_ir.hpp
    descriptor_writer.hpp
    debug_json_writer.hpp
    merkle_writer.hpp
    compatibility_checker.hpp
  generators/
    cpp_generator.hpp
    lua_generator.hpp
    typescript_generator.hpp
    csharp_generator.hpp
    markdown_generator.hpp

schemas/xsd/
  manifest.xsd
  types.xsd
  services.xsd
  mappers.xsd
```

客户端 SDK：

```text
client/
  cpp/
    include/shield/client/
      client.hpp
      connection.hpp
      descriptor_loader.hpp
      call.hpp
      stream.hpp
    src/
  web/
    package.json
    src/
      client.ts
      descriptor.ts
      codec.ts
      call.ts
      stream.ts
  unity/
    package.json
    Runtime/
      ShieldClient.cs
      DescriptorLoader.cs
      Codec.cs
      Call.cs
      Stream.cs
```

生成物：

```text
build/protocol/
  descriptor.server.bin
  descriptor.client.bin
  descriptor.debug.json
  merkle.json
  docs/
  generated/
    cpp/
    lua/
    ts/
    csharp/
```

### C++ Namespace

现有模块已经使用 `shield::protocol`、`shield::gateway`、`shield::core`、`shield::data` 和 `shield::database`。Schema protocol 新代码应避免继续平铺到 `shield::protocol`：

```cpp
namespace shield::protocol::schema {
// descriptor, registry, validation, compatibility, merkle
}

namespace shield::protocol::wire {
// frame codec, protobuf-compatible payload codec
}

namespace shield::protocol::rpc {
// pending call, stream state, correlation registry
}

namespace shield::data::mapper {
// mapper runtime, SQL binder, result mapper
}

namespace shield::tools::schema {
// shield_protoc compiler internals
}

namespace shield::client {
// native C++ client SDK
}
```

Facade API 可以保留在 `shield::protocol`：

```cpp
namespace shield::protocol {
using SchemaRegistry = schema::DescriptorRegistry;
}
```

这样旧代码可以过渡，新代码有明确层次。

### 命名规则

文件：C++ header/source 使用 `lower_snake_case`；XML 文件使用 `lower_snake_case.xml`；生成文件使用目标语言习惯，但保留 schema namespace。

C++ 类型：类型名使用 `PascalCase`；函数名遵循仓库现有风格，优先 `snake_case`；enum class 使用 `PascalCase` 类型名，枚举值使用 `UPPER_SNAKE_CASE` 或跟随现有模块风格。

XML：namespace 使用小写业务域名（`player`、`room`、`common`）；XML type name 使用 `PascalCase`；XML field name 使用 `snake_case`；service/method name 使用 `PascalCase`。

```xml
<types namespace="player">
  <struct name="PlayerProfile">
    <field name="player_id" id="1" type="string"/>
  </struct>
</types>

<services namespace="player">
  <service name="PlayerService" id="100">
    <call name="GetProfile" id="1" request="player.GetProfileRequest" response="player.GetProfileReply"/>
  </service>
</services>
```

### Runtime Object Model 与 Gateway 集成

```text
DescriptorPackage
  -> DescriptorRegistry
    -> TypeRegistry
    -> ServiceRegistry
    -> ErrorRegistry
    -> MapperRegistry(server only)

FrameCodec
  -> reads Shield frame

PayloadCodec
  -> uses TypeDescriptor
  -> protobuf-compatible encode/decode

RpcRuntime
  -> PendingCallRegistry
  -> StreamRegistry
  -> DescriptorRegistry
```

```text
GatewayService
  -> FrameCodec
  -> DescriptorRegistry
  -> RpcRuntime
  -> GatewayRequestDispatcher
  -> Lua/C++ service handler
```

### Core Startup

Schema runtime 作为 gateway 前置依赖：

```text
ServerCommand::run()
  -> ConfigManager
  -> SchemaStarter
       load descriptor.server.bin
       create DescriptorRegistry
       create FrameCodec/PayloadCodec
       create RpcRuntime
  -> ScriptStarter
  -> ActorStarter
  -> ServiceStarter
  -> GatewayStarter
```

如果没有配置 descriptor，gateway 可以退回现有 legacy protocol 模式。`SchemaStarter` 是独立 starter，不由 `GatewayStarter` 内部隐式创建——这样 mapper、gateway、client handshake 和 runtime validation 都能共享同一份 `DescriptorRegistry`。

### Config

```yaml
schema:
  enabled: true
  server_descriptor: "build/protocol/descriptor.server.bin"
  client_descriptor: "build/protocol/descriptor.client.bin"
  compatibility:
    allow_patch: true
    allow_minor_mismatch: true
  runtime:
    max_frame_size: 1048576
    max_string_size: 65536
    max_repeated_items: 10000
    retained_descriptor_versions: 2

mapper:
  enabled: true
  datasource: "game"
  default_timeout_ms: 1000
  max_rows: 1000
```

### CMake Structure 与 Target

分阶段接入：

- Phase 1：新增 runtime 源码到 `shield_core`；新增 `shield_protoc` 可执行文件；不移动现有 `schema_protocol.hpp/cpp`，先作为 facade；默认构建 `shield_protoc`，可通过 `SHIELD_BUILD_TOOLS=OFF` 关闭。
- Phase 2：mapper runtime 接入 `shield_extensions` 或数据访问模块；client cpp core 独立 target；TS/C# SDK 不进入主 CMake 构建。

targets：`shield_core`、`shield_extensions`、`shield_protoc`、`shield_client_cpp`。

### Generated Code Namespace

生成代码不应污染运行时 namespace。生成命名空间来自 manifest 的 `codeNamespace`（C++ generated namespace 必须由 manifest 显式声明，不从 manifest `name` 自动推导）：

```xml
<protocol-manifest name="game" codeNamespace="Game.Protocol" version="1.8.3">
```

```cpp
namespace game::protocol::player {
struct PlayerProfile;
class PlayerServiceClient;
}
```

```ts
import { ShieldClient } from "@shield/client";

export namespace player {
  export interface PlayerProfile {}
  export class PlayerServiceClient {}
}
```

```csharp
namespace Game.Protocol.Player {
    public sealed class PlayerProfile {}
    public sealed class PlayerServiceClient {}
}
```

```lua
local player = require("generated.player")
```

### Migration From Current Prototype

当前原型：`include/shield/protocol/schema_protocol.hpp`、`src/protocol/schema_protocol.cpp`。迁移步骤：

1. 保留 `schema_protocol.hpp` 作为 facade。
2. 抽出 `ProtocolValue` 到 `schema/value.hpp` 或 `wire/value.hpp`。
3. 抽出 `SchemaRegistry` 到 `schema/descriptor_registry.hpp`。
4. 抽出 `PendingRpcRegistry` 到 `rpc/pending_call_registry.hpp`。
5. 替换当前自定义字段编码为 protobuf-compatible payload codec。
6. 添加 descriptor package loader。
7. 逐步让旧 XML 单文件加载变成测试兼容路径。

### 不应做的事情

- 不要把 XML parser 链接进 gateway 热路径。
- 不要让 mapper SQL 下发到客户端 descriptor。
- 不要把 client SDK 放进服务端 plugin 系统。
- 不要继续扩大 `schema_protocol.hpp` 单文件。
- 不要让 generated code 成为 runtime 必需依赖。
- 不要让数据库 entity 自动暴露为 client DTO。

## 实施阶段

Phase 1:

- 目录化 XML 规范
- XSD + 语义校验
- canonical IR
- `descriptor.bin`、`descriptor.debug.json`
- `enum` / `struct` / `alias` / `error-set` / scalar / `list<T>` / `map<K,V>`、field id 与 reserved 校验、类型引用与 default value 校验、client/server expose profile
- `required/min/max/minLength/maxLength/minItems/maxItems` validation rule，不包含通用 `pattern`
- `send` / `call`、`common.ShieldError`、`SchemaStarter`、`shield_protoc`

Phase 2:

- Protobuf-compatible payload 编码
- `stream`
- Merkle root 和 module hash
- TypeScript 和 Unity C# wrapper
- Markdown/HTML 文档生成
- `pattern` validation（固定安全正则语义）
- client cpp core 独立 target；mapper runtime 接入 `shield_extensions`

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

## 已定规则汇总

- `SchemaStarter` 独立存在，并在 `GatewayStarter` 前启动。
- mapper runtime 不进入 `shield_core` 最小启动路径，作为 bundled extension 或 data 模块能力提供。
- `shield_protoc` 作为主仓库 target 默认构建，可通过 `SHIELD_BUILD_TOOLS=OFF` 关闭。
- hash 使用 SHA-256；`compiled_at`、`source_revision` 不参与 schema content hash；CRC32 只用于快速损坏检测。
- `service_id`、`method_id`、`field_id` 必须显式；`module_id` 由 `protocol.lock` 管理，也允许 manifest 显式指定。
- `schema_protocol.hpp` facade 只保留一个 minor release 兼容期；新代码必须迁移到目录化 schema runtime/tooling 入口。
- C++ generated namespace 必须由 manifest `codeNamespace` 显式声明；不从 manifest `name` 自动推导。
- `descriptor.bin` 内部编码第一版使用项目自定义 canonical binary encoding，不直接复用 protobuf message encoding。
- 第一版禁止 recursive struct；`expose` 默认按引用自动推导，显式 `expose="server"` 被 client-visible service 引用时编译失败。
- 通用 `pattern` 不进入 Phase 1；persistence-specific `entity` 优先放在 `mappers.xml`；JSON Schema 生成不进入 Phase 1，调试和工具生态先使用 `descriptor.debug.json`。
- mapper runtime 若恢复，应绑定数据插件接口，不新增 core 内置 lightweight DB runtime；SQL 第一版不允许多语句。
- 不恢复 `shield.db.*` 全局 mapper facade；data worker pool / coroutine-yield 执行仍是后续项。
