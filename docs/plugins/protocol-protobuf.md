# protocol.protobuf

> Protobuf 编解码插件：`FileDescriptorSet` 驱动的 DynamicMessage 编解码，跨语言客户端与 schema 演进的首选。

`protocol.protobuf` 是官方提供的 [`shield.protocol.codec.v1`](/protocol-codec-plugins) 之一，codec 名为 `protobuf`。create 时加载 `FileDescriptorSet` 描述符文件建立 `DescriptorPool` + `DynamicMessageFactory`，此后按路由的类型名动态解析 message——业务工程**不需要**生成或链接 C++ 桩代码，服务端零桩代码接入。需要通过 `body.provider` 绑定本插件启用。

**选型速览**：唯一自带正式 schema 演进模型（field number 兼容、跨语言代码生成）的选择，客户端是 Unity/UE/Go/Java 等多语言栈时的首选；客户端就是 Lua/JS、只想快速起步选 [protocol.json](/plugins/protocol-json)；要二进制体积但不要 schema 编译器选 [protocol.msgpack](/plugins/protocol-msgpack)；要 flatc 生态选 [protocol.flatbuffers](/plugins/protocol-flatbuffers)。

## 包信息

- **包 ID**: `protocol.protobuf`
- **接口**: [`shield.protocol.codec.v1`](/protocol-codec-plugins)（capabilities: `protobuf`）
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_PLUGIN_PROTOBUF`
- **源码**: `plugins/protocol.protobuf/`
- **依赖**: [protobuf](https://protobuf.dev/)（vcpkg 端口 `protobuf`，`protobuf::libprotobuf` CMake target，含 descriptor/DynamicMessage/json_util）+ nlohmann/json

## 构建启用

在 CMake 配置阶段打开 `SHIELD_BUILD_PLUGIN_PROTOBUF`：

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_PROTOBUF=ON
cmake --build build
```

该选项触发两件事：

1. `plugins/protocol.protobuf/` 下的 shared library 被构建，输出到 `plugins/protocol.protobuf/bin/`。
2. CMake 把 vcpkg manifest feature `protocol-protobuf` 透传给 vcpkg，自动安装 `protobuf` 端口（含 libprotobuf 与 protoc）。

CI 的 ci.yml 与 plugins-ci 工作流均以 `SHIELD_BUILD_PLUGIN_PROTOBUF=ON` 构建（`test_protocol_protobuf_plugin`，含真实 descriptor round-trip 测试）。

## 配置 Schema

实例配置只有一个键，**必填**：

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `descriptor_set` | string | 是 | — | `FileDescriptorSet` 二进制文件路径。缺失、非字符串、打不开或解析失败都在 create 期报 `plugin.config.invalid`（bootstrap 启动失败，不会带病运行）。 |

生成描述符文件（`--include_imports` 把依赖的 .proto 一并嵌入，建议总是带上）：

```bash
protoc --include_imports --descriptor_set_out=conf/game.pb game.proto
```

描述符在 create 期一次加载：`ParseFromString` → 逐文件加入 `SimpleDescriptorDatabase` → 建 `DescriptorPool` 与 `DynamicMessageFactory`。运行期不再读盘，**修改 .proto 后需重启进程**（codec vtable 在监听器启动时解析一次，插件不可热卸载）。

### 完整 app.yaml 示例

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: protocol.protobuf.game
      package: protocol.protobuf
      required: true
      config:
        descriptor_set: "conf/game.pb"
  bindings:
    protocol.protobuf: protocol.protobuf.game

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
            action: decode
            lazy_decode: false
          - id: 1002
            name: login
            request_schema: auth.LoginRequest   # 类型名与路由名不一致时显式覆盖
            action: decode
            lazy_decode: false
```

`body.codec` 必须与实例 capability 匹配（`protobuf`）；`provider` 是 binding 逻辑名，不是实例 ID。路由的 schema 类型名按**同名约定**（路由名 = message 全名）解析，例外用 `request_schema` 显式覆盖——这是唯一的逃生门，没有 id 映射表。

## 接口契约

实现 [`include/shield/plugin/protocol_codec.h`](https://github.com/cuihairu/shield/blob/main/include/shield/plugin/protocol_codec.h) 定义的 `shield_protocol_codec_v1`，`codec_name = "protobuf"`。寻址单级：host 把路由解析成最终类型名（`RouteEntry.schema_name`），以 `route_name` 传入，插件 `DescriptorPool::FindMessageTypeByName` 直查。

- **decode**: 按 `route_name` 找 descriptor → `DynamicMessage` → `ParseFromArray`（payload 超过 int 上限拒绝）→ `MessageToJsonString` 产出 canonical JSON 交 host。
- **encode**: 按 `route_name` 找 descriptor → `JsonStringToMessage` → `SerializeToString` 产出出站字节。

**字段名映射**：decode 输出的 JSON 字段名是 protobuf JSON 映射的 **lowerCamelCase**（`user_id` → `userId`）；encode 时 `JsonStringToMessage` 两者都接受（lowerCamelCase 或 `.proto` 原名），但 Lua handler 里读到的字段以 lowerCamelCase 为准。

## 使用示例

客户端（任意语言）用 protoc 生成的桩序列化请求；服务端 Lua handler 拿到的是 descriptor 驱动的 JSON table：

```lua
-- routes: { id = 1001, name = "auth.LoginRequest", action = "decode" }
function handler(ctx, client, request)
    -- request.userId（lowerCamelCase：proto 字段 user_id 经 protobuf JSON 映射）
    return { ok = true, userId = request.userId }
end
```

req/resp 类型名不同的场景不需要 ABI 扩展：一条 c2s 路由 + 一条 s2c 路由各自声明，出站 encode 按目标 s2c 路由自己的 schema 类型名寻址（同 route_id 回包会被出站 direction 校验拒绝）。

## 错误处理

| 错误码 | 触发条件 | 传播 |
|--------|----------|------|
| `protocol.schema_not_found` | `route_name` 在 descriptor 池中找不到对应 message（decode/encode 都会查） | 同 decode/encode 失败；启动期路由表校验也会暴露 |
| `protocol.decode_failed` | payload 解析失败 / JSON 转换失败（c2s） | session `decode_error` → **断连** |
| `protocol.encode_failed` | JSON 转 message 失败 / 序列化失败（s2c） | 丢弃该条出站消息，不断连 |
| `plugin.config.invalid`（create） | `descriptor_set` 缺失/非字符串/打不开/`FileDescriptorSet` 解析失败 | bootstrap 启动期失败 |
| `protocol.codec_unavailable` | provider 未配置、实例未启动或 codec 名不匹配 | listener 启动期构建失败 |

## 已知边界

- 只支持 binary protobuf payload，不做 gRPC、不做服务方法 RPC 语义。
- Lua egress 不自动调用插件 encode：入站 decode 的结果直接成为 request table；业务侧主动出站仍需显式编码后经 `shield.client_rpc`（见 [Protocol Codec Plugins](/protocol-codec-plugins) Known Limitations）。
- body-route 协议（`route_key`）不能配外部 provider，构建期报错；面向 header 路由或单路由 profile。
- codec 按监听器锁定，不支持 per-route / per-session 协商；描述符集运行期只读，改 .proto 需重启。

## 相关链接

- [Protocol Codec Plugins](/protocol-codec-plugins) — codec 插件化边界、schema 寻址收敛（`request_schema` 口径）
- [Protocol Routing Design](/protocol-routing-design) — 路由表、envelope、routing source
- [protobuf 官方文档](https://protobuf.dev/programming-guides/proto3/) — proto3 语言指南
- [protobuf JSON 映射](https://protobuf.dev/programming-guides/json/) — lowerCamelCase 字段名规范
- [插件系统](/plugin-system) — manifest、bootstrap pipeline、binding 解析
