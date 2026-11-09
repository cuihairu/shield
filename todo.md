# TODO

## metric 插件 Lua 面（shield.metrics）
- [x] `shield.metrics(binding)` 脚本侧注册接口：counter/gauge/histogram 对齐 C vtable，labels 为键值表，非法 labels 返回 `invalid_labels`，binding 解析失败 soft-fail
- [x] facade 合同测试补 metric 用例（方法面、冒号/点号双形状、invalid_labels 错误表、soft-fail）
  > facade 套件 3/3 绿；metric 直链套件 4/4 绿
- [x] kickstart 真实演示：app.yaml `metrics.default` binding + game.lua 心跳指标，README 换真实示例输出
- [x] 文档转正 4 处：metrics-prometheus.md Lua 节、lua-api.md Metrics 节、plugin-system.md 矩阵与 namespace 表、manifest `lua:` 块

## 端点插件修复（health.http / metrics.prometheus）
- [x] health.http /health 畸形 JSON 修复 + 新增测试套件
  > test_plugin_health_http 4/4 绿
- [x] health.http / metrics.prometheus 停机死锁修复（shutdown poke loopback 唤醒 accept 循环）
  > 新套件 build/build-plug 两树 4/4 绿，二进制正常退出
- [x] ci.yml / plugins-ci.yml 补 SHIELD_BUILD_PLUGIN_HEALTH / SHIELD_BUILD_PLUGIN_METRIC 开关
- [x] metrics.prometheus 测试套件（文本格式逐行断言、record/record_batch、404、空注册表 200 空体）
  > 两树 4/4 绿

## ops 内嵌仪表盘
- [x] `/` 与 `/ops/dashboard` 内嵌自包含单页仪表盘（2s 轮询只读 `/ops/*`，raw-string 实现）
- [x] DashboardServesSelfContainedHtml 测试
  > 1d6cbbd · test_cov_ops_http 全绿（build 树）

## 依赖更新
- [x] postcss ^8.5.32 + vite 补丁 pin（pnpm-workspace overrides）+ lockfile 同步
  > 4ad06a1 · `pnpm install --lockfile-only --offline` 自洽

## shield.crypto 一期 + jwt.lua
- [x] 原语库 `shield.crypto`：base64/base64url/hex/sha256/hmac_sha256/random_bytes/constant_time_compare
- [x] `scripts/lib/jwt.lua` HS256 参考实现（alg 钉死、签名常数时间比较、exp/nbf/iss/aud 校验）
- [x] plugins/auth_jwt 标弃用（manifest deprecated 块 + 文档标注替代路径）
- [x] test_cov_lua_crypto 一期 12 用例（RFC 向量 + jwt 交叉验证 + 生产注册路径）
- [x] 文档：lua-api.md Crypto 表与 jwt.lua 用法、architecture-decisions.md AD-08、runtime-primitives.md 命名口径差异、open-decisions.md OD-015、roadmap.md Later
  > 五树 ctest 全绿 + 覆盖率六口径全绿

## shield.crypto 二期
- [x] 切片 1：PBKDF2-HMAC-SHA256（RFC 8018 §5.2 向量 + 守卫矩阵）
- [x] 切片 1：HKDF-SHA256（RFC 5869 TC1–TC3 + 守卫矩阵）
  > 36c7782 · test_cov_lua_crypto 24/24 单跑绿
- [x] 切片 2：AES-256-GCM AEAD encrypt/decrypt（NIST 向量 + 篡改拒绝矩阵 + 守卫）
- [x] 切片 3：Ed25519 public_key/sign/verify（RFC 8032 向量 + 尺寸护栏分侧）
  > test_cov_lua_crypto 27→31 用例全绿
- [x] 切片 4：jwt.lua EdDSA + JWKS（RFC 8037 A.1 向量 + 拒绝矩阵 + 全链路往返）
  > test_cov_lua_crypto 31→40 用例全绿
- [x] 文档：lua-api.md 密钥派生/AEAD/签名验签/EdDSA+JWKS 各节
- [x] 覆盖率门禁修复：多行布尔链改用 GCOVR_EXCL_BR_START/STOP 区间
  > CI 37895555333 红 → 本地 CI 同参复跑 100% branch

## jwt.lua 配套测试补全
- [x] JwtJsonEscapeEncoding：json_escape 全臂 + 键排序确定性
- [x] JwtJsonNumberEncoding：整数/小数/指数/NaN/inf/function 分支
- [x] JwtJsonDecodeEscapes：转义与 UTF-8 宽度矩阵（含代理对）
- [x] JwtJsonDecodeMalformed：七类畸形载荷全走 malformed
- [x] JwtVerifyKeyAndTypedClaimArms：key/exp/nbf 守卫 + opts.header 覆盖
- [x] JwtNonArrayObjectKeysAndSignGuards：稀疏数组键静默丢值 bug 修复 + 回归钉
  > 五树 ctest 全绿 + 覆盖率六口径全绿（仅测试文件 + jwt.lua）

## 并发 spawn 竞态用例
- [x] 起跑屏障 + 角色无关不变量断言（恰好一个成功，败者错误码接受 reserved/already exists）
  > 五树 ctest 全绿

## 覆盖率收口
- [x] 2026-11-08 本仓库 codecov 口径实测：排除生成代码/构建缓存/第三方依赖后，gcovr 过滤 `../src/` + `../include/shield/`，行 99.75%、分支 100%，均 ≥98% 门槛
  > build-cov 全量 ctest 97/98（唯一超时 test_cov_http_client 为既有环境 flaky：本机并行负载时该套件偶发 120s 超时，单独运行 2.1s 稳定通过）；重跑该套件补齐缺失行后 coverage.xml line-rate=0.9975、branch-rate=1.0（14403/14439 行、12871/12871 分支）
- [x] 头文件层真缺口补真测试：result.hpp map 双臂单实例化、session_stream transport_name、ip_blocklist v6 非对齐前缀与空白条目
- [x] 不可达臂 / 编译器伪影按登记口径标注（GCOVR_EXCL_*，理由入注释）
- [x] 覆盖率纳入 CI 门禁：filter 扩为 `../src/` + `../include/shield/`，阈值 98/100/100
- [x] 覆盖率口径纠偏：claim_name 不可达臂标注 + 空 out-param 臂补真测试 + on_rate_limited 两臂
  > src/ line/branch/function 100%（11385/11385）；include/ 520/520 与 522/522 两轮；五树 ctest 全绿（97/99/99/97/97）

## 门禁遗留修复（test_db_sqlite_async）
- [x] 预存在回归修复：3s blind sleep 换 gauge 观测排空 + 迟到完成断言
  > 默认树 104/104 全绿 ×3
- [x] CI Linux 腿开 SHIELD_BUILD_DB_PLUGIN_SQLITE=ON + VCPKG_MANIFEST_FEATURES=database-sqlite
- [x] 第二层时序依赖修复：断言改合法二值析取 + 超时前提用例绑 30ms 预算的 db.poison
  > 8dc5f02（2026-10-09）

## 产品化 + 架构安全默认值
- [x] docs/architecture-review.md 九维架构评估
- [x] docs/product-gap.md 8 维对照 skynet + top-3 挡路项
- [x] P0-1 默认配置可观测：echo actor + 真实启动 acceptance 测试
- [x] P0-2 模板工程一条命令：templates/minimal_game + scripts/new_project.sh + ctest 锚定
- [x] P0-3 build.sh 环境预检 + scripts/client_demo.py + quickstart.md 线性化
- [x] 安全默认值：max_frame_size 默认 16MiB 上限、accepted socket TCP_NODELAY、lua.sandbox allow_os/allow_io 真实开关
- [x] CI Coverage（Debug）红修复：绑定层 nil 容忍读取 + SandboxGates 真覆盖
  > 7b395c2 / b1c4cbb / f337072 · 93/93 全绿

## 回归修复
- [x] 固定端口测试互撞：3 套件挂 CTest RESOURCE_LOCK fixed_ports
- [x] ms_left 断言差一：改 `<= 5000` 并注释等号来源
  > Release/Debug 双树 93/93，-j 8 连跑 4 次全绿
- [x] docs/deployment.md 重写（修正 build-release 目录事实错误，build.sh 固定用 build/）

## 连接级限流与黑名单
- [x] network.rate_limit 配置面 + TokenBucket（按解码后消息计费，惰性回填）
- [x] network.blocklist.deny 配置 + accept 建会话前拦截（不占连接计数）
- [x] 测试：test_cov_ip_blocklist 18 用例 + listener 4 用例 + config/bootstrap 校验与接线
- [x] 网关观测收口：listener 原子计数 + ListenerRegistry + `/ops/metrics` 指标族
  > CI gate line/branch/function 100%

## Phase 1 / Phase 2 候选
- [x] docs/db-discipline.md 使用纪律 + 示例
- [x] runtime-ops.md `/ops/metrics` 抓取口径
- [x] runtime-config.md net.threads 默认值评估与调优指引
- [x] gateway.md 顶号/重连时序语义显式化
- [x] DB 异步入口：db-async-design.md 立项 + M1-M4 实现（挂起恢复原语 → sqlite shim → mysql/pg 接入 → tx 异步 + 指标）
  > dc2700a / 46634f5 / d2b68ef / 10133a1
- [x] blue-green 热更新：`shield.claim(name)` 原子接管 + 测试 + 文档对齐
- [x] TLS：network.tls 配置面（双层 fail-loud、SessionStream 抽象、握手前 blocklist/限流、无降级）
- [x] schema 方向不对称评估（结论无需 ABI 扩展）+ 无 schema codec 校验插件 + request_codec 评估（保留）
- [x] Schema 寻址收敛：ABI 删 schema_id/route_id/codec_id，只留 route_name；已删键 YAML/JSON 双路径报错
  > 全量重编 + build-plugins 串行测试 + clang-format
- [x] listener bind 失败三类悬垂根因修复（ASan 取证）+ 三用例摘平台守卫
  > 57dea31 · CI 三平台全绿
- [x] shield.sleep 续延错误臂对齐 + load_script 改 luaL_loadfile+pcall 保护式
- [x] xmldef 工具链/文档适配：descriptor 契约全导出面移除数字 schema_id

## 服务停机 hook on_shutdown
- [x] on_shutdown(ctx) drain 调度落地（spawn 逆序 + 共享预算 + drain 期间 spawn 拒绝）
  > test_cov_lua_shutdown 6 用例 · lua-api/runtime-service/runtime-config 实现状态翻转

## 测试质量 + 防回退收口
- [x] 弱测试扫描与加强 12 处（CHECK(true) → 真断言）
- [x] RegistrationStubs 错链空 stub 修复
  > 20a0158
- [x] 函数门禁防假绿核对：902 实体零缺口
- [x] ci-fix-report.html 误提交移除 + .gitignore 防再犯
  > 21f6ec1
- [x] test_global_manager 纳入 CI（cluster job 开 SHIELD_ENABLE_GLOBAL）
  > 7453b32
- [x] Windows /proc 平台分支修复
  > 3881f97
  > 全量 91/91 绿，三维 100%（11550/10854/902），CI 双 run 全绿

## 覆盖率巡检：可选模块
- [x] test_lua_api_global 真分支双根因修复（kGlobalOrchestration ttl 字段覆盖 + CI label 缺失）
  > GLOBAL-on 13/13 绿，套件进 CI Cluster job
- [x] global_manager 4 未覆盖函数补真用例 + console 三文件 GLOBAL-on 100% 实测
- [x] 四开关三口径实测 + CI coverage-optional job 封死盲区
  > cluster/server/player/global TU line 100%

## 代码深审 P2/P3（第一波，2026-11-01）
- [x] BUG-3 redis.driver free_value 浅释放改真递归
- [x] RACE-2 lua_http_bridge 路由槽 UAF 改 shared state
  > test_cov_lua_http_bridge / test_cov_http_bridge2 / test_cov_lua_runtime2 全绿
- [x] BUG-2 _resume_after 定时器失败泄漏改 SleepAnchor RAII
  > 7c69651 · test_cov_lua_service / test_cov_cpp_lua_api / test_lua_api_timers 全绿
- [x] BUG-6 console_session send_line 裸 this 改 shared_from_this
  > a173bf0 · test_console_server / test_cov_console_net / test_cov_root_commands 全绿
- [x] BUG-7 pgsql rewrite 字面量误改改词法感知重写器
  > 9c3172a · test_plugin_db_pgsql_rewrite 9 用例全绿
- [x] RACE-3/4 running_/listening_ 改 std::atomic&lt;bool&gt;
  > a1f5d8e · 相关六套件全绿
- [x] RACE-5/6 acceptor close-vs-accept 收口（do_accept 入口 listening_ 守卫）
  > 7400271 · test_cov_listener / test_cov_session 全绿
- [x] BUG-9 lua_runtime 窄化转换改 int64 + INT_MAX 拒绝
  > e7acdc0 · 相关五套件全绿
- [x] BUG-8 cache.redis incr_by 整数回复护栏 + 真实 redis 集成测试
  > c29b889 · test_plugin_cache_redis_incrby 全绿
- [x] BUG-5 leaderboard 悬垂名字改自持 std::string
  > a63bdcb · test_plugin_leaderboard_names 全绿
- [x] SEC-2 JWT 缺 exp 拒绝（token_expired）
  > b04240c · test_plugin_auth_jwt 全绿
- [x] SEC-3 裸 __shield_client_ref 标记伪造拒绝（只收受信 userdata）
  > eb216a3 · 相关四套件全绿
- [x] RACE-7 gateway close 补 epoch 校验
  > 3302983 · test_cov_gateway_actor / test_lua_api_client_rpc 全绿
- [x] P3 force_remove 补 timer/call-timeout 驱动清理（与 exit() 同构）
  > ce26cce · 相关六套件全绿
- [x] P3 clang-tidy 既有警告确认无本批新增（观察项，无代码改动）
  > 10bd7be

## 代码深审第二波（2026-11-03/04）
- [x] C1 TLS 握手状态 UAF：weak registry + HandshakeGate + adopt 复查
  > 6477508 · test_cov_tls 13/13 等全绿
- [x] C2 last_rejection_ 跨线程读写加锁
  > 356a0c8 · test_cov_listener 20/20 绿
- [x] C3 msgpack 解码深度守卫 512（非递归 walker + encode 侧同款）
  > d83c1e8 · test_protocol_msgpack_plugin 11/11 绿
- [x] C4 cluster_transport impl_->actor 无锁读写删字段统一 outbound_actor
  > cb142c5 · test_cluster_transport / test_cluster_manager / test_cov_cluster 全绿
- [x] C5 remote_send_fn 锁内快照后再调用
  > b1d3a46 · test_cluster_manager 28 例 / test_cov_cluster 12 例全绿
- [x] C6 broadcast_attach 初始游标下溢钳 0
  > c5fd319 · test_global_manager 77/77 绿
- [x] C7 sampler sweep_active 按服务门控
  > 269807e · test_ops_profile 17/17 等 8 套件全绿
- [x] C8 sleep 定时器误报错误：lua_status 守卫替代盲目 resume
  > c0fab65 · test_cov_lua_service 50/50 等全绿
- [x] D1 cluster running 改原子
  > 51c252c · test_cluster_manager 28/28 / test_cov_cluster 12/12 绿
- [x] D2 player node_id_ 加锁快照
  > f4808a5 · test_player_manager 28/28 绿
- [x] D3 cron step 上界拒绝防溢出
  > 9c5fa65 · test_global_manager 77/77 绿
- [x] D4 max_frame_size 构造死旋钮生效（session 级覆盖 pipeline）
  > daa233d · test_cov_session 30/30 等全绿
- [x] D5 error_code_ 跨线程读写加锁
  > b40e897 · test_cov_session 30/30 绿
- [x] D6 limited_count_ 改原子
  > 2b9e90e · test_cov_session 30/30 绿
- [x] D7 HTTP 客户端响应体上限（max_response_size）
  > 01bd3a9 · test_cov_http_client 11/11 绿
- [x] D8 ConsoleServer 裸 this 改 SharedState
  > 1588a77 · test_console_server 5/5 等全绿

## Redis 审核收口（394351a / 7a94beb / 715a0a3 / 178d73b 批次）
- [x] free_value 语义变更回归修复：pipeline 元素独立堆分配 + 逐元素释放
- [x] ABI 契约同步：free_value 语义 + pipeline `shield_redis_value_v1***` 签名写入 redis.h
- [x] v1 ABI 冻结、接口升 shield.redis.v2，v1 vtable 请求返回 NULL
  > b9f9fd0 · test_redis_driver_free_value 5/5 绿
- [x] pipeline calloc 判空（bad_alloc 错误臂 + 注入测试）
  > b9f9fd0 · test_redis_driver_free_value 5/5 绿
- [x] 测试补齐：pipeline 所有权契约单测 + 真实 redis-server 集成 + bridge 析构后路由覆盖
  > cab3aed · test_cov_http_bridge2 7/7 绿
- [x] `--wrap` 链接选项加 GNU/Clang 平台护栏
- [x] free_value_contents 深递归标注为既有对称暴露面（接受）

## 修复批次复审（178d73b..HEAD）
- [x] SEC-3 收口：invoke_client_rpc 剔除不受信 decoded_request 中的伪造标记
  > 9053060 · test_lua_api_client_rpc 11/11 等全绿
- [x] cleanup_failed_initialize 的 cluster_transport->stop() 前移镜像成功路径
  > f3bf9a8 · test_cov_bootstrap / test_cluster_* 全绿
- [x] pgsql 重写器：含 `$` 未加引号标识符不再误判 dollar-quote
- [x] pgsql 重写器：块注释改 depth 计数支持嵌套
  > fab5e5e · test_plugin_db_pgsql_rewrite 11/11 / test_db_net_async 全绿
- [x] incr_by 测试补 driver 路径两用例（伪造 vtable 注入）
  > af8b571 · test_plugin_cache_redis_incrby 3/3 绿
- [x] force_remove 补 recently_exited 封顶
  > 30bf228 · 相关五套件全绿
- [x] pgsql JSON 运算符 `?`/`?|`/`?&` v1 合同钉住
  > 77cc19c · test_plugin_db_pgsql_rewrite 12/12 绿
- [x] upload() 响应体上限 1 MiB + 回归
- [x] plaintext accept 完成尾部并入 HandshakeGate 守卫
- [x] settle()/cancel() 对同一 socket 的 close 串行化
  > test_cov_tls 15/15 绿（上三条同批）
- [x] invoke_client_rpc 补 ProfileSampler::sweep_active(service_id)
  > test_cov_lua_runtime 48/48 绿
- [x] redis.driver 头注释更新为 v2 口径
  > test_redis_driver_free_value 全绿
- [x] console_server 弃用 io_ 成员移除
  > test_cov_console_net 12/12 绿
- [x] 测试助手补整体 deadline + port 0 临时端口消除 TOCTOU
  > test_cov_http_bridge2 7/7 绿

## dashboard 立项：React + TS + antd + ProComponents 运维控制台（FIX-WEB-01~14）
- [x] FIX-WEB-01 钉住 ops API 契约 docs/dashboard-api.md（逐端点 envelope/错误/超时 + 缺口清单）
  > test_cov_ops_http 45/45 绿
- [x] FIX-WEB-02 React 控制台工程（react 18.3.1 / antd 5.27.6 / pro-components 2.8.10 / vite 6.4.4 锁版本，入 pnpm workspace）
  > pnpm build 成功 · C++ 回归 108/108 绿
- [x] FIX-WEB-03 ops Bearer 认证门（http.ops_auth_token，/ops/health 豁免，常数时间比较）
  > test_cov_ops_http 48/48 绿
- [x] FIX-WEB-04 静态托管与 SPA 路由（set_static_root、穿越防护、未知 /ops/* JSON 404）
  > test_cov_ops_http 52/52 绿
- [x] FIX-WEB-05 只读运维总览页（health/server/cluster/uptime，断网 stale 标记）
  > pnpm build 成功 · 套件 52/52 绿
- [x] FIX-WEB-06 Lua 服务页（列表 + 详情，timeout 504 与 404 区分展示）
  > pnpm test 4/4 绿
- [x] FIX-WEB-07 插件与连接池页（required degraded 口径与 health 一致，池指标同源对账）
  > pnpm test 6/6 绿 · test_cov_ops_http 53/53 绿
- [x] FIX-WEB-09 进程机资源指标（shield_process_rss_bytes/threads/open_fds/cpu_percent）
  > test_cov_ops_http 53/53 绿（ProcessResourceMetricsEmitted）
- [x] FIX-WEB-10 类型化客户端与轮询层（2s 基线、指数退避、queryId 竞态防护）
  > pnpm test 2/2 绿 · C++ 108/108 绿
- [x] FIX-WEB-08 网关连接与限流页（per-listener 计数/TLS 握手成败）
  > pnpm test 9/9 绿 · pnpm build 成功 · test_cov_ops_http 53/53 绿（GatewayMetricsEmittedFromListenerRegistry/GatewayTlsMetricsEmittedFromListenerRegistry）
- [x] FIX-WEB-11 配置脱敏视图页（脱敏键族标注，不回显原文）
  > eecc22b · pnpm test 12/12 绿 · pnpm build 成功 · test_cov_ops_http 53/53 绿 · ctest 108/108 绿（console/src/pages/ConfigPage.tsx:11,32,91 · console/src/pages/__tests__/config.test.tsx:40,53）
- [x] FIX-WEB-12 eval/profile 受控操作页（依赖 FIX-WEB-03、FIX-WEB-10：仅在端点 opt-in 注册时可达的 profiler 表单与 eval 台；token 手工输入、不持久化 localStorage、危险操作显式标注。验收：端点未启用时页面显示不可用及原因，不显示假表单；token 不落盘不进日志；profile 409/429/504 分支如实展示）
  > console/src/pages/EvalProfilePage.tsx（端点启用判定、token 仅 state、409/429/504 分支、不可用原因）· console/src/services/api.ts:19-23 · console/src/services/polling.ts:28-48 · console/src/App.tsx · pnpm test 16/16 绿 · pnpm build 成功
- [x] FIX-WEB-13 真机浏览器验收与安全门（依赖 FIX-WEB-03、FIX-WEB-04、FIX-WEB-12：真实 shield 进程 + 真浏览器用例及证据——未认证拒绝、凭据失效、断网过期显示、API 缺失错误、路径穿越拒绝。验收：拒绝请求不产生副作用；不得以 test_cov_ops_http 进程内用例或内嵌单页用例充当浏览器证据；证据（截图/日志/CI artifact）可追溯）
  > tests/acceptance/e2e/run-e2e.mjs（真实 build/bin/shield + Playwright Chromium，9 场景全绿：未认证 401、SPA 壳可载入、错误凭据 401、正确凭据 200、未知 /ops 404、穿越不越界、断网 stale 横幅）；证据落 tests/acceptance/e2e/artifacts/（截图 01/02 + summary.json，本仓库可复现、README 记载）
- [x] FIX-WEB-14 生产构建与托管收口（依赖 FIX-WEB-04、FIX-WEB-13：CI 构建前端产物并随 ops 面托管交付；构建产物无秘密；docs（diagnostics-console.md、deployment.md）更新接入方式与开发代理说明。验收：生产路径无前端 dev server；SPA 直刷正常；产物不含秘密有检查手段）
  > .github/workflows/ci.yml 新增 console-build job（pnpm install --frozen-lockfile + pnpm build + 产物秘密扫描 + upload shield-console-dist artifact）· docs/deployment.md、docs/zh/deployment.md、docs/diagnostics-console.md 更新接入方式与开发代理说明（http.static_root 托管 console/dist，无 dev server，SPA 直刷经 FIX-WEB-13 验收，产物秘密扫描命中即失败）

## 开放审核发现（2026-11-04，待修）
- [x] 默认未配置 http.ops_auth_token 时只读运维接口全开放，需默认拒绝显式放行（src/console/ops_http_handler.cpp:629）
  > register_routes 改为默认拒绝：auth_token_ 为空仍拒绝，仅 http.ops_allow_unauthenticated=true 显式放行；OpsDefaultDenyWithNoTokenConfigured 覆盖（tests/coverage/test_cov_ops_http.cpp:389-441）
- [x] 前端 fetch 无 Bearer 凭据入口，认证开启后浏览器无法使用受保护接口（console/src/services/polling.ts:39）
  > LoginGate 组件 + getOpsToken/setOpsToken（sessionStorage，不落盘）；fetchJson/fetchText 附 Bearer 头（console/src/components/LoginGate.tsx · console/src/services/polling.ts:28-48）
- [x] eval/profile 路由未经 ops-wide 认证包装（src/console/ops_http_handler.cpp:716）
  > authed_control 包装 /ops/eval 与 /ops/profile：ops_auth_token 配置时 Bearer 须先过 ops 门（src/console/ops_http_handler.cpp:660-676,745,779）；OpsAuthGateCoversEvalAndProfile 覆盖
- [x] 静态目录启用时默认处理器未加认证（src/console/ops_http_handler.cpp:697）
  > set_default_handler 包 authed + set_static_guard 保护 try_static（src/console/ops_http_handler.cpp:655-673,727 · src/net/http_server.cpp:148-158）；StaticGateDeniesByDefault 覆盖
- [x] 未命中路由先查静态再判 API 前缀，ops 子目录可吞未知 /ops/* 的 JSON 404（src/net/http_server.cpp:317）
  > dispatch 对 /ops/* 前缀先走 default handler（JSON 404），仅非 API 路径落静态（src/net/http_server.cpp:316-329）
- [x] /ops/dashboard 用含查询串的原始 target 比较，`?x=1` 时误 404（src/console/ops_http_handler.cpp:1597）
  > handle_default 先剥查询串再判路径（src/console/ops_http_handler.cpp:1590-1595）
- [x] 轮询退避读 effect 初始 state.data，退避不随实际状态切换（console/src/services/polling.ts:123）
  > usePolling 用 hasDataRef 跟踪最新快照决定退避（console/src/services/polling.ts:110-113,160-163）
- [x] 服务列表与插件页仅挂载时请求一次，无过期提示（console/src/pages/PluginsPage.tsx:51）
  > PluginsPage 改用 usePolling（stale 横幅 + 401 登录门）；ServicesPage 增序号/取消保护（console/src/pages/PluginsPage.tsx:47-60 · console/src/pages/ServicesPage.tsx:31-34,55-78）
- [x] 服务详情请求缺取消/序号保护，晚返回覆盖新选择（console/src/pages/ServicesPage.tsx:61）
  > detailSeq + detailAbort：晚返回被丢弃（console/src/pages/ServicesPage.tsx:55-78）
- [x] accept 回调持 handshake gate 锁同步触发 on_connect，回调调 stop() 死锁（src/net/listener.cpp:301）
  > do_accept 验证 stopping 后释放 gate 再 adopt_connection（src/net/listener.cpp:413-432）；test_cov_listener/tls/session 全绿
- [x] SPA 回退未做根目录与符号链接规范化检查，可读目录外文件（src/console/ops_http_handler.cpp:1601）
  > handle_default 对 static_root 与 index.html 做 weakly_canonical + 包含检查（src/console/ops_http_handler.cpp:1620-1642）
- [x] fetchJson 仅查 401，非 2xx 非 error 信封被当成功处理（console/src/services/polling.ts:43）
  > fetchJson/fetchText 对 !res.ok 抛 HttpError（带状态码），error 信封取 data/message（console/src/services/polling.ts:52-72）

## 审核发现
- [x] 审核发现：整理未完成的 FIX-WEB-08～14 时删除了原有依赖及关键交付和验收条件，包括网关计数语义、eval/profile token 禁止持久化、浏览器安全场景与生产托管要求，后续可在遗漏这些约束时误判完成（todo.md:263）。
  > todo.md 已为 FIX-WEB-12~14 补齐依赖、交付与验收条件（todo.md:267-269）
- [x] 审核发现：波次 2 审计称中英文侧栏均已补充 Dashboard HTTP API 入口，但中文侧栏只新增诊断控制台，中文站点侧栏仍无法导航到该契约（docs/审计-文档一致性.md:83）。
  > 中文侧栏"官方可选模块"补 `Dashboard HTTP API 契约（英文）` 条目（docs/.vitepress/config.ts:199）
- [x] 审核发现：.VPDoc.has-aside .content-container 强制 max-width:960px 未为右侧 TOC 侧栏（默认 max-width:256px+32px 间距）预留空间，1280–1440px 视口下内容列+侧栏总宽约 1248px 超出可用宽度，右侧目录会被挤压或裁切（docs/.vitepress/theme/custom.css:67）
  > 移除 .VPDoc.has-aside .content-container 与两条重复 max-width 规则，恢复 VitePress 响应式（docs/.vitepress/theme/custom.css:59-66）
- [x] 审核发现：word-break: break-word 为规范弃用的非标准值，等价于 overflow-wrap: break-word，建议改用标准属性（docs/.vitepress/theme/custom.css:59）
  > 改用标准 overflow-wrap: break-word（docs/.vitepress/theme/custom.css:58-60）
- [x] 审核发现：.VPDoc .container > .content 与 .VPDoc:not(.has-sidebar) .content 两条规则均强制 max-width:960px !important，功能重复；且 !important 会覆盖 VitePress 默认响应式宽度（≥960px 时 752px、≥1440px 时 784px），破坏其响应式布局意图（docs/.vitepress/theme/custom.css:61-66）
  > 两条重复规则已删除（docs/.vitepress/theme/custom.css:61-66）
- [x] 审核发现：新"查询任意配置键"功能将非脱敏值以明文 `<code>` 回显，脱敏完全依赖服务端按固定子串（token/password/passwd/secret/credential/api_key/apikey/access_key/private_key）匹配，`auth.bearer`、`mysql.pass`、`db.connection_string` 等不命中子串的密钥会被当普通配置原样回显（console/src/pages/ConfigPage.tsx:93，服务端匹配见 src/console/ops_http_handler.cpp:1517-1524）。
  > 服务端 kSecretNeedles 扩充 bearer/auth/pass/connection_string/dsn/pwd；ConfigEndpointValueShapes 覆盖（src/console/ops_http_handler.cpp:1517-1524 · tests/coverage/test_cov_ops_http.cpp:1775-1788）
- [x] 审核发现：客户端对 key 用 encodeURIComponent 编码（console/src/services/api.ts:15），但服务端 /ops/config 直接取原始查询串不 URL 解码（src/console/ops_http_handler.cpp:1483），含空格 `/` `:` `&` `=` 等字符的用户输入键永不匹配真实键，一律误显"未配置"。
  > handle_config 新增 url_decode；ConfigEndpointValueShapes 覆盖空格/斜杠/冒号键（src/console/ops_http_handler.cpp:60-84,1647 · tests/coverage/test_cov_ops_http.cpp:1768-1773）
- [x] 审核发现：初始加载用 Promise.all 全有全无，任一键查询失败（401/超时/信封错误）即整体报错页，即使其余键已成功也无部分结果渲染（console/src/pages/ConfigPage.tsx:41）。
  > ConfigPage 逐键独立查询，成功键照常渲染；unauthorized/error 单独分流（console/src/pages/ConfigPage.tsx:47-79）
- [x] 审核发现：计数器回归检测在端口行从某次快照消失时丢弃其前值，监听器重启若短暂移除端口行再以重置计数回归则不被标记，与"监听器重启即异常"的注释意图不符（console/src/pages/GatewayPage.tsx:87-96）。
  > GatewayPage 合并前值：消失端口的前值保留，重置计数回归仍被标记（console/src/pages/GatewayPage.tsx:95-100）
- [x] 审核发现：parseSample 数值正则 [-0-9.eE+]+ 不含 N/I/f，Prometheus 合法 NaN、+Inf、-Inf 采样整行被静默丢弃；形如 `1.2.3` 的畸形数值经 parseFloat 截断为 1.2 而非拒绝该行；标签按 `,` split 对含逗号的引号值（如 reason="a,b"）解析错位（console/src/pages/GatewayPage.tsx:18,22,27）。
  > GatewayPage/PluginsPage parseSample 支持 NaN/±Inf、拒绝畸形数字、引号标签逗号感知（console/src/pages/GatewayPage.tsx:18-47 · console/src/pages/PluginsPage.tsx:8-37）
- [x] 审核发现：ConfigPage 无独立 unauthorized 处理（与 GatewayPage 不同），/ops/config 返回 401 时仅显示通用"Failed to load config"而非"Authentication is required"提示（console/src/pages/ConfigPage.tsx:38）。
  > ConfigPage 新增 unauthorized 状态并渲染 LoginGate（console/src/pages/ConfigPage.tsx:31,98-101）
- [x] 审核发现：addKey 的 onPressEnter 未以 pending 门控，按钮 loading 禁用期间按回车仍会触发并发请求，多个查询交错 resolve 时错误/结果状态可互相覆盖（console/src/pages/ConfigPage.tsx:118）。
  > addKey 以 pending 门控，in-flight 时 onPressEnter 直接返回（console/src/pages/ConfigPage.tsx:67-69）
- [x] 审核发现：启用 ops Bearer 认证后，浏览器首次导航和加载静态资源无法携带该请求头，因而连登录页面都无法加载（src/console/ops_http_handler.cpp:787）。
  > 静态 SPA 壳与静态资源改为公开加载（浏览器首次导航无 Authorization），数据 API（/ops/*）仍走 Bearer 门；页面首次 401 渲染 LoginGate（src/console/ops_http_handler.cpp:781-800,864-877 · tests/coverage/test_cov_ops_http.cpp:2826-2860 StaticGateDeniesByDefault 覆盖资产公开+数据 API 仍 401）
- [x] 审核发现：受控接口同时用同一个 Authorization 头校验 ops token 和独立的 eval/profile token，两个 token 不同时相等时请求必然返回 401（src/console/ops_http_handler.cpp:826）。
  > eval/profile 为显式 opt-in，其独立 token 门即授权；不再叠加 ops-wide 门（单个头无法携带两个 token）（src/console/ops_http_handler.cpp:814-819 · tests/coverage/test_cov_ops_http.cpp:450-472 ControlEndpointsUseTheirOwnTokenUnderOpsAuth 覆盖独立 token 200）
- [x] 审核发现：健康接口在服务降级时以 503 返回有效的健康数据，新 fetchJson 却将其当作请求失败，导致仪表盘无法显示降级详情且可能保留旧的健康状态（console/src/services/polling.ts:92）。
  > fetchJson 对 503 仍解析信封返回 data，仅 401 与错误信封/非 JSON 算失败（console/src/services/polling.ts:83-107 · console/src/services/__tests__/polling.test.tsx fetchJson 503 degraded 用例）
- [x] 审核发现：监听器接受回调释放生命周期锁后仍访问 this，stop 或析构可在检查后并发完成，导致关闭后仍接纳连接或访问已销毁对象（src/net/listener.cpp:417）。
  > adopt_connection 注册后返回 session；session->start() 移至释放 gate 之后，on_connect 调 stop() 不死锁，且本地下游 shared_ptr 防 UAF（src/net/listener.cpp:108-122,411-447 · tests/coverage/test_cov_listener.cpp:236-262 OnConnectMayStopListenerWithoutDeadlockOrUseAfterFree）
- [x] 审核发现：受控操作页只检查 enabled 配置，启用标志为 true 但对应 token 为空时服务端并未注册端点，页面仍显示可操作表单（console/src/pages/EvalProfilePage.tsx:53）。
  > EvalProfilePage 同时查询 eval_token/profile_token，token 为空时判为未启用（console/src/pages/EvalProfilePage.tsx:41-54 · console/src/pages/__tests__/eval-profile.test.tsx enabled-but-empty-token 用例）
- [x] 审核发现：轮询的 hasDataRef 在每次 run 内重新初始化，成功快照后的每次失败仍按无数据状态退避，无法按预期依据已有快照选择轮询间隔（console/src/services/polling.ts:155）。
  > everHadData 提升到 effect 作用域，跨 run 持久；成功后失败仍按正常间隔轮询（console/src/services/polling.ts:147-150,156,190 · console/src/services/__tests__/polling.test.tsx 成功后退避用例）
- [x] 审核发现：SPA 回退的目录包含检查使用硬编码正斜杠，Windows 原生路径使用反斜杠时会拒绝根目录内的 index.html，导致深链接刷新失败（src/console/ops_http_handler.cpp:1811）。
  > 用 lexically_relative 做分隔符无关包含检查（src/console/ops_http_handler.cpp:1804-1811）
- [x] 审核发现：macOS 指标采集使用 ru_maxrss 作为当前 RSS，进程内存下降后仍报告历史峰值，误导资源监控（src/console/ops_http_handler.cpp:190）。
  > macOS RSS 改用 mach_task_basic_info resident_size（实时驻留），ru_maxrss 仅作回退（src/console/ops_http_handler.cpp:189-201）
- [x] 审核发现：macOS 每次 task_threads 调用取得的线程端口未逐个释放，仅释放数组会使持续抓取指标不断泄漏 Mach 端口权限（src/console/ops_http_handler.cpp:201）。
  > task_threads 返回的每个线程端口逐个 mach_port_deallocate（src/console/ops_http_handler.cpp:203-209）
- [x] 审核发现：accept 回调在取得生命周期锁并检查后立即释放锁，随后仍访问 socket_ 等成员，监听器并发析构时仍可能发生 use-after-free（src/net/listener.cpp:428）。
  > accept 回调整段持有 gate（含 remote_endpoint/blocklist/adopt_connection 等所有 this 成员访问），仅 session->start() 前释放，避免并发析构 UAF；新增 ConcurrentStopDuringAcceptTailIsSafe 并发竞态回归（src/net/listener.cpp:313,442 · tests/coverage/test_cov_listener.cpp:273 · build/build-cov 两树 test_cov_listener 绿）
- [x] 审核发现：stop 在握有 gate 的接纳过程期间设置 stopping_ 时，adopt_connection 会解引用已移入 TcpSession 的空 stream，导致进程崩溃（src/net/listener.cpp:490）。
  > stopping_ 检查移入 sessions_mutex_ 临界区并在 make_shared<TcpSession>（move stream）之前执行，命中时先 close 再返回 null，不再解引用空 unique_ptr；新增 AdoptConnectionAfterStopDropsSocket 确定性回归（src/net/listener.cpp:479-493 · tests/coverage/test_cov_listener.cpp:319 · build/build-cov 两树 test_cov_listener 绿）
- [x] 审核发现：释放 gate 后调用 start 前若 stop 已关闭该 session，仍会触发 on_connect 并启动接收，造成关闭后的连接事件和断开事件顺序颠倒（src/net/listener.cpp:447）。
  > TcpSession::start 入口加 alive_ 守卫，已关闭 session 直接 no-op（不触发 on_connect、不启动接收）；新增 StartOnClosedSessionIsNoOp 确定性回归（src/net/session.cpp:112 · tests/coverage/test_cov_session.cpp:1065 · build/build-cov 两树 test_cov_session 绿）
- [x] 审核发现：错误信封在检查 HTTP 状态前被转换为普通 Error，导致 profile 的 409、429、504 状态码分支失效（console/src/services/polling.ts:106）。
  > fetchJson 对 type=error 信封改抛 HttpError（携带 res.status），profile 页面可按 409/429/504 分支；新增 polling.test.tsx 三个状态码参数化用例（console/src/services/polling.ts:108-115 · console/src/services/__tests__/polling.test.tsx:33-54 · vitest 全绿 23/23）
- [x] 审核发现：浏览器验收配置把静态目录硬编码为本机绝对路径，其他工作区或 CI 无法加载控制台并复现验收（tests/acceptance/e2e/shield-e2e.yaml:47）。
  > 配置改为占位符，run-e2e 渲染时从 checkout ROOT 解析 `console/dist` 绝对路径；本地验证渲染结果为绝对路径且存在（tests/acceptance/e2e/shield-e2e.yaml:47 · tests/acceptance/e2e/run-e2e.mjs:51-58）
- [x] 审核发现：穿越验收使用 fetch 发送 %2e%2e 路径时客户端先将其规范化为 /etc/passwd，服务端实际未收到穿越路径而测试仍可通过（tests/acceptance/e2e/run-e2e.mjs:164）。
  > 改用 Node http.request 原样发送 `/%2e%2e/...`，断言真实穿透返回 JSON 404 且无 /etc/passwd/HTML；真实 shield 进程实测 status=404、body=not found（tests/acceptance/e2e/run-e2e.mjs:105-130,196-212）
- [x] 审核发现：start 的 alive_ 检查与 on_connect 调用未同步，若 stop 在两者之间关闭 session，仍会在 on_disconnect 之后触发 on_connect 并尝试接收（src/net/session.cpp:120）。
  > 用可重入 lifecycle_mutex_ 串行化 start/close 的 connect/disconnect 回调投递；on_connect 期间并发 close() 不再先发 on_disconnect。新增 ConcurrentCloseDuringConnectPreservesEventOrder 回归（src/net/session.cpp:112-127,290-293 · include/shield/net/session.hpp:329 · tests/coverage/test_cov_session.cpp:1083 · build 树 test_cov_session 32/32 与 test_cov_listener 23/23 绿、ctest 108/108 绿）
- [x] 审核发现：本轮 diff 未见问题（65d5b46f..HEAD）。
  > 核实：65d5b46f..HEAD 仅含 1b8c3db（删除 package.json 中已由 pnpm-workspace.yaml 承接的 stale overrides），lockfile 离线自洽（pnpm install --lockfile-only --offline 无变更）；pnpm test 23/23 绿、pnpm build 成功。
- [x] 审核发现：新增符号链接测试强制要求创建链接成功，而 Windows 普通权限或未启用开发者模式时创建符号链接会失败，导致 Windows 测试矩阵失败（tests/coverage/test_cov_ops_http.cpp:3064）。
  > 用 Boost.Test precondition 装饰器替换硬性 REQUIRE：一次性探测 create_symlink，失败即整体跳过该用例（Windows 无开发者模式），Linux 仍全 4 case 原样运行。实测 Linux build 树 test_cov_ops_http StaticSymlinkContainment 4 case 全绿（121ms 无错）；Windows 语义由 precondition 声明 skipped 而非失败。commit 4d98840（tests/coverage/test_cov_ops_http.cpp:291-309,3079-3083）。
- [x] 审核发现：覆盖率排除区域屏蔽了实际可达的未知 API 404 分支和强制移除服务时的超时清理分支，令 CI 的行及分支覆盖率门禁无法发现这些逻辑未被测试（src/console/ops_http_handler.cpp:1906、src/lua/lua_service.cpp:3528）。
  > 两处 EXCL 区均删除并补测：ops 的 `/ops/...` 与裸 `/ops` 复合前缀判定由 OpsDefaultTraversalDecodeAndUnknown 用 GET 实测（gcov：首臂 true、次臂 true、次臂 false 三向 arc 全覆盖），commit 4454a56；force_remove 的 actor_call_timeouts 清理是 exit() 已验证同形的孪生逻辑，新增 ForceRemoveCancelsPendingCallTimeouts（未完成外呼 + 牺牲服务耗尽预算），gcov 实测 3527 行 a-true/a-false/b-false 与 else 体 `++it` 全部命中，commit 2317ec9（三套 lua_service 套件全绿 rc=0）。
- [x] 审核发现：ForceRemoveCapsRecentlyExitedTombstones 仅断言服务列表为空，并未验证墓碑集合是否受限或清空，因此即使上限逻辑失效测试仍会通过（tests/coverage/test_cov_lua_service2.cpp:659）。
  > 新增 recently_exited_count()/recently_exited_contains() 可观测访问器（与 active_actor_timer_count 同风格），断言 4112 次移除触发一次清空后恰好保留 16 条：末移除的 ids.front() 在集合内、首移除的 ids.back() 已被清空淘汰。实测该用例 61s 通过，count==16 与双向成员断言全绿，commit e216836。

## 源码分层分析 2026-11-07

### 抽象层清单

- 基础值与标识：提供无依赖 ID/时间/Result/Error 基础类型（include/shield/base/base.hpp:5）。
- 可观测诊断：提供日志 sink、崩溃 dump 采集和上传留位（src/crash/crash.cpp:31）。
- 配置总线：加载、校验并暴露进程级运行时配置（src/config/config.cpp:1111）。
- 进程装配：按序组合、失败回滚并关停全部子系统（src/bootstrap/bootstrap.cpp:402）。
- 消息契约：定义本地服务与跨节点 CAF 消息形状（include/shield/core/service_message.hpp:46）。
- Lua 服务域：串行执行服务 VM、协程、调用和生命周期（src/lua/lua_service.cpp:2884）。
- 客户端网关：执行接入准入、协议解析和单目标 session 路由（include/shield/net/listener.hpp:76）。
- 可插拔能力：编排 manifest、ABI、实例、binding 与插件生命周期（src/plugin/plugin_host.cpp:1066）。
- 后端提供者：持有外部 Redis/SQL/文档/协议/运维资源的连接与操作（plugins/cache.redis/shield_cache_redis.cpp:969）。
- 全局运行态：承载跨服务数据、锁、排行、队列、调度和限流（src/global/global_manager.cpp:319）。
- 多节点通信：维护节点发现、认证心跳、路由快照和远端 envelope（src/cluster/cluster_transport.cpp:889）。
- 玩家索引：维护玩家准入、重连、设备与服务定位（src/player/player_manager.cpp:102）。
- 服务器状态：维护进程状态机、watcher 和关停请求（src/server/server_manager.cpp:60）。
- 运维入口：暴露只读 HTTP/console 快照和显式启用的控制动作（src/console/ops_http_handler.cpp:805）。
- 管理端应用：轮询认证运维快照并呈现浏览器页面（console/src/services/api.ts:14）。

### 超大文件候选

- [ ] LAY-001 [P1] 拆分 src/lua/lua_service.cpp（6966 行）——服务 actor、协程、timer、profile、async plugin、命名与强移治理耦合在同一实现（src/lua/lua_service.cpp:360）。
- [ ] LAY-002 [P1] 拆分 src/lua/lua_api.cpp（5273 行）——service、message、timer、client、http、crypto、plugin 与可选模块 facade 共享一个注册文件（src/lua/lua_api.cpp:5145）。
- [ ] LAY-003 [P1] 拆分 include/shield/lua/binding.hpp（2767 行）——绑定桥接与各能力 facade 的模板实现集中在公共头（include/shield/lua/binding.hpp:1）。
- [ ] LAY-004 [P1] 拆分 src/lua/lua_runtime.cpp（2598 行）——VM 策略、loader、handler 编译和多种安全重入机制集中在一个 runtime 文件（src/lua/lua_runtime.cpp:1）。
- [ ] LAY-005 [P1] 拆分 src/console/ops_http_handler.cpp（2173 行）——内嵌 dashboard、路由、认证、指标、SPA 和 eval/profile 控制混在一个 handler（src/console/ops_http_handler.cpp:805）。
- [ ] LAY-006 [P1] 拆分 plugins/mysql/shield_db_mysql.cpp（2077 行）——连接池、SQL 类型映射、Lua proxy、async worker、事务 lane 与生命周期同文件实现（plugins/mysql/shield_db_mysql.cpp:1796）。
- [ ] LAY-007 [P1] 拆分 plugins/mongodb/shield_doc_mongodb.cpp（2026 行）——文档 CRUD、聚合、索引、事务、错误映射和 Lua proxy 耦合在同一 provider（plugins/mongodb/shield_doc_mongodb.cpp:1861）。
- [ ] LAY-008 [P1] 拆分 src/transport/protocol.cpp（1893 行）——envelope、codec、路由、descriptor 编译和外部 codec 绑定集中实现（src/transport/protocol.cpp:1622）。
- [ ] LAY-009 [P1] 拆分 plugins/postgresql/shield_db_pgsql.cpp（1853 行）——连接池、SQL 执行/重写、Lua proxy、async worker 与事务状态耦合（plugins/postgresql/shield_db_pgsql.cpp:1264）。
- [ ] LAY-010 [P1] 拆分 src/bootstrap/bootstrap.cpp（1823 行）——CLI、装配、可选模块、gateway、console、HTTP、失败清理和关停顺序在一个入口（src/bootstrap/bootstrap.cpp:402）。
- [ ] LAY-011 [P1] 拆分 src/config/config.cpp（1818 行）——锁式存储、schema 校验、可选模块门禁和 runtime accessor 混在同一文件（src/config/config.cpp:1115）。
- [ ] LAY-012 [P1] 拆分 src/global/global_manager.cpp（1550 行）——data/lock/rank 委托与队列、调度器、限流状态机集中实现（src/global/global_manager.cpp:1196）。
- [ ] LAY-013 [P1] 拆分 plugins/sqlite/shield_db_sqlite.cpp（1473 行）——本地数据库驱动、mapper、Lua proxy、async shim 和生命周期同文件承载（plugins/sqlite/shield_db_sqlite.cpp:1）。
- [ ] LAY-014 [P1] 拆分 src/plugin/plugin_host.cpp（1327 行）——manifest 编排、ABI 守卫、host API、依赖解析、Lua 钩子和生命周期集中（src/plugin/plugin_host.cpp:1066）。
- [ ] LAY-015 [P1] 拆分 plugins/leaderboard.redis/shield_leaderboard_redis.cpp（1166 行）——Redis 协议封装、复合分数、结果映射、Lua proxy 与实例生命周期耦合（plugins/leaderboard.redis/shield_leaderboard_redis.cpp:525）。
- [ ] LAY-016 [P1] 拆分 plugins/redis.driver/shield_redis_driver.cpp（1116 行）——连接池、Redis 命令、pipeline、Lua 代理与生命周期在一个驱动实现（plugins/redis.driver/shield_redis_driver.cpp:1060）。
- [ ] LAY-017 [P1] 拆分 plugins/queue.redis/shield_queue_redis.cpp（1108 行）——stream 队列、消费组、订阅线程、driver fallback 与 Lua proxy 耦合（plugins/queue.redis/shield_queue_redis.cpp:344）。
- [ ] LAY-018 [P1] 拆分 src/cluster/cluster_transport.cpp（1102 行）——握手、心跳、路由同步、envelope 桥接和 CAF 连接生命周期集中（src/cluster/cluster_transport.cpp:554）。
- [ ] LAY-019 [P1] 拆分 plugins/cache.redis/shield_cache_redis.cpp（1011 行）——driver/内置 Redis 双路径、KV/hash/TTL、Lua proxy 与池统计耦合（plugins/cache.redis/shield_cache_redis.cpp:41）。

### 目录/分类不合理处

- `src/core` 只有 CAF 初始化和消息类型，名字暗示服务核心；服务注册、dispatch 和生命周期实际位于 `src/lua/lua_service.cpp:360`，抽象核心契约与执行实现分居两个目录。
- `src/shield_base` 是唯一带前缀的源码目录；其他模块均无 `shield_` 前缀，且基础类型声明集中在 `include/shield/base/base.hpp:5`，命名模式不一致。
- `include/shield/core/service_message.hpp:1` 定义 `namespace shield::lua` 类型；core 头承载 Lua 命名空间契约是为了打破依赖，但路径与命名空间、服务实现所在层不一致。
- `src/plugin/schema_validator.hpp` 是与实现同目录的私有头，而其他模块契约集中放在 `include/shield/`；插件公共宿主 API 与内部校验器的可见性边界混在同一目录类型。
- 仓库根残留大量 `.gcov` 覆盖产物（例如 actor.hpp.gcov:1），与 `.cache/`、`build/`、`Testing/` 生成物混在源码工作区，掩盖源码清单。`build-cov/` 下的 `coverage.txt` 为本地覆盖率报告生成物，不应纳入提交；如需保留权威报告，请放入 `build-cov/` 并在 `.gitignore` 中登记。
