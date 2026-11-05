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
- [ ] FIX-WEB-12 eval/profile 受控操作页（opt-in 可达、token 不落盘、分支如实展示）
- [ ] FIX-WEB-13 真机浏览器验收与安全门（真实进程 + 真浏览器证据，不得以进程内用例充当）
- [ ] FIX-WEB-14 生产构建与托管收口（CI 构建产物、无秘密检查、文档接入说明）

## 开放审核发现（2026-11-04，待修）
- [ ] 默认未配置 http.ops_auth_token 时只读运维接口全开放，需默认拒绝显式放行（src/console/ops_http_handler.cpp:629）
- [ ] 前端 fetch 无 Bearer 凭据入口，认证开启后浏览器无法使用受保护接口（console/src/services/polling.ts:39）
- [ ] eval/profile 路由未经 ops-wide 认证包装（src/console/ops_http_handler.cpp:716）
- [ ] 静态目录启用时默认处理器未加认证（src/console/ops_http_handler.cpp:697）
- [ ] 未命中路由先查静态再判 API 前缀，ops 子目录可吞未知 /ops/* 的 JSON 404（src/net/http_server.cpp:317）
- [ ] /ops/dashboard 用含查询串的原始 target 比较，`?x=1` 时误 404（src/console/ops_http_handler.cpp:1597）
- [ ] 轮询退避读 effect 初始 state.data，退避不随实际状态切换（console/src/services/polling.ts:123）
- [ ] 服务列表与插件页仅挂载时请求一次，无过期提示（console/src/pages/PluginsPage.tsx:51）
- [ ] 服务详情请求缺取消/序号保护，晚返回覆盖新选择（console/src/pages/ServicesPage.tsx:61）
- [ ] accept 回调持 handshake gate 锁同步触发 on_connect，回调调 stop() 死锁（src/net/listener.cpp:301）
- [ ] SPA 回退未做根目录与符号链接规范化检查，可读目录外文件（src/console/ops_http_handler.cpp:1601）
- [ ] fetchJson 仅查 401，非 2xx 非 error 信封被当成功处理（console/src/services/polling.ts:43）
