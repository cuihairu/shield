# protocol.flatbuffers

> FlatBuffers 编解码插件：`.fbs` 文本 schema 驱动，二进制 ↔ JSON 桥接，flatc 工具链生态直通。

`protocol.flatbuffers` 是官方提供的 [`shield.protocol.codec.v1`](/protocol-codec-plugins) 之一，codec 名为 `flatbuffers`。create 时从磁盘加载 `.fbs` 文本 schema；decode 把 FlatBuffers 二进制转成 JSON（`GenerateText`），encode 把 JSON 按 schema 解析成 FlatBuffers 二进制。需要通过 `body.provider` 绑定本插件启用。

**选型速览**：价值在 flatc 工具链融合（`.fbs` 直接进 FlatBuffers 生态）与单 schema 强类型 encode；与 [protocol.protobuf](/plugins/protocol-protobuf) 相比没有 descriptor 预编译（本实现每次调用重新解析 schema 文本、经 JSON 桥接，非零拷贝路径），大流量场景先压测吞吐；只想快速起步选 [protocol.json](/plugins/protocol-json) / [protocol.msgpack](/plugins/protocol-msgpack)。

## 包信息

- **包 ID**: `protocol.flatbuffers`
- **接口**: [`shield.protocol.codec.v1`](/protocol-codec-plugins)（capabilities: `flatbuffers`, `fbs`）
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_PLUGIN_FLATBUFFERS`
- **源码**: `plugins/protocol.flatbuffers/`
- **依赖**: [FlatBuffers](https://flatbuffers.dev/)（vcpkg 端口 `flatbuffers`，`flatbuffers::flatbuffers` CMake target）+ nlohmann/json

## 构建启用

在 CMake 配置阶段打开 `SHIELD_BUILD_PLUGIN_FLATBUFFERS`：

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_FLATBUFFERS=ON
cmake --build build
```

该选项触发两件事：

1. `plugins/protocol.flatbuffers/` 下的 shared library 被构建，输出到 `plugins/protocol.flatbuffers/bin/`。
2. CMake 把 vcpkg manifest feature `protocol-flatbuffers` 透传给 vcpkg，自动安装 `flatbuffers` 端口。

CI 的 plugins-ci 工作流以 `SHIELD_BUILD_PLUGIN_FLATBUFFERS=ON` 构建。

## 配置 Schema

实例配置共两个键，**至少设一个**，都缺则 create 失败（`plugin.config.invalid`）：

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `schema` | string | 二选一 | — | `.fbs` schema 文件路径（如 `conf/game.fbs`）。多文件/含 `include` 的工程建议逐文件显式指定或合并后传入。 |
| `schema_dir` | string | 二选一 | — | 含 `.fbs` 文件的目录。create 时扫描目录取**枚举到的第一个** `.fbs` 文件——目录枚举顺序不做保证，目录里有多个 `.fbs` 时结果不确定，单 schema 文件的目录才推荐用这个键。 |

行为细节（与性能相关，如实说明）：

- schema 文本在 **create 时**读入缓存（文件打不开 / 目录不存在 / 目录无 `.fbs` → create 失败）。
- 但每次 decode/encode 都会**重新 `Parser::Parse`** 一遍 schema 文本——schema 很大且流量高时这是显著开销，压测时把 schema 解析计入成本。
- 本插件**没有** protocol.json/msgpack 那套 per-route payload schema 校验（`schemas` / `require_schema` / `on_violation` 均不存在）；约束来自 `.fbs` 本身：encode 的 JSON 不符合 schema 会解析失败。

### 完整 app.yaml 示例

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: protocol.flatbuffers.game
      package: protocol.flatbuffers
      required: true
      config:
        schema: "conf/game.fbs"
  bindings:
    protocol.flatbuffers: protocol.flatbuffers.game

actors:
  - name: gateway
    script: scripts/gateway.lua
    network:
      tcp: "0.0.0.0:8001"
      protocol:
        name: game.flatbuffers
        envelope:
          type: idlen
          route_id_bytes: 4
          length_bytes: 4
          endian: big
        body:
          codec: flatbuffers
          provider: protocol.flatbuffers
        routing:
          source: header.route_id
          unknown_route_action: drop
        routes:
          - id: 1001
            name: auth.LoginRequest
            action: decode
            lazy_decode: false
```

`body.codec` 必须与实例 capability 匹配（`flatbuffers`）；`provider` 是 binding 逻辑名，不是实例 ID。

## 接口契约

实现 [`include/shield/plugin/protocol_codec.h`](https://github.com/cuihairu/shield/blob/main/include/shield/plugin/protocol_codec.h) 定义的 `shield_protocol_codec_v1`，`codec_name = "flatbuffers"`。

- **decode**: 空 payload 直接返回 `"{}"`；否则用缓存 schema 文本新建 `Parser` 并 `Parse`，再 `GenerateText` 把二进制转成 JSON 文本交付 host。
- **encode**: 把 canonical JSON message 输入先用 `Parser::Parse` 按 schema 解析（这一步同时完成 schema 校验——JSON 与 `.fbs` 定义不符即失败），再从 builder 取 buffer 作为出站字节。

`.fbs` 中定义的 root type 决定 encode 的目标 message；多 message 的 `.fbs` 里，路由到 message 的映射仍由 host 的 `route_name`（schema 类型名）单级寻址约定承载。

## 使用示例

客户端用 flatc 生成代码序列化请求；服务端 Lua handler 拿到的是 JSON 桥接后的 table：

```lua
-- routes: { id = 1001, name = "auth.LoginRequest", action = "decode" }
function handler(ctx, client, request)
    -- request 由 GenerateText 生成，字段名与 .fbs 声明一致
    return { ok = true, userid = request.userid }
end
```

调试期可以先用 `flatc --json` 验证 `.fbs` schema 能被解析（插件 create 用的是同一个解析器，schema 语法错误会在 create 期暴露为 `failed to parse FlatBuffers schema`）。

## 错误处理

| 错误码 | 触发条件 | 传播 |
|--------|----------|------|
| `protocol.decode_failed` | schema 文本解析失败 / 二进制转 JSON 失败（c2s） | session `decode_error` → **断连** |
| `protocol.encode_failed` | schema 解析失败 / JSON 不符合 schema / buffer 获取失败（s2c） | 丢弃该条出站消息，不断连 |
| `plugin.config.invalid`（create） | `schema` 与 `schema_dir` 都缺、文件打不开、目录不存在、目录无 `.fbs` | bootstrap 启动期失败 |
| `protocol.codec_unavailable` | provider 未配置、实例未启动或 codec 名不匹配 | listener 启动期构建失败 |

## 已知边界

- JSON 桥接实现：decode/encode 都经 JSON 文本中转，且每次调用重解析 schema——零拷贝是 FlatBuffers 库本身的能力，本插件的桥接路径不享受它（见 [Protocol Codec Plugins](/protocol-codec-plugins) Known Limitations）。
- `schema_dir` 的多文件目录语义不确定（取枚举到的第一个 `.fbs`），生产配置用 `schema` 显式指文件。
- Lua egress 不自动调用插件 encode；body-route 协议（`route_key`）不能配外部 provider。
- codec 按监听器锁定，不支持 per-route / per-session 协商。

## 相关链接

- [Protocol Codec Plugins](/protocol-codec-plugins) — codec 插件化边界、ABI 形状、Known Limitations
- [FlatBuffers 官方文档](https://flatbuffers.dev/flatbuffers_guide_writing_schema.html) — `.fbs` schema 语法
- [插件系统](/plugin-system) — manifest、bootstrap pipeline、binding 解析
