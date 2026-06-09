# Schema Implementation Layout

本文定义 schema protocol 的源码组织、namespace、库边界、工具链位置和生成物目录。它补齐协议设计从“概念可行”到“可以开始实现”的工程约束。

## 完整性判断

当前 schema protocol 设计已经覆盖：

- XML 契约模型
- `types.xml`
- `services.xml`
- `mappers.xml`
- `descriptor.bin`
- Merkle 增量
- 强校验
- 客户端运行时和开发期产物

还必须补齐的实现约束是：

- runtime 源码放在哪里
- compiler 工具放在哪里
- namespace 如何拆
- 哪些内容进 `shield_core`
- 哪些内容是 optional extension
- client SDK 与服务端插件系统如何隔离
- generated 文件落到哪里

## 库边界

Schema protocol 拆成四类组件：

```text
shield_core
  protocol schema runtime
  wire codec
  rpc pending/stream runtime
  descriptor loader

shield_extensions
  optional metrics/health integration
  optional mapper runtime integration if it depends on database stack

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

- 运行时解码、descriptor registry、`send/call/stream` 分发属于 `shield_core`。
- XML 编译器不进入 `shield_core` 热路径。
- mapper descriptor 可以在 `shield_core` 中定义，但 mapper runtime 如果依赖数据库连接池，应放在 `shield_extensions` 或 `shield_data` 相关模块。
- 客户端 SDK 不依赖服务端 plugin 系统。
- `shield_protoc` 默认随开发构建启用，并通过 `SHIELD_BUILD_TOOLS=OFF` 关闭。
- Merkle root、module hash 和 node hash 第一版使用 SHA-256；CRC32 只用于快速损坏检测。

## 推荐源码目录

现有 `include/shield/protocol/schema_protocol.hpp` 可以作为早期兼容入口，但新实现不应继续堆在一个文件里。

推荐结构：

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

## C++ Namespace

现有模块已经使用 `shield::protocol`、`shield::gateway`、`shield::core`、`shield::data` 和 `shield::database`。Schema protocol 新代码应避免继续平铺到 `shield::protocol`。

推荐 namespace：

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

## 命名规则

文件：

- C++ header/source 使用 `lower_snake_case`。
- XML 文件使用 `lower_snake_case.xml`。
- 生成文件使用目标语言习惯，但保留 schema namespace。

C++ 类型：

- 类型名使用 `PascalCase`。
- 函数名遵循仓库现有风格，优先 `snake_case`。
- enum class 使用 `PascalCase` 类型名，枚举值使用 `UPPER_SNAKE_CASE` 或跟随现有模块风格。

XML：

- namespace 使用小写业务域名，例如 `player`、`room`、`common`。
- XML type name 使用 `PascalCase`。
- XML field name 使用 `snake_case`。
- service/method name 使用 `PascalCase`。

示例：

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

## Runtime Object Model

运行时对象关系：

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

Gateway 集成：

```text
GatewayService
  -> FrameCodec
  -> DescriptorRegistry
  -> RpcRuntime
  -> GatewayRequestDispatcher
  -> Lua/C++ service handler
```

## Core Startup

Schema runtime 应作为 gateway 前置依赖：

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

如果没有配置 descriptor，gateway 可以退回现有 legacy protocol 模式。

`SchemaStarter` 是独立 starter，不由 `GatewayStarter` 内部隐式创建。这样 mapper、gateway、client handshake 和 runtime validation 都能共享同一份 `DescriptorRegistry`。

## Config

建议配置：

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
```

Mapper 配置：

```yaml
mapper:
  enabled: true
  datasource: "game"
  default_timeout_ms: 1000
  max_rows: 1000
```

## CMake Structure

建议分阶段接入：

Phase 1:

- 新增 runtime 源码到 `shield_core`。
- 新增 `shield_protoc` 可执行文件。
- 不移动现有 `schema_protocol.hpp/cpp`，先作为 facade。
- 默认构建 `shield_protoc`，可通过 `SHIELD_BUILD_TOOLS=OFF` 关闭。

Phase 2:

- mapper runtime 接入 `shield_extensions` 或数据访问模块。
- client cpp core 独立 target。
- TS/C# SDK 不进入主 CMake 构建。

建议 targets：

```text
shield_core
shield_extensions
shield_protoc
shield_client_cpp
```

## Generated Code Namespace

生成代码不应污染运行时 namespace。

C++:

```cpp
namespace game::protocol::player {
struct PlayerProfile;
class PlayerServiceClient;
}
```

TypeScript:

```ts
import { ShieldClient } from "@shield/client";

export namespace player {
  export interface PlayerProfile {}
  export class PlayerServiceClient {}
}
```

C#:

```csharp
namespace Game.Protocol.Player {
    public sealed class PlayerProfile {}
    public sealed class PlayerServiceClient {}
}
```

Lua:

```lua
local player = require("generated.player")
```

生成命名空间来自 manifest：

```xml
<protocol-manifest name="game" codeNamespace="Game.Protocol" version="1.8.3">
```

## Migration From Current Prototype

当前原型：

```text
include/shield/protocol/schema_protocol.hpp
src/protocol/schema_protocol.cpp
```

建议迁移步骤：

1. 保留 `schema_protocol.hpp` 作为 facade。
2. 抽出 `ProtocolValue` 到 `schema/value.hpp` 或 `wire/value.hpp`。
3. 抽出 `SchemaRegistry` 到 `schema/descriptor_registry.hpp`。
4. 抽出 `PendingRpcRegistry` 到 `rpc/pending_call_registry.hpp`。
5. 替换当前自定义字段编码为 protobuf-compatible payload codec。
6. 添加 descriptor package loader。
7. 逐步让旧 XML 单文件加载变成测试兼容路径。

## 不应做的事情

- 不要把 XML parser 链接进 gateway 热路径。
- 不要让 mapper SQL 下发到客户端 descriptor。
- 不要把 client SDK 放进服务端 plugin 系统。
- 不要继续扩大 `schema_protocol.hpp` 单文件。
- 不要让 generated code 成为 runtime 必需依赖。
- 不要让数据库 entity 自动暴露为 client DTO。

## Open Decisions

- `schema_protocol.hpp` facade 兼容期保留多久。
- C++ generated namespace 是否默认使用 manifest `name`，还是必须显式 `codeNamespace`。

已定规则：

- `SchemaStarter` 独立存在，并在 `GatewayStarter` 前启动。
- mapper runtime 不进入 `shield_core` 最小启动路径，作为 bundled extension 或 data 模块能力提供。
- `shield_protoc` 作为主仓库 target 默认构建。
- hash 使用 SHA-256；`compiled_at`、`source_revision` 不参与 schema content hash。
- `service_id`、`method_id`、`field_id` 必须显式；`module_id` 由 `protocol.lock` 管理，也允许 manifest 显式指定。
