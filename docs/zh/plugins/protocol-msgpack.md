# protocol.msgpack

> MessagePack 二进制编解码，与 protocol.json 同一套可选 per-route payload schema 校验，体积约为 JSON 的一半。

`protocol.msgpack` 是官方提供的 [`shield.protocol.codec.v1`](/plugin-system#interface-model) 之一，codec 名为 `msgpack`。核心不再内置 msgpack codec——**必须**通过 `body.provider` 绑定本插件启用。decode 用 `from_msgpack`、encode 用 `to_msgpack`（nlohmann/json 内置 MessagePack 实现，无外部 msgpack-cxx 运行时依赖），并可在 ingress/egress 边界按路由校验 payload 形状。

**选型速览**：与 [protocol.json](/plugins/protocol-json) 语义同构但 wire 格式是二进制，适合对包体/带宽敏感、又不想引入 schema 编译器的场景；需要跨语言强类型 schema 演进选 [protocol.protobuf](/plugins/protocol-protobuf)，需要 flatc 工具链融合选 [protocol.flatbuffers](/plugins/protocol-flatbuffers)。

## 包信息

- **包 ID**: `protocol.msgpack`
- **接口**: [`shield.protocol.codec.v1`](/protocol-codec-plugins)（capabilities: `msgpack`）
- **版本**: 1.1.0（schema 校验能力自 v1.1.0 起）
- **CMake 选项**: `SHIELD_BUILD_PLUGIN_MSGPACK`
- **源码**: `plugins/protocol.msgpack/`
- **依赖**: [nlohmann/json](https://github.com/nlohmann/json)（header-only；MessagePack 编解码是其内置能力，插件不链接 msgpack-cxx。vcpkg manifest feature `protocol-msgpack` 仍会透传安装 `msgpack` 端口，属构建期行为，非运行时依赖）

## 构建启用

在 CMake 配置阶段打开 `SHIELD_BUILD_PLUGIN_MSGPACK`：

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_MSGPACK=ON
cmake --build build
```

该选项触发两件事：

1. `plugins/protocol.msgpack/` 下的 shared library 被构建，输出到 `plugins/protocol.msgpack/bin/`。
2. CMake 把 vcpkg manifest feature `protocol-msgpack` 透传给 vcpkg。

CI 的 ci.yml 与 plugins-ci 工作流均以 `SHIELD_BUILD_PLUGIN_MSGPACK=ON` 构建（`test_protocol_msgpack_plugin`）。

## 配置 Schema

实例配置共四个键，全部可选，与 [protocol.json](/plugins/protocol-json) 完全同构：

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `schemas` | object | 否 | — | 内联 schema 表；键 = host 解析好的 schema 类型名（`RouteEntry.schema_name`），值 = JSON-Schema 子集 schema。**与 `schemas_file` 互斥**，两者都设则 create 失败。 |
| `schemas_file` | string | 否 | — | 从文件读取 schema 表（create 时读盘解析，须为 JSON object）。与 `schemas` 互斥。 |
| `require_schema` | boolean | 否 | `false` | `true` 时未配置 schema 的路由直接失败 `protocol.schema_not_found`；默认放行。 |
| `on_violation` | string | 否 | `reject` | `reject` 校验失败即拒绝；`warn` 记 host WARN 日志后放行。取值仅 `reject`/`warn`。 |

校验时机：decode 在 `from_msgpack` 之后、encode 在 `to_msgpack` 之前。校验器复用 host 的最小 JSON-Schema 子集校验器（口径见 [Protocol Codec Plugins](/protocol-codec-plugins)）。

**注意与 json 的差异**：msgpack 没有 `{"payload": ...}` 信封解包——schema 校验和交付给 Lua 的都是 `from_msgpack` 的整个结果。

### 完整 app.yaml 示例

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: protocol.msgpack.gateway
      package: protocol.msgpack
      required: true
      config:
        schemas:
          auth.LoginRequest:
            type: object
            required: [userid]
            properties:
              userid: { type: string, minLength: 1 }
            additionalProperties: false
        require_schema: false
        on_violation: reject
  bindings:
    protocol.msgpack: protocol.msgpack.gateway

actors:
  - name: gateway
    script: scripts/gateway.lua
    network:
      tcp: "0.0.0.0:8001"
      protocol:
        name: game.msgpack
        envelope:
          type: idlen
          route_id_bytes: 4
          length_bytes: 4
          endian: big
        body:
          codec: msgpack
          provider: protocol.msgpack
        routing:
          source: header.route_id
          unknown_route_action: drop
        routes:
          - id: 1001
            name: auth.LoginRequest
            action: decode
            lazy_decode: false
```

`body.codec` 必须与实例 capability 匹配（`msgpack`）；`provider` 是 binding 逻辑名，不是实例 ID。

## 接口契约

实现 [`include/shield/plugin/protocol_codec.h`](https://github.com/cuihairu/shield/blob/main/include/shield/plugin/protocol_codec.h) 定义的 `shield_protocol_codec_v1`，`codec_name = "msgpack"`。寻址与 json 同构：host 把路由解析成最终 schema 类型名，以 `route_name` 传入，插件单级查找。

- **decode**: `from_msgpack` 解码 payload 字节（空/损坏字节失败）→ 可选 schema 校验 → dump 成 canonical JSON 交 host。
- **encode**: 解析 canonical JSON message → 可选 schema 校验 → `to_msgpack` 产出字节。

## 使用示例

与 json 同一套收紧节奏：先观察再拒绝。

```yaml
config:
  schemas_file: "conf/payload-schemas.json"
  on_violation: warn   # 观察模式：违规只记 WARN（前缀 protocol.msgpack），照常放行
```

Lua handler 拿到的 `request` 即解码后的整条消息（无信封）：

```lua
-- routes: { id = 1001, name = "auth.LoginRequest", action = "decode" }
function handler(ctx, client, request)
    return { ok = true, userid = request.userid }
end
```

## 错误处理

| 错误码 | 触发条件 | 传播 |
|--------|----------|------|
| `protocol.schema_not_found` | `require_schema: true` 且路由未配置 schema | 同 decode/encode 失败 |
| `protocol.decode_failed` | payload 非 MessagePack / reject 违规（c2s） | session `decode_error` → **断连** |
| `protocol.encode_failed` | message 不可解析 / reject 违规（s2c） | 丢弃该条出站消息，不断连 |
| `protocol.codec_unavailable` | provider 未配置、实例未启动或 codec 名不匹配 | listener 启动期构建失败 |

create 期配置形状错误（互斥键、类型错误、`schemas_file` 打不开/非 JSON object）报 `plugin.create.failed`。

## 已知边界

- 无信封解包：顶层 `{"payload": ...}` 约定仅 json 路径存在。
- Lua egress 不自动调用插件 encode（见 [Protocol Codec Plugins](/protocol-codec-plugins) Known Limitations）。
- body-route 协议（`route_key`）不能配外部 provider，构建期报错；面向 header 路由或单路由 profile。
- codec 按监听器锁定，不支持 per-route / per-session 协商。

## 相关链接

- [Protocol Codec Plugins](/protocol-codec-plugins) — codec 插件化边界、校验器关键字子集、错误映射
- [protocol.json](/plugins/protocol-json) — 同一套 schema 校验的 JSON 版
- [插件系统](/plugin-system) — manifest、bootstrap pipeline、binding 解析
