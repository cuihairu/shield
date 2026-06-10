# Schema Types

> Status: deferred extension design.
>
> Schema types are not part of the current refactor core. Treat this document as
> future protocol/tooling design, separate from the initial `shield.*` Lua
> runtime contract.

`types.xml` 是 Shield schema protocol 的公共类型定义层。它为 RPC、客户端 SDK、mapper、错误模型和文档生成提供统一的数据模型来源。

## 设计目标

1. 类型定义可读、可验证、可生成。
2. XML 结构由 XSD 强校验。
3. 类型引用、字段 ID、兼容性由语义校验保证。
4. 运行时使用 descriptor，不直接解析 XML。
5. RPC DTO、DB entity 和 view 能共享基础类型，但边界清晰。

## 文件位置

推荐按业务域拆分：

```text
protocol/
  common/
    types.xml
    errors.xml
  player/
    types.xml
  room/
    types.xml
```

每个 `types.xml` 必须声明 namespace：

```xml
<types namespace="player">
  ...
</types>
```

类型全名为 `namespace.name`，例如 `player.PlayerProfile`。

## 基础类型

第一版内置 scalar：

- `bool`
- `int32`
- `int64`
- `uint32`
- `uint64`
- `sint32`
- `sint64`
- `float`
- `double`
- `string`
- `bytes`

说明：

- `sint32` 和 `sint64` 使用 zigzag 编码。
- `int32`、`int64` 使用普通 varint 编码。
- `string` 必须是 UTF-8。
- `bytes` 不做字符集假设。

## Enum

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

## Struct

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
- `required` 只能作为 validation 规则，不能成为 wire format required。

保留字段：

```xml
<struct name="PlayerProfile">
  <reserved id="5"/>
  <reserved range="100-199"/>
  <field name="player_id" id="1" type="string"/>
</struct>
```

## Field

字段属性：

```xml
<field name="nickname"
       id="2"
       type="string"
       optional="true"
       default="guest"
       deprecated="false"/>
```

建议字段属性：

- `name`: 字段名。
- `id`: wire field number。
- `type`: 类型引用。
- `optional`: 是否可缺省。
- `default`: 默认值。
- `deprecated`: 是否废弃。
- `since`: 首次出现版本。
- `doc`: 短文档，可选。

字段命名建议统一使用 `snake_case`。生成器可以为 C#、TS 转换为目标语言习惯命名。

## 集合类型

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
- map key 第一版只允许 `string`、整数和 enum。
- map value 可以是 scalar、enum、struct。
- map 在 wire format 中按 repeated entry message 编码。

## Alias

Alias 用于表达领域语义，不改变 wire encoding。

```xml
<alias name="PlayerId" type="string"/>
<alias name="RoomId" type="uint64"/>
```

生成器可以用 alias 生成更明确的类型别名。运行时 descriptor 保留 alias 信息用于文档和校验。

## Entity

`entity` 是持久化模型，通常在 mapper 文件中定义，也可以放在 types 文件中集中管理。

```xml
<entity name="PlayerEntity" table="player">
  <field name="player_id" column="player_id" type="string" id="1" primaryKey="true"/>
  <field name="nickname" column="nickname" type="string" id="2"/>
</entity>
```

规则：

- entity 可以参与 mapper result mapping。
- entity 默认不暴露给客户端 descriptor。
- entity 不应直接作为公网 RPC response，除非显式 `expose="client"`。

## View / DTO

对外协议建议使用 struct 作为 DTO 或 view：

```xml
<struct name="PlayerProfile" expose="client">
  <field name="player_id" id="1" type="string"/>
  <field name="nickname" id="2" type="string"/>
  <field name="level" id="3" type="int32"/>
</struct>
```

建议：

- 客户端可见类型显式 `expose="client"`。
- 服务端内部类型默认 `expose="server"`。
- 公共错误类型默认 `expose="both"`。

## 错误类型

公共错误结构通常放在 `common/errors.xml`，但语义上仍属于类型系统。

```xml
<struct name="ShieldError" expose="both">
  <field name="code" id="1" type="int32"/>
  <field name="name" id="2" type="string"/>
  <field name="category" id="3" type="common.ErrorCategory"/>
  <field name="message" id="4" type="string"/>
  <field name="retryable" id="5" type="bool" default="false"/>
  <field name="details" id="6" type="map<string,string>" optional="true"/>
</struct>
```

错误码集合：

```xml
<error-set name="PlayerErrors">
  <error code="20001" name="PLAYER_NOT_FOUND" category="Business"/>
  <error code="20002" name="PLAYER_BANNED" category="Business"/>
</error-set>
```

## Validation

Validation 分为四层：

1. XML 结构校验。
2. 类型语义校验。
3. 兼容性校验。
4. 运行时数据校验。

### XML 结构校验

由 XSD 完成：

- 根节点是否合法。
- 必填属性是否存在。
- 属性类型是否正确。
- 子节点顺序和层级是否合法。
- enum、struct、field 等元素是否出现在允许位置。

XSD 不能完成跨文件引用、ID 唯一性和兼容性判断，这些由语义校验完成。

### 类型语义校验

编译器必须检查：

- namespace 合法。
- 类型名合法。
- 同 namespace 内类型名唯一。
- 类型引用存在。
- field id 唯一。
- field id 不在 reserved 范围。
- field type 合法。
- default value 能转换为字段类型。
- map key 类型合法。
- enum value 唯一。
- error code 唯一。
- expose profile 不泄漏 server-only 类型。

### 兼容性校验

编译器对比旧 descriptor 和新 descriptor：

安全变更：

- 新增 optional field。
- 新增 struct。
- 新增 enum value。
- 新增 error code。
- 新增 server-only 类型。

条件变更：

- 新增 validation rule。
- 修改默认值。
- 标记 deprecated。
- 修改 expose profile。

破坏性变更：

- 修改 field id。
- 修改 field type。
- 删除 field 但未 reserved。
- 复用 reserved id。
- 修改 enum value 的含义。
- 删除客户端仍可见类型。
- 将 optional 字段改成 required validation。

### 运行时数据校验

运行时按 descriptor 校验：

- required validation。
- string 长度。
- bytes 长度。
- 数值范围。
- list 长度。
- map 长度。
- enum 是否在已知范围。
- 已启用的内置 validation rule。

运行时校验失败统一返回 `common.ShieldError`，category 为 `Validation`。

## Validation Rule

字段级规则：

```xml
<field name="nickname" id="2" type="string">
  <validate required="true" minLength="2" maxLength="16"/>
</field>
```

数值范围：

```xml
<field name="level" id="3" type="int32">
  <validate min="1" max="999"/>
</field>
```

集合长度：

```xml
<field name="items" id="4" type="list<player.ItemInfo>">
  <validate maxItems="200"/>
</field>
```

第一版建议支持：

- `required`
- `min`
- `max`
- `minLength`
- `maxLength`
- `minItems`
- `maxItems`

暂不支持：

- 通用 `pattern` 正则。
- 任意脚本表达式。
- 跨字段复杂约束。
- 数据库唯一性校验。
- 远程服务校验。

复杂业务规则应放在 service 实现中。

通用 `pattern` 放到 Phase 2。若启用，建议固定为 RE2 语义或受限正则子集，避免不同语言 regex 行为不一致和 ReDoS 风险。

## XSD 范围

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

XSD 不应尝试表达所有语义规则。比如跨文件类型引用、reserved ID 和兼容性检查必须由 `shield protoc` 完成。

## Descriptor 映射

`types.xml` 编译后进入 descriptor：

```text
TypeDescriptor
  id
  module_id
  name
  full_name
  kind
  flags
  hash

StructDescriptor
  type_id
  fields[]

FieldDescriptor
  id
  name
  type_ref
  cardinality
  default_value
  validation_rules
  deprecated
  reserved
```

字段按 `field id` 排序后参与 canonical hash。

## Client Profile

类型暴露规则：

- `expose="client"`: 进入客户端 descriptor。
- `expose="server"`: 只进入服务端 descriptor。
- `expose="both"`: 同时进入。
- 未声明时，struct 默认 server，RPC request/response 引用到的类型自动提升到 client-visible。

编译器必须防止 client-visible 类型引用 server-only 类型。

## Debug JSON

开发期生成的 `descriptor.debug.json` 应包含类型详情：

```json
{
  "types": [
    {
      "fullName": "player.PlayerProfile",
      "kind": "struct",
      "expose": "client",
      "fields": [
        {
          "id": 1,
          "name": "player_id",
          "type": "string",
          "optional": true
        }
      ]
    }
  ]
}
```

这个文件用于 IDE 插件、协议 diff、调试面板和联调工具。

## Phase 1 范围

第一版建议实现：

- `enum`
- `struct`
- `alias`
- `error-set`
- scalar 类型
- `list<T>`
- `map<K,V>`
- field id / reserved 校验
- 类型引用校验
- default value 校验
- `required/min/max/minLength/maxLength/minItems/maxItems`
- client/server expose profile

推迟：

- oneof
- union
- recursive struct
- 复杂泛型
- 跨字段 validation
- 通用 `pattern`
- 自定义 validation plugin
- schema migration 生成

## Open Decisions

- 是否生成 JSON Schema 作为调试和工具生态补充。

已定规则：

- 第一版禁止 recursive struct。
- `expose` 默认按引用自动推导，显式 `expose="server"` 被 client-visible service 引用时编译失败。
- 通用 `pattern` 不进入 Phase 1。
- persistence-specific `entity` 优先放在 `mappers.xml`。
