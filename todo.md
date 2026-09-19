# TODO

## Schema 寻址收敛（已设计待实施）

背景与设计理由见 `docs/protocol-codec-plugins.md` 的「Schema 寻址收敛」一节。
一句话：**寻址决策上收到 host 编译期，插件降为纯「名字 → 类型」单级查找；
ABI 只暴露 route_name，不暴露 host 内部路由概念。**

收敛后唯一规则：

```text
插件收到的类型名 = request_schema（显式覆盖，非空时）
                 ∨ route name（同名约定，默认）
```

- [x] ABI（`include/shield/plugin/protocol_codec.h`）：`decode_args_v1` /
      `encode_args_v1` 删除 `route_id` / `codec_id` / `schema_id`，只留
      `route_name` + payload/message 字段；注释写明 route_name 语义 =
      host 解析好的最终 schema 类型名
- [x] `RpcDescriptor`：删 `response_schema` 死字段；`request_schema`
      语义改为「显式 schema 类型名覆盖；空 = 同名约定」
- [x] `RouteEntry`：删 `schema_id`；新增 `schema_name`（`request_schema`
      非空取之，否则取 `debug_name`）
- [x] `DecodedBody`：删 `schema_id`；`codec_id` 无消费点则一并删
- [x] `ExternalBodyCodec::decode/encode`：`args.route_name` 改用
      `route.schema_name`；核实出站（response table key）路径统一走
      `schema_name`
- [x] xmldef catalog：删 `schema_id` / `schema` attr 解析段
      （连同 `RouteEntry.codec_id` 死字段与 `XmldefCatalogOptions.default_codec_id` 一并删除——
      `codec_for_route` 有意不用 route.codec_id，管线绑定单一 codec）
- [x] `config.cpp`：routes 键白名单删 `response_schema`；`schema_id` /
      `response_schema` 已删键在 YAML 与 JSON 两条解析路径均「出现即报错」
      （pre-1.0 不做静默兼容读，先例：`network.protocol.routes`）
- [x] `protocol.protobuf` / `protocol.flatbuffers`：删 `schema_names` /
      `route_names` 两张映射与 `messages` 配置解析，resolve 收为单行
      按名查找；manifest `config_schema` 删 `messages`（fbs 的映射本是
      死代码：decode/encode 从未调用 resolve）
- [x] `protocol.msgpack`：跟随新 ABI 签名（零字段引用，无需改动）
- [x] 测试：fake codec 跟随新 ABI；schema_id 用例改写为
      「同名约定默认」+「request_schema 覆盖优先」两条正路用例；
      config 新增已删键报错用例（YAML/JSON 双路径）
- [x] docs：`protocol-routing-design.md` 字段表已更新；
      `protocol-codec-plugins.md` Implementation Order 第 5 条已改写为
      收敛后口径

验收：全量重编 + 串行测试（build-plugins 与主树）+ clang-format，
四家协议插件编译/测试全绿。

## Phase 2 候选（另行立项，不与上刀混合）

- [ ] 方向不对称 schema：req/resp 不同类型名的表达。需要 ABI 扩展
      （encode 侧独立的 response schema 寻址）；现状 s2c 独立路由声明
      已覆盖主要场景，非急迫
- [ ] 无 schema codec（json/msgpack）的可选 schema 校验插件：Lua 边界
      严格性（`user_id = 123` vs `userid = "123"` 目前 runtime 才暴露）
- [ ] 寻址软收敛完成后的下一步：评估 `request_codec` per-route 覆盖的
      实际使用率，决定是否收敛为 profile 级唯一
- [ ] listener bind 失败清理路径的跨平台崩溃：`DuplicateListenerPortFails`
      在 macOS 必崩（已 `#ifndef __APPLE__` 禁用，见用例注释 TODO）、
      Windows CI 偶发段错误（22847bb 主 CI 首现；ac48657 主 CI 再现，
      rerun 通过——flaky 性质已判定，非稳定回归）；
      `cleanup_failed_initialize` 拆除首个 listener 的路径存在竞态/悬垂，
      需要在真平台上定位根因而不是继续扩排除名单。
      线索（ac48657 失败日志，panic forensics 抓到）：崩溃前有
      `[C]: in global 'error'` 携带 nil 错误对象触发裸 panic
      （"non-string error object (type=nil)"）——bind 失败的错误传播
      链上存在 error(nil) 调用且无 pcall 保护，可能与拆除路径的悬垂
      叠加；排查时先找 error(nil) 的调用点
- [ ] xmldef 工具链/文档适配：xmldef descriptor 的 `schema_id` 是其
      descriptor 系统内部概念，与 codec ABI 无关；xmldef 实施时 catalog
      导出的路由需适配收敛后的 ABI（只产出 `schema_name`）
