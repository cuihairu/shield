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
      线索勘误（2026-09-19 重读 ac48657 attempt-1 完整日志，此前归因有误）：
      该次 Windows job 实为**两个独立失败**——
      (a) Test #9 `shield_runtime_lua_smoke`（exit 1，10.08s）：smoke_root
      的 on_init 10s spawn 超时。stderr 序列：session=3 requeue spin
      n=3..20 全 ok=1（无 resume_diag → cap 触顶后 fall-through resume
      未被拒）→ panic ctx `state=coroutine depth=0 []` → panic detail
      "non-string error object (type=nil)" → CAF 报 `user.scheduled-actor`
      unhandled exception（即服务 actor；双层 "lua: error:" 前缀 =
      旧 handler 拼一层 + sol::error 构造函数自动加一层，是**单次
      panic** 非二次触发）。工作假设：nil panic 使旧 handler 的
      sol::error 从 actor 消息处理中逃逸 → actor 死亡 → 挂起的 call
      永无完成 → on_init 超时。ctx depth=0 与 detail type=nil 在同一
      handler 内自相矛盾（top=0 时 lua_type(-1)=TNONE 应报 "no
      value"；5.5 reset 后 at_panic 恒可见错误对象）→ 疑为同线程两个
      panic 事件的 stderr 交错，或存在空栈进 at_panic 的未知路径。
      **panic 改造（c2ce720）后此形态若复现将以 abort + forensics
      确定性暴露**，届时以 `*** shield lua panic` 输出为准重启排查。
      (b) 08:48 的 `DuplicateListenerPortFails` memory access violation
      （write to 0x20f1b196cd8）——独立段错误，与 (a) 的 nil panic
      无关，本条目真正的根因目标仍是它。
      原线索中 "`[C]: in global 'error'` 携带 nil 触发裸 panic" 系
      误读：那些 traceback 来自 doomed/flaky 用例**故意**在 main chunk
      调 error('load time boom') 的良性加载失败日志（错误对象是
      字符串），与 panic 无关。
- [ ] shield.sleep 续延（lua_api.cpp `_resume_after` resume_fn）的终态
      错误分支与其它 resume 路径不对称：缺 `lua_settop(co,0)` 清理
      （错误对象滞留协程栈至 GC）、不走 error hook/on_handler_failed。
      补齐是行为变更（on_handler_failed 开始对 sleep 路径触发，含
      错误阈值 panic 计数），需独立设计 error_type/method_label 取值
      并补 service handler sleep 后 error 的集成用例
- [x] `load_script`（lua_runtime.cpp）的 sol `script_file` throw 式 API
      已消除（2026-09-19）：c2ce720 的 panic-abort 语义让它从"清理项"
      变成承重 bug——NDEBUG 下 script_file 退化为 luaL_dofile，加载
      失败直达 at_panic（原 throw 设计靠 catch(sol::error) 兜住，abort
      设计下直接 SIGABRT），三平台 Release 矩阵的 LoadScriptFile /
      LoadFailures / LoadScriptOnDirectoryFails 全灭。已改为
      luaL_loadfile + lua_pcall 保护式（失败返回 false，永不 raise），
      并补了执行期 error 分支用例。
- [ ] xmldef 工具链/文档适配：xmldef descriptor 的 `schema_id` 是其
      descriptor 系统内部概念，与 codec ABI 无关；xmldef 实施时 catalog
      导出的路由需适配收敛后的 ABI（只产出 `schema_name`）
