# Xmldef Compiler / Runtime MVP

> Status: draft / implementation-structure spec.
>
> 本文把 `xmldef` Phase 1 的代码落点再明确一层：哪些模块属于 compiler toolchain，哪些属于 runtime descriptor 层，哪些属于 transport 集成层，哪些属于 Lua binding 编译层。目标是让后续实现不再围绕一个“大而杂的 xmldef codec”展开，而是按干净边界拆开。

如果需要看 Phase 1 范围和交付物，请先读 [Xmldef Phase 1 Implementation Plan](xmldef-phase1-implementation.md)。如果需要看 descriptor 字段契约，请读 [Xmldef Descriptor Spec](xmldef-descriptor-spec.md)。

## Goal

Phase 1 的实现结构要保证四件事：

1. XML 解释只发生在 toolchain，不发生在 runtime 热路径。
2. runtime 只依赖 compiled descriptor，不依赖 XML 文件。
3. `XmldefBodyCodec` 只负责 codec 语义，不兼管 binding、manifest、代码生成。
4. Lua handler 查找在启动时编译完成，运行时只做 `route_id` 命中。

补充一点：这里的 `XmldefBodyCodec` 是“当前 Phase 1 方便落地的 runtime 适配名”，不代表 `xmldef` 只能绑定单一 native binary 表示。长期模型里，`xmldef` 更偏向 contract source / descriptor provider，后续可驱动多种 payload codec。

## Layer Split

推荐拆成这四层：

```text
xmldef xml
  -> compiler toolchain
  -> descriptor artifacts
  -> runtime descriptor layer
  -> transport/lua integration
```

对应职责：

- compiler toolchain：读 manifest/XML、做语义校验、构建 canonical IR、输出 descriptor
- descriptor artifacts：`descriptor.debug.json`、`route_constants.json`、`manifest.json`
- runtime descriptor layer：加载 descriptor 并建立查询索引
- transport/lua integration：`XmldefBodyCodec`、protocol pipeline adapter、Lua binding registry

## Recommended Source Layout

基于仓库当前结构，推荐这样落：

```text
tools/xmldef_compiler/
  main.cpp
  manifest_loader.hpp/.cpp
  xml_loader.hpp/.cpp
  semantic_validator.hpp/.cpp
  canonical_ir.hpp/.cpp
  descriptor_json_writer.hpp/.cpp
  route_constants_writer.hpp/.cpp
  manifest_writer.hpp/.cpp

include/shield/transport/xmldef/
  descriptor_registry.hpp
  schema_types.hpp
  xmldef_codec.hpp
  lua_binding_registry.hpp

src/transport/xmldef/
  descriptor_registry.cpp
  xmldef_codec.cpp
  lua_binding_registry.cpp

tests/transport/
  test_xmldef_descriptor_registry.cpp
  test_xmldef_codec.cpp
  test_xmldef_lua_binding_registry.cpp

tests/integration/
  test_xmldef_gateway_flow.cpp
```

### Why Under `transport`

Phase 1 的 runtime `xmldef` 仍然服务于固定 protocol pipeline，因此建议先落在 `shield_transport` 子目录中。

但要注意：

- 这是 runtime 侧的 descriptor/codec/binding 层
- 不是 compiler toolchain
- 不是 Unity/U3D generator

toolchain 不应塞进 `src/transport/`。

## Compiler MVP Modules

### 1. ManifestLoader

职责：

- 读取 `protocol/manifest.xml`
- 解析 package metadata
- 收集 module 列表
- 产出“需要继续加载哪些 XML 文件”的稳定输入

不负责：

- schema 语义校验
- type/method 编号分配
- descriptor 输出

最小接口建议：

```text
load_manifest(path) -> ManifestModel
```

### 2. XmlLoader

职责：

- 加载 module XML
- 解析 message/type/enum/field 原始语法树
- 保留 source location 供错误报告使用

不负责：

- 业务语义判断
- ID 稳定性策略

最小接口建议：

```text
load_modules(manifest) -> RawModuleAst[]
```

### 3. SemanticValidator

职责：

- cross-file type resolution
- method/type/field/enum 唯一性检查
- direction/kind 合法性检查
- client-visible 引用边界检查
- reserved ID 冲突检查

这是 compiler MVP 的关键闸口。  
任何不满足 descriptor 契约的 XML，都应在这里 fail fast。

最小接口建议：

```text
validate(raw_modules) -> ValidatedSchemaModel
```

### 4. CanonicalIrBuilder

职责：

- 生成稳定 module/type/method/route 记录
- 归一化 `direction`
- 归一化 `kind`
- 构建 type_ref 树
- 做 canonical sort
- 计算 hash 输入

这里产出的 IR，才是 writer 和 generator 的真正输入。

最小接口建议：

```text
build_ir(validated_model) -> CanonicalPackage
```

### 5. DescriptorJsonWriter

职责：

- 输出 `descriptor.debug.json`
- 确保字段名、字段顺序和 shape 稳定
- 不丢失 generator/runtime 所需字段

这层不应再重新推导语义，只做 IR 序列化。

### 6. RouteConstantsWriter

职责：

- 从 canonical IR 输出简化 route 表
- 方便前端 generator、脚本工具和测试直接消费

### 7. ManifestWriter

职责：

- 输出 build artifact manifest
- 记录 package/version/hash/生成文件列表

它不属于 schema 语义本身，但属于 Phase 1 交付物的一部分。

## Runtime MVP Modules

### 1. DescriptorRegistry

职责：

- 加载 `descriptor.debug.json`
- 建立查询索引
- 提供 method/type/route lookup API

推荐内部索引：

- `route_id -> MethodDescriptor`
- `full_name -> MethodDescriptor`
- `type_id -> TypeDescriptor`
- `method_id -> MethodDescriptor`

推荐最小接口：

```text
load_descriptor(path) -> DescriptorRegistry
find_method_by_route(route_id)
find_method_by_name(full_name)
find_type(type_id)
find_request_type(route_id)
find_response_type(route_id)
find_binding_hint(route_id)
```

`DescriptorRegistry` 不负责：

- Lua function 查找
- bytes encode/decode
- XML fallback

### 2. SchemaTypes

Phase 1 虽然只用 debug JSON，也仍建议把 runtime 内部 type model 单独抽出来，而不是直接把 JSON object 散传。

建议抽象：

- `TypeDescriptor`
- `StructDescriptor`
- `FieldDescriptor`
- `EnumDescriptor`
- `MethodDescriptor`
- `RouteDescriptor`
- `TypeRef`

这样后续切换到 `descriptor.bin` 时，不需要改 `XmldefBodyCodec` 和 Lua binding 层接口。

### 3. XmldefBodyCodec

职责：

- 根据 `route_id` 查 method schema
- decode bytes -> structured business message
- encode business message -> bytes

它依赖：

- `DescriptorRegistry`
- `MethodDescriptor`
- `TypeDescriptor`

它不负责：

- route policy
- route extraction
- Lua dispatch
- XML 文件读取

推荐内部拆成：

- `decode_scalar`
- `decode_enum`
- `decode_struct`
- `decode_list`
- `encode_scalar`
- `encode_enum`
- `encode_struct`
- `encode_list`

这样后续补 `map` 时可以自然扩展。

### 4. LuaBindingRegistry

职责：

- 读取 Lua service 暴露的 binding table
- 对照 descriptor methods 做启动期校验
- 编译出 `route_id -> cached lua function`

推荐接口：

```text
compile_bindings(descriptor_registry, lua_module) -> LuaBindingRegistry
find_handler(route_id) -> cached_function
```

运行时 dispatch 应只做：

```text
route_id -> cached_function
```

不应做：

- `full_name` 字符串查找
- 每包反射式 `module[handler_name]`
- 动态猜函数名

## Transport Integration

`xmldef` 接入固定 protocol pipeline 的方式应该非常窄：

```text
Envelope
  -> RouteExtractor
  -> RoutePolicy
  -> XmldefBodyCodec.decode
  -> LuaBindingRegistry dispatch
```

出站：

```text
Lua business message
  -> RouteResolver
  -> XmldefBodyCodec.encode
  -> Envelope
```

这部分意味着 `xmldef` runtime 只需要对 `BodyCodec` 槽位和 Lua dispatch 槽位负责。  
它不应重新定义一套 gateway middleware。

## Startup Order

Phase 1 推荐启动顺序：

1. 读取 `descriptor.debug.json`
2. 构建 `DescriptorRegistry`
3. 初始化 `XmldefBodyCodec`
4. 加载 Lua gateway service module
5. 编译 `LuaBindingRegistry`
6. 验证所有 required binding
7. 开始接收客户端流量

关键点是：

- descriptor 要先于 Lua binding 编译
- Lua binding 要先于 session 收包
- binding 缺失属于启动失败，不属于运行时弱告警

## Validation Split

必须区分两类错误：

### Compiler-time Errors

- route/type/field/enum ID 冲突
- type 引用不存在
- direction/kind 非法
- client-visible 引用 server-only

这些错误属于 schema 契约错误，必须由 compiler 报。

### Startup-time Errors

- descriptor 文件缺失
- descriptor 文件格式版本不兼容
- Lua binding 缺失
- binding 指向不存在函数

这些错误属于部署/接线错误，必须在 runtime startup 报。

### Runtime Errors

- decode bytes 失败
- enum value 非法
- required field 缺失
- egress message shape 不匹配 schema

这些错误属于请求级错误，必须作为 transport/protocol 错误返回或记录。

## Test Split

测试建议按层拆，不要只做一条大集成测试：

### Compiler Tests

- manifest load
- module load
- semantic validation fail cases
- canonical ordering stability
- `descriptor.debug.json` golden tests

### Runtime Unit Tests

- `DescriptorRegistry` lookup
- `XmldefBodyCodec` scalar/enum/struct/list encode/decode
- `LuaBindingRegistry` compile/validation

### Integration Tests

- protocol pipeline ingress decode-local -> Lua handler
- Lua `session:send(...)` -> xmldef encode -> envelope
- binding 缺失导致启动失败

## Phase 1 Non-goals

这篇文档不要求 Phase 1 直接做这些事情：

- `descriptor.bin` runtime load
- Unity/U3D generator 实现
- Unreal/TS generator
- XML hot reload
- schema patch / diff
- entity/component runtime

这些要么属于后续 phase，要么属于独立 tooling plugin。
