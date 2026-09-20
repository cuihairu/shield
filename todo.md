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
- [x] 寻址软收敛完成后的下一步：`request_codec` per-route 覆盖评估
      已完成（2026-09-20），结论**保留**：全仓库唯一消费点是
      lua_service.cpp 客户端 RPC dispatch 的 json/raw 启发式——无
      codec 插件产出 `decoded_request` 时，空或 "json" 由目标 VM 尝试
      JSON 解码（失败回退原始字节字符串），其他值原样传字节。它是
      无 codec 插件 profile 下同监听器混合 JSON/二进制路由的唯一
      控制点，删除即破坏真实部署形态（gateway 测试的 raw 路由是
      语义而非测试杠杆）。已修正误导表述：字段注释与
      protocol-routing-design.md 字段表原先称 "per-route codec 覆盖 /
      空 = profile 默认 codec"，收敛后 codec 绑定恒为 profile 级单一、
      本字段从不参与 codec 插件选择，已改写为"解码提示，非 codec
      选择"的准确口径。
- [x] listener bind 失败清理路径的跨平台崩溃已根因修复（2026-09-19，
      Linux ASan 现场取证，非猜测）：不是单一竞态，而是 bootstrap
      拆除顺序的**三类悬垂**——macOS/Windows 分配器不复用 freed
      chunk 故必炸，Linux 靠 chunk 复用侥幸存活（推断，解释为何
      coverage 长期绿）——
      (a) console 命令对象悬垂 `this`：`RootCommands`/`LuaCommands`
      是 initialize 局部 shared_ptr，注册进 dispatcher 的 lambda 捕
      裸 `this`，initialize 一返回即悬垂（ASan：FullStack 里
      `cmd_help` 读已释放的 RootCommands；Linux 上疑似 LuaCommands
      复用同 chunk 且字段同布局，帮助命令"照常工作"实为读错对象）
      → 两者移入 GlobalState；
      (b) console dispatcher 先于 net 线程 join 销毁：shutdown 顶部
      reset dispatcher 时 net 线程还可能在跑 console read 完成回调
      （ASan 实证 T9 正在 dispatch、T0 已 free）→ dispatcher 与命令
      对象改到 net 线程 join 之后销毁（cleanup_failed_initialize 本
      就先 join，顺序不动）；
      (c) initialize 失败点在监听器循环**内部**调
      cleanup_failed_initialize：`g_state_owner.reset()` 销毁
      GlobalState 之后 `return false` 的栈展开才析构循环局部（失败的
      listener/bridge/callbacks），其捕获仍指 GlobalState 内存
      （ASan：~TcpListener 读 freed `vector<uint8_t>`）→ 主体改
      `initialize_impl`，cleanup 由 wrapper 统一延迟到栈展开完成后；
      附带：gateway actor `anon_send_exit` 异步退出与 `lua_services`
      销毁竞态（GatewayDeps.manager 裸指针）→ 新增
      `exit_gateway_actors_and_wait`（monitor + down_msg + 5s 兜底，
      惯用法同 lua_service `wait_for_actors_until`），shutdown 与
      cleanup 共用。
      验证：ASan 树（build-asan）复现 → 修复后 test_cov_bootstrap
      全量与 test_cov_lua_http_bridge 零报错；三个用例已摘除
      `#ifndef __APPLE__`（DuplicateListenerPortFails /
      FullStackInitializeAndShutdown / HttpPortBindFailureIsNonFatal），
      CI 三平台真平台复核通过（2026-09-19，57dea31：macOS/Windows/
      ubuntu job 全绿，三个用例在 macOS 首次真实运行无崩溃）；
      http_bridge 的 0x40/0x9 近空指针家族在 Linux ASan 下未复现，
      暂无证据指向同类，先不动。
      线索勘误存档（2026-09-19 重读 ac48657 attempt-1 完整日志）：
      该次 Windows job 实为两个独立失败——smoke 的 nil panic（requeue
      spin cap 触顶 → 旧 throw 式 panic handler 的 sol::error 逃逸
      actor → on_init 超时；c2ce720 后将以 abort+forensics 确定性
      暴露）与 08:48 `DuplicateListenerPortFails` 段错误，与本条目
      (a)(b)(c) 同根；"`[C]: in global 'error'` 携带 nil" 系误读
      （doomed/flaky 用例故意在 main chunk 调 error 的良性加载失败
      日志，错误对象是字符串）。
- [x] shield.sleep 续延（lua_api.cpp `_resume_after` resume_fn）的终态
      错误分支已与其它 resume 路径对齐（2026-09-20）：错误臂补齐
      `lua_settop(co,0)`（错误对象不再滞留协程栈）、
      `on_handler_failed`（sleep 后 error 的在途 call 从"挂到超时"变
      为立即收到失败）、`invoke_error_hook`（error_type 取 "sleep"、
      method_label 空，对齐 ("fork","") 先例；on_error 钩子 + 连续
      错误阈值 panic 计数自此覆盖 sleep 路径）。顺序镜像
      invoke_coroutine 终态错误臂；文档注释同步。
      集成用例：SleepContinuationErrorRoutesFailureAndHook
      （string error 上游收到 boom 消息 + on_error 收到
      {type="sleep"}；table error 走默认消息 "sleep continuation
      error"；无 call 会话的 plain handler 只计数不上报）+
      SleepContinuationErrorsCountTowardPanic（10 次连续 sleep 错误
      触发 panic 退出，与 handler 路径阈值语义一致）。
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
