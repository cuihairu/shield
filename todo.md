# TODO

## 产品化 + 架构安全默认值（2026-09-25 本轮，进行中）

背景与差距清单见 `docs/product-gap.md`（对照 skynet 的产品化评估）与
`docs/architecture-review.md`（九维架构评估，结论服务产品化）。优先级基准：
**新用户 10 分钟跑通一个最小游戏服务端**。

- [x] 架构评估文档 `docs/architecture-review.md`：九维逐项判断（进程线程/
      网络协议/会话状态/存储持久化/定时调度/热更新/扩展点/容错监控/横向扩展），
      风险清单（严重度 + file:line），必须/过度/欠缺分析
- [x] 产品化差距文档 `docs/product-gap.md`：8 维对照 skynet，top-3 挡路项
      排序（默认配置可观测、模板工程、启动脚本与文档线性化）
- [x] P0-1 默认配置可观测：config/app.yaml 增加 echo actor
      （scripts/echo.lua，TCP 0.0.0.0:7900，idlen+json，echo id=1 →
      echo_result id=100，无认证）+ 默认配置真实启动 acceptance 测试
      （tests/acceptance/test_default_config_boot.cpp，ops 端点 hermetic
      化补丁后按原始文件启动并完成 TCP 回包闭环）
- [x] P0-2 模板工程一条命令生成：templates/minimal_game/（config+scripts+
      README，<APP_NAME> 占位符）+ scripts/new_project.sh（生成 + 自动
      --check-config 自检）+ ctest 锚定（shield_new_project_scaffold，
      POSIX 平台）
- [x] P0-3a build.sh 环境预检：cmake ≥ 3.30 / C++23 编译器探测（-std=c++23
      试编译）/ ninja / VCPKG_ROOT，缺失给一行修复指引，不再让 vcpkg 的
      二级错误（"unable to find Ninja"）背锅
- [x] P0-3b 客户端样例 scripts/client_demo.py：stdlib-only idlen+json
      客户端，打通 echo / hello_world login，quickstart 的"连接验证"步骤
- [x] P0-3c quickstart.md 重写为线性 10 分钟路径（前置矩阵 → 构建 → 启动 →
      连接验证 → 脚手架 → hello_world → FAQ）；tutorial-game-backend.md
      头部改为显式指向已验证路径；Dockerfile 过时 EXPOSE 注释对齐实际
      （echo 7900；http ops 默认 127.0.0.1 的容器口径说明）
- [x] 架构 P0 安全默认值 1：max_frame_size 默认从「0=不限」改为
      「0=16MiB 默认上限」（kDefaultMaxFrameSize；5 处封包检查点统一走
      effective_max_frame_size），异常长度前缀不再能驱动服务端按其分配
- [x] 架构 P0 安全默认值 2：accepted socket 统一 TCP_NODELAY（游戏小包
      低延迟；best-effort 不影响建连）
- [x] 架构 P0 诚实性：lua.sandbox.allow_os/allow_io 从死配置键变为真实
      开关（VM 创建期条件 open io/os 库；未设置=历史行为开放，随仓库分发
      的默认配置声明 false）+ SandboxGatesOsAndIoLibraries 测试
- [x] 全量构建 + ctest 全绿验证（91 + 新增 3 个测试用例，93/93），逐块
      commit + push（7b395c2 安全默认值 / b1c4cbb 产品化三件套 /
      f337072 评估文档与 todo 重排）
- [x] CI Coverage（Debug 构建）红修复：sol2 在 Debug 下开启安全检查，
      `sol::table` 从 nil proxy（os 被 sandbox 关闭时）构造即触发
      "(type check failed in constructor)" panic-abort，`.valid()` 守卫
      来不及执行——本地 Release（NDEBUG）编译掉该检查故全绿。两处
      （lua_api.cpp AD-07 os 钩子、lua_runtime.cpp restrict_vm）改为
      `get<sol::optional<sol::table>>()` nil 容忍读取；SandboxGates 测试
      补 restrict_vm 双臂真覆盖（替换原 GCOVR 分支豁免）；本地 Debug
      树复现配置验证 + 全量回归

## Phase 1 候选（下一步，均为文档/低风险改动）

- [ ] DB 使用纪律文档 + 示例：同步 ABI 下 DB 调用必须隔离进专职 service
      （业务经 shield.call 访问）+ 超时配置；见 architecture-review.md §4
- [ ] /ops/metrics 口径文档（字段、语义、granularity）
- [ ] net.threads 默认值评估与调优指引（当前默认单线程 legacy 模式）
- [ ] 顶号/重连语义在 gateway.md 显式化（epoch CAS 的时序细节）

## Phase 2 候选（另行立项）

- [ ] DB ABI 异步入口（callback/future + 协程恢复）
- [ ] 连接级限流/黑名单（runtime-security.md 草案落地）
- [ ] blue-green 热更新落地（runtime-lua-vm.md 设计稿）
- [ ] TLS（network.tls 配置面）

## 测试质量 + 防回退收口（2026-09-25，全部完成）

覆盖率三维度 100%（line/branch/function）后的质量收口，不追加数字：

- [x] 弱测试扫描与加强（12 处）：gateway_bridge 6 处 CHECK(true)→
      registry/is_alive/binding 真断言；logger 2 处（ConsoleSink 重定向
      rdbuf 断流分流+Error 升级、RotatingFileSink 残留断言）；
      caf_bridge 析构后 service_manager==nullptr + respawn；
      http_client cleanup 后真请求；global_manager start/stop 幂等后
      data 存活断言
- [x] RegistrationStubs「被 mock 掉真实逻辑」修复（20a0158）：测试
      自声明在 namespace shield::lua::api 内链接到 1396 行空 stub 而非
      968 行真实现；两 namespace 各自声明，stub 照调保函数分母，
      monotonic() 真断言（>0 且单调不减）
- [x] 函数门禁防假绿核对：902 实体按 basename 逐文件核对
      line-rate=1.0 零缺口，与 CI 分母一致；「自声明链错实现」全仓库
      仅 RegistrationStubs 一处；make_error 唯一定义
- [x] ci-fix-report.html 误提交移除（21f6ec1）+ .gitignore 防再犯
- [x] test_global_manager 纳入 CI（7453b32）：tests/CMakeLists 注释
      声称 cluster job 经 global 标签运行它，但从未开
      SHIELD_ENABLE_GLOBAL——测试资产空转；cluster job 补开该开关 +
      label 加 global；本地 Release 树预验证全绿；coverage 维度不开
      （避免 shield_global 进函数分母）
- [x] Windows /proc 平台分支修复（3881f97）：unopenable-path 用例在
      Windows 把 /proc 解析到盘根真创建成功致 !exists 必败；Windows
      改用 C:\Windows\win.ini\x.log

验收：全量 91/91 绿，三维 100%（11550/10854/902），CI 双 run 全绿
（含 Cluster/Coverage/Windows）。

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

- [x] 方向不对称 schema 评估完成（2026-09-20），结论**无需 ABI 扩展**，
      "需要 encode 侧独立 response schema 寻址"的前提已过时：出站
      encode 按目标 s2c 路由自己的 `RouteEntry.schema_name` 寻址
      （`ExternalBodyCodec::encode` → `resolve_outbound_route`），
      gateway 出站强制 `direction == ServerToClient`（同 route_id
      回包被 `egress_direction_rejected` 拒绝；测试锚点
      test_cov_gateway_actor.cpp:251、test_cov_lua_gateway_bridge.cpp:306），req≠resp 即「c2s 一条 + s2c 一条路由」
      各自声明，类型名与路由名不一致用 `request_schema` 覆盖。
      protocol-codec-plugins.md 三处过时表述（决策第 5 点、已知
      边界、Known Limitations）已同步勘正。
- [x] 无 schema codec（json/msgpack）的可选 schema 校验插件（2026-09-20）：
      校验嵌入 codec 插件本体（零管线改动）。新增 `protocol.json` 校验替身
      provider；`protocol.msgpack` 升 1.1.0。实例配置 `schemas`（键 =
      `RouteEntry.schema_name`）XOR `schemas_file`、`require_schema`
      （miss → `protocol.schema_not_found`）、`on_violation`
      reject|warn。校验核心复用 host 子集校验器并补
      `additionalProperties`（仅布尔 false 强制——拼写错误的最后防线）/
      `minLength`/`maxLength`/`minItems`/`maxItems`。decode 违规（reject）
      沿用 decode 失败语义即断连，`warn` 为观察模式。方向不对称 schema
      仍留 Phase 2（上一条）。
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
- [x] xmldef 工具链/文档适配完成（2026-09-22）：runtime 侧经核查无需
      改动——catalog 解析器与 `route_entry_from_descriptor` 两条编译
      路径均已只产出 `schema_name`（显式覆盖 ∨ 路由名同名约定），
      代码里残余的 `schema_id` 全部是「已删键报错」路径。文档面把
      数字 `schema_id` 从 descriptor 契约的全部导出面移除：
      xmldef-descriptor-spec.md（源模型 `schema` attr、语义字段、
      Required Checks、Method/Route IR、debug.json 要求、
      route_constants.json 示例、稳定性规则改 `schema_name`；Route
      节写明边界——数字编号只是具体 schema 系统内部概念，不进
      descriptor 契约与 codec ABI）；xmldef-phase1-implementation.md
      （methods/routes/route_constants 示例、decode 元字段改
      `__xmldef_schema_name`）；xmldef-toolchain-design.md（IR 字段、
      Descriptor Registry 映射、Current Gap 现状注记）；
      xmldef-unity-generator-spec.md（必需 descriptor 字段
      schema name）
