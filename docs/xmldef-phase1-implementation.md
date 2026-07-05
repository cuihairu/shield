# Xmldef Phase 1 Implementation Plan

> Status: draft / implementation-phase spec.
>
> 本文把 `xmldef` 的设计草案继续收敛为 Phase 1 可开工范围，冻结 compiler MVP、`descriptor.debug.json` 固定格式、runtime `DescriptorRegistry` 最小执行面，以及 `XmldefBodyCodec` 第一版边界。目标不是覆盖完整 schema 系统，而是定义一个最小、干净、可逐步扩展的第一阶段交付面。
>
> 如果关注“代码应该怎么拆、哪些模块属于 toolchain、哪些模块属于 runtime/transport/Lua”，请结合 [Xmldef Compiler / Runtime MVP](xmldef-compiler-runtime-mvp.md) 一起阅读。

## Phase 1 Goal

Phase 1 的目标不是完成完整 `KBEngine/BigWorld` 风格 schema runtime，而是先打通这条最小闭环：

```text
xmldef xml
  -> compiler MVP
  -> descriptor.debug.json + route_constants.json
  -> runtime DescriptorRegistry
  -> XmldefBodyCodec MVP
  -> Lua binding registry MVP
```

交付完成后，应满足：

1. 服务端不再直接依赖 XML 热路径解析
2. runtime 能从 descriptor 读到 `route_id -> method schema`
3. `xmldef` 能作为真实 `BodyCodec` 的第一版执行面存在
4. Unity/U3D generator 已有稳定输入格式，即使生成器本身还未完全实现

## Out Of Scope

Phase 1 明确不做：

- `descriptor.bin` 二进制格式冻结
- 多版本 descriptor patch / Merkle 增量
- 完整 call/stream schema runtime
- Unity 网络 SDK 实现
- Unreal / TS generator
- XML mapper / entity runtime
- 高级 validation 规则执行

Phase 1 先冻结 JSON debug descriptor 作为稳定输入输出契约。

## Deliverables

Phase 1 必须交付四块：

1. `xmldef` compiler MVP
2. `descriptor.debug.json` 固定格式
3. runtime `DescriptorRegistry` MVP
4. runtime `XmldefBodyCodec + Lua binding registry` MVP

这里的命名是 Phase 1 落地口径，不代表 `xmldef` 在架构上必须等于单一 body wire format。长期上，`xmldef` 更像 contract source / descriptor provider；Phase 1 只是先把它通过一个 runtime codec 适配进固定 pipeline。

## Compiler MVP

### Input

compiler MVP 输入：

- `protocol/manifest.xml`
- referenced module XML files

### Output

compiler MVP 最小输出：

- `descriptor.debug.json`
- `route_constants.json`
- `manifest.json`

Phase 1 可以先不产出最终 `descriptor.bin`，或者只保留空壳占位。  
真正稳定的 runtime/generator 契约，Phase 1 以 `descriptor.debug.json` 为准。

### Required Features

compiler MVP 必须支持：

- manifest 读取
- module 加载
- `message/method` 收集
- type graph 收集
- semantic validation
- canonical ordering
- JSON descriptor 输出

### Required Semantic Validation

Phase 1 至少要拦住这些错误：

- 重复 `route_id`
- 重复 method full name
- request/response type 缺失
- struct 字段 ID 冲突
- struct 字段名冲突
- enum value 冲突
- enum name 冲突
- 非法 `direction`
- 非法 `kind`
- client-visible 类型引用 server-only 类型

### Deferred Validation

以下校验留到后续 phase：

- schema compatibility diff
- Merkle subtree diff
- 高级 validation rule 执行
- 复杂 binding hint 策略
- entity / mapper 交叉检查

## descriptor.debug.json Format

### Why JSON First

Phase 1 先冻结 `descriptor.debug.json`，理由很直接：

- generator plugin 更容易接入
- runtime prototype 更容易调试
- QA / 工具更容易检查
- 可以先把 compiler/runtime/generator 解耦

二进制 `descriptor.bin` 可以在 Phase 2 再严格收口。

### Top-level Shape

固定顶层结构如下：

```json
{
  "format": "shield.xmldef.descriptor.debug.v1",
  "package": { ... },
  "modules": [ ... ],
  "types": [ ... ],
  "methods": [ ... ],
  "routes": [ ... ]
}
```

`format` 是强制字段，用于 generator/runtime 做版本检查。

### package

固定字段：

```json
{
  "id": "game",
  "name": "game",
  "version": "1.0.0",
  "code_namespace": "Game.Protocol",
  "compiler_version": "0.1.0",
  "schema_root_hash": "hex-string"
}
```

### modules

每个 module：

```json
{
  "module_id": 1,
  "name": "avatar",
  "path": "avatar",
  "dependencies": [0],
  "hash": "hex-string"
}
```

### types

每个 type 最小字段：

```json
{
  "type_id": 1001,
  "module_id": 1,
  "full_name": "avatar.MoveRequest",
  "kind": "struct",
  "client_visibility": "client",
  "hash": "hex-string",
  "struct": {
    "fields": [ ... ],
    "reserved_field_ids": []
  }
}
```

enum 示例：

```json
{
  "type_id": 1002,
  "module_id": 1,
  "full_name": "avatar.MoveMode",
  "kind": "enum",
  "client_visibility": "client",
  "hash": "hex-string",
  "enum": {
    "values": [
      { "name": "Walk", "value": 0 },
      { "name": "Run", "value": 1 }
    ]
  }
}
```

### type_ref

字段引用固定为显式对象，而不是字符串缩写：

```json
{
  "kind": "named",
  "named_type_id": 1001
}
```

list：

```json
{
  "kind": "list",
  "item": {
    "kind": "named",
    "named_type_id": 1001
  }
}
```

map：

```json
{
  "kind": "map",
  "key": { "kind": "scalar", "scalar_kind": "string" },
  "value": { "kind": "named", "named_type_id": 1001 }
}
```

### methods

每个 method 固定字段：

```json
{
  "method_id": 1,
  "route_id": 4097,
  "schema_id": 33,
  "module_id": 1,
  "full_name": "Avatar.move",
  "short_name": "move",
  "kind": "send",
  "direction": "client_to_server",
  "request_type_id": 1001,
  "response_type_id": null,
  "item_type_id": null,
  "target_hint": null,
  "binding_hint": "on_avatar_move",
  "hash": "hex-string"
}
```

### routes

必须保留显式 route 表：

```json
{
  "route_id": 4097,
  "method_id": 1,
  "full_name": "Avatar.move",
  "direction": "client_to_server",
  "schema_id": 33
}
```

### route_constants.json

Phase 1 同时输出简化 route 表：

```json
{
  "format": "shield.xmldef.route_constants.v1",
  "package": "game",
  "version": "1.0.0",
  "routes": [
    {
      "route_id": 4097,
      "name": "Avatar.move",
      "schema_id": 65,
      "direction": "client_to_server"
    },
    {
      "route_id": 4098,
      "name": "Login.reqAuth",
      "schema_id": 33,
      "direction": "client_to_server"
    }
  ]
}
```

## End-to-end Example

### Source Sketch

下面给一份最小但完整的示例，帮助后续 compiler/runtime/generator 对齐语义：

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

假定 XML 语义里包含：

- `common.Vector3`
- `login.AuthRequest`
- `login.CharacterSummary`
- `login.AuthReply`
- `avatar.MoveMode`
- `avatar.MoveRequest`
- `Login.reqAuth` (`call`, `client_to_server`)
- `Avatar.move` (`send`, `client_to_server`)

### Example descriptor.debug.json

下面这份 JSON 应当可以直接作为 generator、QA 工具和 runtime loader 的对照样例：

```json
{
  "format": "shield.xmldef.descriptor.debug.v1",
  "package": {
    "id": "game",
    "name": "game",
    "version": "1.0.0",
    "code_namespace": "Game.Protocol",
    "compiler_version": "0.1.0",
    "schema_root_hash": "b4556a5e4e8a6e3a90d3c55df4b95a2cf4f06243f4f4a6b0db8b28331ad0b6ff"
  },
  "modules": [
    {
      "module_id": 1,
      "name": "common",
      "path": "common",
      "dependencies": [],
      "hash": "fd148f46a6db2722ea6bd1db0f1bd1af2f2324d35f0b4d6a466095a84e4a7771"
    },
    {
      "module_id": 2,
      "name": "login",
      "path": "login",
      "dependencies": [1],
      "hash": "6a17d7f6de4ea0cef5dd7b31d7c91f6fa0f0682f565f6f83b5fcdf9ccaa3d70c"
    },
    {
      "module_id": 3,
      "name": "avatar",
      "path": "avatar",
      "dependencies": [1],
      "hash": "22ac0710f57aeb1b09f4e8f8a6dbbe0a0d0a7c58e81b080c41d0c9db64167c53"
    }
  ],
  "types": [
    {
      "type_id": 1001,
      "module_id": 1,
      "full_name": "common.Vector3",
      "kind": "struct",
      "client_visibility": "client",
      "hash": "eb401d58db8a4fc0133bcf396f3e3ddc0f7ba0beee0cb9f7bf8d972f22f4fd01",
      "struct": {
        "fields": [
          {
            "field_id": 1,
            "name": "x",
            "type_ref": { "kind": "scalar", "scalar_kind": "float" },
            "cardinality": "optional",
            "default_value": null,
            "deprecated": false,
            "validation_rules": []
          },
          {
            "field_id": 2,
            "name": "y",
            "type_ref": { "kind": "scalar", "scalar_kind": "float" },
            "cardinality": "optional",
            "default_value": null,
            "deprecated": false,
            "validation_rules": []
          },
          {
            "field_id": 3,
            "name": "z",
            "type_ref": { "kind": "scalar", "scalar_kind": "float" },
            "cardinality": "optional",
            "default_value": null,
            "deprecated": false,
            "validation_rules": []
          }
        ],
        "reserved_field_ids": []
      }
    },
    {
      "type_id": 2001,
      "module_id": 2,
      "full_name": "login.AuthRequest",
      "kind": "struct",
      "client_visibility": "client",
      "hash": "f0a4a613547f1df40cdf1f6bfb8e17d5f0118028470d4db6fd311d223eef09c2",
      "struct": {
        "fields": [
          {
            "field_id": 1,
            "name": "account",
            "type_ref": { "kind": "scalar", "scalar_kind": "string" },
            "cardinality": "optional",
            "default_value": null,
            "deprecated": false,
            "validation_rules": []
          },
          {
            "field_id": 2,
            "name": "token",
            "type_ref": { "kind": "scalar", "scalar_kind": "string" },
            "cardinality": "optional",
            "default_value": null,
            "deprecated": false,
            "validation_rules": []
          }
        ],
        "reserved_field_ids": []
      }
    },
    {
      "type_id": 2002,
      "module_id": 2,
      "full_name": "login.CharacterSummary",
      "kind": "struct",
      "client_visibility": "client",
      "hash": "32b0ff4418a4fb4c28329cae05f0946e8dff0a56da94290435603f49102cf244",
      "struct": {
        "fields": [
          {
            "field_id": 1,
            "name": "player_id",
            "type_ref": { "kind": "scalar", "scalar_kind": "uint64" },
            "cardinality": "optional",
            "default_value": null,
            "deprecated": false,
            "validation_rules": []
          },
          {
            "field_id": 2,
            "name": "nickname",
            "type_ref": { "kind": "scalar", "scalar_kind": "string" },
            "cardinality": "optional",
            "default_value": null,
            "deprecated": false,
            "validation_rules": []
          },
          {
            "field_id": 3,
            "name": "level",
            "type_ref": { "kind": "scalar", "scalar_kind": "uint32" },
            "cardinality": "optional",
            "default_value": null,
            "deprecated": false,
            "validation_rules": []
          }
        ],
        "reserved_field_ids": []
      }
    },
    {
      "type_id": 2003,
      "module_id": 2,
      "full_name": "login.AuthReply",
      "kind": "struct",
      "client_visibility": "client",
      "hash": "5e31d9ee12f1fd04f2c5962e8fd893fe6ef61dcb5e84b154f6effe9ee753ccf0",
      "struct": {
        "fields": [
          {
            "field_id": 1,
            "name": "session_token",
            "type_ref": { "kind": "scalar", "scalar_kind": "string" },
            "cardinality": "optional",
            "default_value": null,
            "deprecated": false,
            "validation_rules": []
          },
          {
            "field_id": 2,
            "name": "characters",
            "type_ref": {
              "kind": "list",
              "item": {
                "kind": "named",
                "named_type_id": 2002
              }
            },
            "cardinality": "optional",
            "default_value": null,
            "deprecated": false,
            "validation_rules": []
          }
        ],
        "reserved_field_ids": []
      }
    },
    {
      "type_id": 3001,
      "module_id": 3,
      "full_name": "avatar.MoveMode",
      "kind": "enum",
      "client_visibility": "client",
      "hash": "00fa7f6a0c3d2bd6dbb7ec8a53d6bcb93fd46fa38642ad45653022fb7ee65f90",
      "enum": {
        "values": [
          { "name": "Walk", "value": 0 },
          { "name": "Run", "value": 1 }
        ]
      }
    },
    {
      "type_id": 3002,
      "module_id": 3,
      "full_name": "avatar.MoveRequest",
      "kind": "struct",
      "client_visibility": "client",
      "hash": "f2b94ed7bbd11a64d7cf45cb975484617b1b5b0af27960486cf2ba1139de9ff0",
      "struct": {
        "fields": [
          {
            "field_id": 1,
            "name": "entity_id",
            "type_ref": { "kind": "scalar", "scalar_kind": "uint64" },
            "cardinality": "optional",
            "default_value": null,
            "deprecated": false,
            "validation_rules": []
          },
          {
            "field_id": 2,
            "name": "position",
            "type_ref": { "kind": "named", "named_type_id": 1001 },
            "cardinality": "optional",
            "default_value": null,
            "deprecated": false,
            "validation_rules": []
          },
          {
            "field_id": 3,
            "name": "mode",
            "type_ref": { "kind": "named", "named_type_id": 3001 },
            "cardinality": "optional",
            "default_value": null,
            "deprecated": false,
            "validation_rules": []
          }
        ],
        "reserved_field_ids": []
      }
    }
  ],
  "methods": [
    {
      "method_id": 1,
      "route_id": 4098,
      "schema_id": 33,
      "module_id": 2,
      "full_name": "Login.reqAuth",
      "short_name": "reqAuth",
      "kind": "call",
      "direction": "client_to_server",
      "request_type_id": 2001,
      "response_type_id": 2003,
      "item_type_id": null,
      "target_hint": "auth",
      "binding_hint": "on_login_req_auth",
      "hash": "265c5ffbc0bce4a727cd95df95f5560af0a96485d8d89ba89723d2be69f02efe"
    },
    {
      "method_id": 2,
      "route_id": 4097,
      "schema_id": 65,
      "module_id": 3,
      "full_name": "Avatar.move",
      "short_name": "move",
      "kind": "send",
      "direction": "client_to_server",
      "request_type_id": 3002,
      "response_type_id": null,
      "item_type_id": null,
      "target_hint": "avatar",
      "binding_hint": "on_avatar_move",
      "hash": "39a31598f6ca21141fb1b85a2faf2bf532cab4ce6289fdcb75ebf70e1ac054c4"
    }
  ],
  "routes": [
    {
      "route_id": 4097,
      "method_id": 2,
      "full_name": "Avatar.move",
      "direction": "client_to_server",
      "schema_id": 65
    },
    {
      "route_id": 4098,
      "method_id": 1,
      "full_name": "Login.reqAuth",
      "direction": "client_to_server",
      "schema_id": 33
    }
  ]
}
```

这个例子刻意覆盖了：

- `struct`
- `enum`
- named type reference
- `list<T>`
- `send`
- `call`
- request/response 对
- route table 和 method table 的双重显式表达

## Runtime DescriptorRegistry MVP

### Responsibility

Phase 1 的 `DescriptorRegistry` 只做 descriptor 查询，不做：

- 兼容性检查
- patch 合并
- 多版本共存
- 复杂 auth policy 执行

### Required Queries

Phase 1 至少实现：

- `find_method_by_route(route_id)`
- `find_method_by_name(full_name)`
- `find_type(type_id)`
- `find_request_type(route_id)`
- `find_response_type(route_id)`
- `find_binding_hint(route_id)`

### Load Source

Phase 1 runtime 直接加载 `descriptor.debug.json` 即可。  
等 binary descriptor 稳定后，再切到 `descriptor.bin` + debug json 辅助调试。

这意味着 Phase 1 runtime 不需要等 binary package 先落地。

## XmldefBodyCodec MVP

### Responsibility

Phase 1 的 `XmldefBodyCodec` 只负责：

- `route_id -> method schema`
- request/response type lookup
- minimal typed field encode/decode
- `DecodedBody.message` 产出

### Phase 1 Supported Types

第一版建议只支持：

- `bool`
- `int32` / `int64`
- `uint32` / `uint64`
- `float` / `double`
- `string`
- `bytes`
- `enum`
- `struct`

以及：

- `list<T>`

先不支持：

- `map<K,V>`
- alias-specific special handling
- entity
- union / oneof / variant

如果需要，可以把 `map` 延到 Phase 1.5。

### Egress Rule

`XmldefBodyCodec.encode` 必须接受“业务消息对象”，不能接受未建模 raw bytes 旁路。  
这和当前 `json/msgpack` structured codec 的出站边界保持一致。

### Decode Output Shape

Phase 1 decode 后给 Lua 的 `DecodedBody.message` 形状统一为 object：

```json
{
  "__xmldef_method": "Avatar.move",
  "__xmldef_route_id": 4097,
  "__xmldef_schema_id": 33,
  "entity_id": 7,
  "x": 10,
  "y": 20,
  "z": 30
}
```

是否保留这些元字段进 Lua，可以在后续 phase 再决定。  
但 runtime 内部必须保留 method metadata，不能只剩字段表。

## Lua Binding Registry MVP

### Input

Phase 1 接受显式 binding table：

```lua
M.xmldef = {
  bindings = {
    ["Avatar.move"] = "on_avatar_move"
  }
}
```

### Compile Step

启动时做一次：

```text
descriptor methods
  + lua binding table
  -> route_id -> cached function
```

### Required Validation

Phase 1 必须拦住：

- descriptor 有 method，但 binding 缺失
- binding 指向不存在函数
- 两个 method 映射到同一不兼容 handler 名的非法配置

### Runtime Dispatch

Phase 1 runtime dispatch 不做动态字符串查找，只允许：

```text
route_id -> cached Lua function
```

## Compiler/Runtime Boundary

必须保持这条边界：

- compiler 负责把 XML 解释成 descriptor
- runtime 只负责执行 descriptor

runtime 不允许：

- 直接重新读 XML
- 在热路径做 XML 语义解释
- 猜测 route / field / binding

## Recommended Module Split

Phase 1 推荐明确拆成四层，而不是把所有逻辑塞进一个 `XmldefBodyCodec`：

1. compiler：负责 `xml -> canonical IR -> descriptor.debug.json`
2. descriptor runtime：负责 descriptor load、索引和 schema 查询
3. transport adapter：负责把 runtime codec 接到固定 protocol pipeline
4. Lua binding compiler：负责启动期校验和 `route_id -> cached lua function`

更细的模块职责、建议源码布局和启动流程见 [Xmldef Compiler / Runtime MVP](xmldef-compiler-runtime-mvp.md)。

## Unity/U3D Generator Dependency

Phase 1 虽然可以暂时还不实现完整 Unity generator，但 `descriptor.debug.json` 必须已经稳定到足以支持：

- 生成 `RouteIds.cs`
- 生成 DTO
- 生成 `GatewayClient.cs`

也就是说，compiler Phase 1 的输出不是“只够服务端自用”，而是“已经够前端生成器接入”。

## Recommended Task Breakdown

建议按下面顺序开工：

1. XML reader + semantic validator
2. canonical IR builder
3. `descriptor.debug.json` writer
4. `route_constants.json` writer
5. runtime JSON descriptor loader
6. `DescriptorRegistry` query API
7. `XmldefBodyCodec` support for scalar/enum/struct/list
8. Lua binding registry compile + validation

## Done Criteria

Phase 1 可视为完成，当且仅当：

1. 给定一组 `xmldef/xml`，能稳定产出 `descriptor.debug.json`
2. runtime 能从该 descriptor 完成 `route_id -> schema` 查询
3. `XmldefBodyCodec` 能完成至少一个真实 struct message 的 encode/decode
4. Lua service 能通过显式 binding 表接收该 message
5. Unity generator 所需字段已经全部能从 descriptor.debug.json 读取

## Non-goals

以下不属于 Phase 1 完成标准：

- binary descriptor 稳定格式
- 热更新 patch
- 完整 client runtime
- Unreal / Web generator
- XML mapper runtime
