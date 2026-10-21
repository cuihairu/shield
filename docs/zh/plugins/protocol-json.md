# protocol.json

> 内置 JSON codec 的可选校验替身：不启用时内置 json 照旧生效，启用后获得 per-route payload schema 校验。

`protocol.json` 是官方提供的 [`shield.protocol.codec.v1`](/plugin-system#interface-model) 之一，codec 名为 `json`。**不设置 `body.provider` 时核心内置 json 照旧生效，行为零变化**；显式把 provider 绑到本插件后，解码/编码语义与内置完全一致（含可选的顶层 `{"payload": ...}` 信封解包），额外获得在 ingress/egress 边界按路由校验 payload 形状的能力。

**选型速览**：JSON 可读性最好、排障零成本、无额外协议运行时——开发期与配置型协议首选；要二进制体积选 [protocol.msgpack](/plugins/protocol-msgpack)，要跨语言强类型 schema 选 [protocol.protobuf](/plugins/protocol-protobuf)，要 flatc 工具链融合选 [protocol.flatbuffers](/plugins/protocol-flatbuffers)。

## 包信息

- **包 ID**: `protocol.json`
- **接口**: [`shield.protocol.codec.v1`](/protocol-codec-plugins)（capabilities: `json`）
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_PLUGIN_JSON`
- **源码**: `plugins/protocol.json/`
- **依赖**: [nlohmann/json](https://github.com/nlohmann/json)（header-only，随核心已在 vcpkg manifest 中，无独立运行时库）

## 构建启用

在 CMake 配置阶段打开 `SHIELD_BUILD_PLUGIN_JSON`：

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_JSON=ON
cmake --build build
```

产物输出到 `plugins/protocol.json/bin/libshield_protocol_json.{so|dylib|dll}`。该插件不引入新的第三方端口（编解码复用核心已装的 nlohmann/json）。CI 的 plugins-ci 工作流以 `SHIELD_BUILD_PLUGIN_JSON=ON` 构建。

## 配置 Schema

实例配置（`plugins.instances[].config`）共四个键，全部可选：

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `schemas` | object | 否 | — | 内联 schema 表；键 = host 解析好的 schema 类型名（`RouteEntry.schema_name`，即路由名或该路由的 `request_schema` 覆盖），值 = JSON-Schema 子集 schema。**与 `schemas_file` 互斥**，两者都设则 create 失败。 |
| `schemas_file` | string | 否 | — | 从文件读取 schema 表（create 时读盘解析，须为 JSON object，每个 value 也须是 object）。与 `schemas` 互斥。 |
| `require_schema` | boolean | 否 | `false` | `true` 时未配置 schema 的路由直接失败 `protocol.schema_not_found`；默认放行（无 schema = 无约束）。 |
| `on_violation` | string | 否 | `reject` | `reject` 校验失败即拒绝；`warn` 记 host WARN 日志后放行（观察模式）。取值仅 `reject`/`warn`，其他值 create 失败。 |

schema 校验复用 host 的最小 JSON-Schema 子集校验器（关键字集、`integer` 的 token 级语义、`additionalProperties` 仅布尔 false 强制等口径见 [Protocol Codec Plugins](/protocol-codec-plugins)），插件不自带第二套实现。schema 形状错误以 `schema evaluation error` 暴露为 decode/encode 失败，不会逃逸 C ABI。

### 完整 app.yaml 示例

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: protocol.json.gateway
      package: protocol.json
      required: true
      config:
        schemas:
          auth.LoginRequest:
            type: object
            required: [userid]
            properties:
              userid: { type: string, minLength: 1 }
            additionalProperties: false
        on_violation: reject
  bindings:
    protocol.json: protocol.json.gateway   # binding 逻辑名 -> 实例 ID

actors:
  - name: gateway
    script: scripts/gateway.lua
    network:
      tcp: "0.0.0.0:8001"
      protocol:
        name: game.json
        envelope:
          type: idlen
          route_id_bytes: 4
          length_bytes: 4
          endian: big
        body:
          codec: json
          provider: protocol.json          # 引用上面的 binding 逻辑名
        routing:
          source: header.route_id
          unknown_route_action: drop
        routes:
          - id: 1001
            name: auth.LoginRequest
            action: decode
            lazy_decode: false
```

`body.codec` 必须与实例 capability 匹配（`json`）；`provider` 是 binding 逻辑名，不是实例 ID——替换 provider 指向的实例无需改 actor 配置。

## 接口契约

实现 [`include/shield/plugin/protocol_codec.h`](https://github.com/cuihairu/shield/blob/main/include/shield/plugin/protocol_codec.h) 定义的 `shield_protocol_codec_v1`，`codec_name = "json"`。decode/encode 都是单级名字查找：host 编译期把路由解析成最终 schema 类型名（`request_schema` 覆盖或路由名本身），以 `route_name` 传入。

### decode（c2s）

1. `nlohmann::json::parse` 解析 payload 字节（空区间/不可解析字节失败）。
2. 解包可选的顶层 `{"payload": ...}` 信封——**schema 校验的是解包后的业务消息**（与内置 json codec 同语义，交付给 Lua 的形状即 schema 描述的形状）。
3. 有 schema 则校验；`reject` 违规返回 `protocol.decode_failed`，`warn` 违规记 WARN 后放行。
4. 通过后把业务消息 dump 成 canonical JSON，交 host 作为 Lua handler 的 request table。

### encode（s2c）

1. 解析 canonical JSON message 输入。
2. 按 `route_name` 查 schema 校验（同上，无信封包装）。
3. 通过后 dump 成 JSON 字节作为出站 payload。

## 使用示例

schema 违规的 c2s（reject）会**断连**（沿用 decode 失败语义，`error_code_ = "decode_error"`）；s2c（encode）违规仅丢弃该条出站消息。上线收紧前先用观察模式：

```yaml
config:
  schemas_file: "conf/payload-schemas.json"
  require_schema: false      # 未声明 schema 的路由照常放行
  on_violation: warn         # 只记日志观察线上违规，再收紧为 reject
```

违规明细（含首个错误点的路径，如 `userid: ...`）走 host 日志，前缀 `protocol.json`。

Lua handler 收到的 `request` 已是解码并校验后的业务消息，无需再处理信封：

```lua
-- routes: { id = 1001, name = "auth.LoginRequest", action = "decode" }
function handler(ctx, client, request)
    -- request = { userid = "..." }（schema 保证存在且非空）
    return { ok = true, userid = request.userid }
end
```

## 错误处理

| 错误码 | 触发条件 | 传播 |
|--------|----------|------|
| `protocol.schema_not_found` | `require_schema: true` 且路由未配置 schema | 同 decode/encode 失败 |
| `protocol.decode_failed` | payload 不可解析 / reject 违规（c2s） | session `decode_error` → **断连** |
| `protocol.encode_failed` | message 不可解析 / reject 违规（s2c） | 丢弃该条出站消息，不断连 |
| `protocol.codec_unavailable` | provider 未配置、实例未启动或 codec 名不匹配 | listener 启动期构建失败 |

实例启动期（create）的配置形状错误（互斥键、类型错误、`schemas_file` 打不开/非 JSON object）报 `plugin.create.failed`，bootstrap 即失败。

## 已知边界

- Lua egress 不自动调用插件 encode：入站 decode 的结果直接成为 request table；业务侧主动出站仍需显式编码后经 `shield.client_rpc`（见 [Known Limitations](/protocol-codec-plugins)）。
- body 路由键提取（`route_key`）不能配外部 provider：body-route 协议配 `protocol.json` 在构建期报错；校验插件面向 header 路由（idlen/typelen）或单路由 profile。
- codec 按监听器锁定（`actors[].network.protocol.body`），不支持 per-route / per-session 协商。

## 相关链接

- [Protocol Codec Plugins](/protocol-codec-plugins) — codec 插件化边界、schema 寻址收敛、校验器关键字子集
- [Protocol Routing Design](/protocol-routing-design) — 路由表、envelope、routing source
- [插件系统](/plugin-system) — manifest、bootstrap pipeline、binding 解析
- [Lua API](/lua-api) — handler 绑定与 `shield.client_rpc` 出站约定
