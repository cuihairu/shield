# Schema Descriptor

本文定义 Shield schema protocol 的运行时 descriptor 设计。`descriptor.bin` 是服务器和客户端共同加载的协议元数据包，用于动态编码、解码、校验、分发和兼容判断。

## 设计边界

`descriptor.bin` 不是生成后的业务代码，也不要求重新编译客户端或服务端。它是 XML 契约经过校验后的 canonical runtime package。

它必须支持：

- 快速加载
- 跨语言解析
- 内容 hash 稳定
- Merkle 增量比较
- 多版本共存
- 开发期可读调试产物

它不负责：

- 业务流程
- 数据库连接实现
- 客户端 UI 绑定
- 语言特定的强类型 wrapper

## Package Layout

建议将 descriptor package 分为三段：

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

第一版使用 little-endian。跨语言客户端必须显式按 little-endian 读取，不依赖平台字节序。

`package_crc32` 只用于快速发现文件损坏。安全校验和生产环境完整性应使用签名机制，放在后续阶段。

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

所有名称、命名空间、文档字符串、默认值字符串都进入 string table。Schema 对象只保存 `string_id`。

这样做的目的：

- 减小 descriptor 体积
- 保证 canonical 排序更简单
- 方便跨语言解析
- 降低重复字符串带来的 hash 噪音

第一版可以使用 UTF-8 字符串，长度使用 `uint32`。

## Canonical IR

XML 被编译为 canonical IR 后再写入 `SchemaBlock`。Canonical IR 必须消除 XML 文件层面的非语义差异：

- 注释
- 格式化
- 属性顺序
- include 顺序
- 文件拆分方式

Canonical IR 的排序规则必须固定：

- module 按 `name` 排序
- type 按 fully qualified name 排序
- service 按 `service_id` 排序
- method 按 `method_id` 排序
- field 按 `field_id` 排序
- error 按 `code` 排序
- mapper method 按 `id` 排序

## SchemaBlock

SchemaBlock 存放运行时真正需要的语义对象。

```text
SchemaBlock
  modules[]
  types[]
  services[]
  error_sets[]
  mappers[]
```

### ModuleDescriptor

```text
ModuleDescriptor
  id                  uint16
  name                string_id
  version             string_id
  hash                bytes[32]
  dependency_ids[]    uint16
```

模块 ID 可以由 manifest 显式声明，也可以由工具分配后写入 lock 文件。为了长期兼容，发布后的模块 ID 不能变。

### TypeDescriptor

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

类型 XML 语法和强校验规则见 [Schema Types](schema-types.md)。

### EnumDescriptor

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

### StructDescriptor

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

### TypeRef

```text
TypeRef
  kind                uint8      # scalar, named, list, map
  scalar              uint8
  named_type_id       uint32
  key_type            TypeRef
  value_type          TypeRef
```

第一版 map key 只允许 `string`、整数和 enum，不允许 struct 作为 key。

## ServiceDescriptor

```text
ServiceDescriptor
  id                  uint16
  module_id           uint16
  name                string_id
  full_name           string_id
  direction_mask      uint8
  methods[]
  hash                bytes[32]
```

`service_id` 进入 frame header，必须显式声明并长期稳定。

## MethodDescriptor

```text
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

规则：

- `send` 必须有 `request_type_id`，不能有 `response_type_id` 和 `item_type_id`。
- `call` 必须有 `request_type_id` 和 `response_type_id`。
- `stream` 必须有 `request_type_id` 和 `item_type_id`。
- `method_id` 在 service 内唯一，进入 frame header。
- 发布后的 `method_id` 不能复用。

## ErrorSetDescriptor

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

## MapperDescriptor

Mapper descriptor 第一版用于工具生成和服务端运行时绑定，不进入客户端默认 descriptor。

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

客户端 descriptor 应默认剔除 mapper SQL，避免把服务端数据访问细节分发到客户端。工具可以生成 server descriptor 和 client descriptor 两个 profile：

- `descriptor.server.bin`
- `descriptor.client.bin`

更详细的 mapper 契约和运行时边界见 [Schema Mapper](schema-mapper.md)。

## MerkleBlock

MerkleBlock 记录 canonical schema 的内容树。

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

Hash 输入必须包含：

- node kind
- stable identity
- canonical payload bytes
- child hashes

Hash 输入不能包含：

- compiled_at
- source_revision
- source file path
- XML formatting
- comments

## Compatibility Check

兼容检查基于两个 canonical descriptors，而不是文本 diff。

建议输出：

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

安全变更：

- 新增 optional 字段
- 新增 service method
- 新增 error code
- 新增 enum value

条件变更：

- 修改 timeout
- 修改 auth policy
- 修改 route
- 修改默认值
- 标记 deprecated

破坏性变更：

- 修改 field id
- 修改 field type
- 删除未 reserved 的字段
- 复用 field id
- 修改 service id
- 修改 method id
- 修改 call response type
- 修改 stream item type
- 修改 error code 含义

## Multi-Version Runtime

运行时必须允许 descriptor 多版本短期共存。

```text
DescriptorManager
  active_descriptor
  retained_descriptors[]
  call_descriptor_map[correlation_id]
  stream_descriptor_map[correlation_id]
```

规则：

- 新请求使用 active descriptor。
- `CALL_OK` 和 `CALL_ERR` 使用请求创建时记录的 descriptor。
- `STREAM_ITEM`、`STREAM_CLOSE` 和 `STREAM_ERR` 使用 stream 打开时记录的 descriptor。
- descriptor 没有 pending call 和 stream 引用后才可回收。

## Descriptor Debug JSON

`descriptor.debug.json` 是开发期可读产物，应与 `descriptor.bin` 来自同一 canonical IR。

示例：

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

这个文件用于：

- 客户端开发查看结构
- IDE 插件
- 协议 diff
- QA 联调工具
- 线上问题排查

## Descriptor Profiles

同一份 XML 可以编译成不同 profile：

- `server`: 包含 services、types、errors、mappers。
- `client`: 包含可暴露给客户端的 services、types、errors，不包含 SQL。
- `docs`: 包含文档注释和展示元信息。
- `debug`: 包含完整可读结构和 source mapping。

Profile 不应改变协议语义 hash。对于 client/server 差异，应分别计算 profile package hash，同时保留共同的 schema root hash。

## Open Decisions

这些问题需要在实现前定死：

- Hash 算法使用 SHA-256、BLAKE3 还是 xxHash + 签名组合。
- `module_id` 是否强制显式声明。
- `descriptor.bin` 内部编码是否直接复用 protobuf message 编码。
- `ValidationRule` 第一版支持哪些规则。
- mapper 是否进入 Phase 1，还是推迟到 descriptor 基础稳定之后。
