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

- [ ] ABI（`include/shield/plugin/protocol_codec.h`）：`decode_args_v1` /
      `encode_args_v1` 删除 `route_id` / `codec_id` / `schema_id`，只留
      `route_name` + payload/message 字段；注释写明 route_name 语义 =
      host 解析好的最终 schema 类型名
- [ ] `RpcDescriptor`：删 `response_schema` 死字段；`request_schema`
      语义改为「显式 schema 类型名覆盖；空 = 同名约定」
- [ ] `RouteEntry`：删 `schema_id`；新增 `schema_name`（`request_schema`
      非空取之，否则取 `debug_name`）
- [ ] `DecodedBody`：删 `schema_id`；`codec_id` 无消费点则一并删
- [ ] `ExternalBodyCodec::decode/encode`：`args.route_name` 改用
      `route.schema_name`；核实出站（response table key）路径统一走
      `schema_name`
- [ ] xmldef catalog：删 `schema_id` / `schema` attr 解析段
- [ ] `config.cpp`：routes 键白名单删 `schema_id` / `response_schema`
      （JSON routes 的 `schema_id` 从未被解析过，纯删即可）
- [ ] `protocol.protobuf` / `protocol.flatbuffers`：删 `schema_names` /
      `route_names` 两张映射与 `messages` 配置解析，resolve 收为单行
      按名查找；manifest `config_schema` 删 `messages` / `route_map`
- [ ] `protocol.msgpack`：跟随新 ABI 签名（已确认零字段引用）
- [ ] 测试：fake codec 跟随新 ABI；schema_id 用例改写为
      「同名约定默认」+「request_schema 覆盖优先」两条正路用例；
      config 新增已删键报错用例
- [ ] docs：`protocol-routing-design.md` 字段表已更新（本刀已改）；
      实施后复核两篇协议文档与代码一致——特别是
      `protocol-codec-plugins.md` Implementation Order 第 5 条
      「schema/route 映射已实现」需改写为收敛后的口径

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
- [ ] xmldef 工具链/文档适配：xmldef descriptor 的 `schema_id` 是其
      descriptor 系统内部概念，与 codec ABI 无关；xmldef 实施时 catalog
      导出的路由需适配收敛后的 ABI（只产出 `schema_name`）
