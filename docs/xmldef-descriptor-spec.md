# Xmldef Descriptor Spec

> Status: draft / deferred tooling spec.
>
> 本文把 [Xmldef Toolchain Design](xmldef-toolchain-design.md) 进一步收敛成可实现规格，定义 `xmldef/xml -> canonical IR -> descriptor package` 的输入、校验、IR、输出和稳定性规则。目标不是解释“为什么需要 descriptor”，而是给后续 compiler/runtime/generator 实现提供统一契约。

## Scope

本文只定义：

- `xmldef/xml` 输入模型
- semantic validation
- canonical IR
- descriptor package layout
- ID / hash / stability rules
- runtime 消费所需最小字段
- generator 消费所需最小字段

不定义：

- `shield_protoc` CLI UX
- Unity/U3D 具体生成代码样式
- runtime `XmldefBodyCodec` 的二进制编码细节
- 多版本 patch / Merkle 增量协议

如果目标是“这一版先做什么、做到什么算完成”，请继续阅读 [Xmldef Phase 1 Implementation Plan](xmldef-phase1-implementation.md)。
如果目标是“代码落在哪里、模块职责怎么拆”，请继续阅读 [Xmldef Compiler / Runtime MVP](xmldef-compiler-runtime-mvp.md)。

## Source Model

### Layout

建议的 `xmldef` 源文件组织：

```text
protocol/
  manifest.xml
  common/
    types.xml
  login/
    messages.xml
  avatar/
    messages.xml
```

`manifest.xml` 是唯一入口。

### Manifest

最小 manifest 语义：

```xml
<protocol-manifest name="game"
                   version="1.0.0"
                   packageId="game"
                   codeNamespace="Game.Protocol">
  <module name="common" path="common"/>
  <module name="login" path="login"/>
  <module name="avatar" path="avatar"/>
</protocol-manifest>
```

最小字段：

- `name`
- `version`
- `packageId`
- `codeNamespace`
- `module[]`

约束：

- `packageId` 在 descriptor 中长期稳定
- `codeNamespace` 不从 `name` 自动推导，必须显式声明
- `module.name` 在 package 内唯一

### Message Definition

`xmldef` 主要关注消息 / 方法 / 字段定义。最小消息模型示意：

```xml
<protocol name="avatar">
  <message id="0x1001"
           name="Avatar.move"
           direction="client_to_server"
           request="avatar.MoveRequest"
           response=""
           schema="33"/>
</protocol>
```

本文不冻结 XML 标签名细节，只冻结编译后的语义字段。

最小语义字段：

- `route_id`
- `full_name`
- `direction`
- `request_type`
- `response_type?`
- `schema_id?`
- `target_hint?`
- `binding_hint?`

### Type Definition

复用 [schema-design.md](schema-design.md) 的类型系统口径，`xmldef` 最小必须支持：

- scalar
- enum
- struct
- alias
- list
- map

其中 map key 第一版限制为：

- `string`
- integer scalar
- enum

## Semantic Validation

编译器必须在 XML 阶段完成强校验，不能把错误推给 runtime。

### Required Checks

必须校验：

- `route_id` 唯一
- `method full_name` 唯一
- `schema_id` 在 package 内唯一或满足显式命名空间约束
- `request_type` / `response_type` 引用存在
- 字段 ID 在 struct 内唯一
- 字段名在 struct 内唯一
- enum value 唯一
- enum item 名唯一
- client-visible 类型不能引用 server-only 类型
- `direction` 值合法
- XML 中声明的 reserved ID 不可复用

### Direction Rules

支持的最小方向：

- `client_to_server`
- `server_to_client`
- `bidirectional`
- `server_to_server`

编译器要把它们归一化成稳定枚举，不保留原始字符串。

### Method Kind

`xmldef` 在 descriptor 层必须明确 method kind：

- `send`
- `call`
- `stream`

规则：

- `send` 必须有 request type，不能有 response/item type
- `call` 必须有 request + response type
- `stream` 必须有 request + item type

## Canonical IR

### Top-level Shape

canonical IR 最小结构：

```text
package {
  package_id
  package_name
  package_version
  code_namespace
  modules[]
  types[]
  methods[]
  routes[]
}
```

### Package

字段：

- `package_id: string`
- `package_name: string`
- `package_version: string`
- `code_namespace: string`
- `compiler_version: string`
- `schema_root_hash: bytes[32]`

### Module

字段：

- `module_id: uint16`
- `name: string`
- `path: string`
- `hash: bytes[32]`
- `dependencies[]: module_id`

稳定性规则：

- `module_id` 必须来自 lock file 或显式声明
- 发布后不能重分配

### Type

字段：

- `type_id: uint32`
- `module_id: uint16`
- `full_name: string`
- `kind: enum`
- `client_visibility: enum`
- `hash: bytes[32]`

支持的 `kind`：

- `scalar`
- `enum`
- `struct`
- `alias`

### Struct

字段：

- `type_id`
- `fields[]`
- `reserved_field_ids[]`

Field 最小字段：

- `field_id: uint32`
- `name: string`
- `type_ref`
- `cardinality`
- `default_value?`
- `deprecated: bool`
- `validation_rules[]`

### TypeRef

最小模型：

```text
type_ref {
  kind                // scalar | named | list | map
  scalar_kind?
  named_type_id?
  list_item?
  map_key?
  map_value?
}
```

### Method

最小字段：

- `method_id: uint16`
- `route_id: uint32`
- `schema_id: uint16`
- `module_id: uint16`
- `full_name: string`
- `short_name: string`
- `kind: enum(send/call/stream)`
- `direction: enum`
- `request_type_id: uint32`
- `response_type_id?: uint32`
- `item_type_id?: uint32`
- `target_hint?: string`
- `binding_hint?: string`
- `hash: bytes[32]`

### Route

显式保留 route 表，避免 generator 或 runtime 自己从 method 里再推：

- `route_id: uint32`
- `method_id: uint16`
- `full_name: string`
- `direction`
- `schema_id`

## Canonical Ordering

为了保证稳定 hash，IR 写入 descriptor 前必须 canonical sort：

- modules 按 `module_id`
- named types 按 `type_id`
- methods 按 `route_id`
- route table 按 `route_id`
- struct fields 按 `field_id`
- enum values 按 `value`

不允许：

- 按 XML 文件顺序输出
- 按 include 顺序输出
- 按字符串 locale 排序

## Descriptor Outputs

### Required Files

最小输出：

- `descriptor.bin`
- `descriptor.debug.json`
- `route_constants.json`
- `manifest.json`

### descriptor.bin

给 runtime 用，要求：

- 快速加载
- little-endian
- 稳定 offsets
- 不依赖目标语言反射

至少包含：

- package header
- module table
- type table
- method table
- route table
- string table

### descriptor.debug.json

给 generator / debug / QA 工具使用。

最小要求：

- 可完全表达 binary descriptor 的语义内容
- 字段名明确
- 不丢失 `route_id` / `schema_id` / `direction`
- 可供 generator 无损读取

### route_constants.json

轻量产物，给简单工具、脚本和测试使用。

示意：

```json
{
  "package": "game",
  "version": "1.0.0",
  "routes": [
    {
      "route_id": 4097,
      "name": "Avatar.move",
      "schema_id": 33,
      "direction": "client_to_server"
    }
  ]
}
```

## Hashing Rules

### schema_root_hash

`schema_root_hash` 必须只依赖语义内容，不依赖：

- 编译时间
- 编译器版本
- 源码脏状态
- XML 格式化
- 注释

### meta vs schema

必须区分：

- `MetaBlock`: build info，不进入语义 hash
- `SchemaBlock`: 语义内容，进入语义 hash

## Runtime Consumption Contract

runtime 加载 descriptor 后，最少能做这些查找：

- `find_method_by_route(route_id)`
- `find_method_by_name(full_name)`
- `find_type(type_id)`
- `find_request_type(route_id)`
- `find_response_type(route_id)`

`XmldefBodyCodec` 后续实现不得再依赖 XML 源文件。

## Generator Consumption Contract

generator 侧最少要能拿到：

- package id / version / namespace
- route constants
- method full name
- request/response/item type
- direction
- field layout
- optional binding hint

如果 descriptor 缺少这些字段，则视为 compiler 契约不完整。

## Stability Rules

发布后以下 ID 不能复用：

- `module_id`
- `type_id`
- `method_id`
- `route_id`
- `schema_id`
- struct `field_id`

删除只能：

- 标记 deprecated
- 或进入 reserved

不能删掉后重新分配给新语义。

## Non-goals

本文不做这些事情：

- 定义 Unity 生成文件名细节
- 定义 runtime socket frame 格式
- 定义 Lua binding table 的最终 API 名字
- 定义 XML 标签是否必须叫 `<message>` / `<method>`

这些属于上层 generator/runtime 契约，不属于 descriptor compiler 的核心语义。
