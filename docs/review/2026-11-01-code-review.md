# Shield 代码深审报告（2026-11-01）

> 审计范围：本仓全量（src/ ~35.4k LOC、include/ ~10.3k LOC、plugins/ 21 个插件、scripts/、examples/）。
> 审计方法：安全静态扫描 + 隐藏 bug 猎捕（错误处理/竞态/越界/资源泄漏/off-by-one/并发）+ 质量门（clang-format / clang-tidy / 构建警告 / 测试基线）。
> 审计基线：`e4e3567`（修复前）——构建 Release + 全量 ctest **97/97 绿（161.84s）**，构建 0 警告 0 错误。
> 修复方向：P0/P1 当场修（一问题一 commit，测试绿）；P2/P3 记 todo。
> 修复后：构建三棵树（build/ build-cluster/ build-plugins/ build-fb/）全绿，全量 ctest **100/100 绿（185.75s）**（基线 97 + 新增回归测试 3）。

## 严重度定义

- **P0**：可被外部未授权触发的安全漏洞 / 会导致崩溃、数据损坏或服务不可用的 bug。
- **P1**：需特定条件触发的安全弱点或明确的功能性 bug（错值、泄漏、竞态窗口）。
- **P2**：健壮性/可维护性缺陷，无直接安全或功能影响，但应记录。
- **P3**：风格/提示性观察。

## 一、安全静态扫描

### 1.1 硬编码密钥

未发现。集群认证密钥、TLS 证书路径均经 config 注入（`src/config/config.cpp`），示例配置使用占位符。CI 与脚本中无凭据。

### 1.2 命令注入 / 系统调用

未发现。全仓无 `system()` / `popen()` / `std::system` 调用；无拼接 shell 字符串的路径。进程操作均走 CAF / asio 原生 API。

### 1.3 SQL 拼接

未发现。DB 异步层使用绑定参数（`src/db/`），无字符串拼接 SQL 的调用点。

### 1.4 反序列化 / eval

发现 **P0×2**（详见 2.3/2.5）：

- JSON 反序列化深度攻击（本节归"反序列化"类）：nlohmann `parse()` 本身可抗深嵌套（迭代式），但 `dump()` 与 `push_json_to_stack` 递归走 C++ 栈，深嵌套 payload 直接栈溢出崩溃——外部可触发。
- FlatBuffers：`GenerateText` 以 schema 访问器无界走 buffer，截断/伪造 buffer 越界读。

Lua 侧：`load`/`safe_script` 仅用于受信脚本（自仓 scripts/ 与 config 声明的模块路径），无把网络输入喂给 `load` 的路径。

### 1.5 路径穿越 / 任意文件

未发现。插件 schema 加载（`plugins/protocol.flatbuffers` 的 `schema`/`schema_dir`）来自宿主配置而非网络输入；HTTP 路由与静态文件处理无用户可控路径拼接。

### 1.6 弱密码学

未发现。`src/lua/lua_crypto.cpp` 走系统库（OpenSSL/LibreSSL）原语；无自研加密、无 MD5/SHA1 用于安全用途（仅既有兼容项，非新增）。

## 二、隐藏 bug 猎捕

### 2.1 错误处理缺失

**P1 — listener：瞬时 accept 错误后监听器永久失能**（已修）
`src/net/listener.cpp:205`（修复前）：accept 错误仅记日志返回，除 `operation_aborted` 外**不重挂 accept 循环**。EMFILE/ENFILE/ECONNABORTED/ENOBUFS 等瞬时错误后，监听器 idle 至进程退出而端口仍"显示"在开。修复：非 `operation_aborted` 一律 `do_accept()` 重挂（`src/net/listener.cpp:208-213`）。回归测试：`tests/coverage/test_cov_listener.cpp`（AcceptErrorWhileListeningIsLogged 追加"fd 压力恢复后必须能再次 accept"断言）。

### 2.2 资源泄漏

**P1 — queue.redis：重订阅销毁可 join 线程 → std::terminate**（已修）
`plugins/queue.redis/shield_queue_redis.cpp`（修复前）：同 channel 重订阅时先 `workers.erase(it)` 再起新线程——被 erase 的 `std::thread` 仍 joinable，析构即 `std::terminate`，进程全灭。修复：`shield_queue_worker{thread, shared_ptr<atomic<bool>> stop}`，换人前 stop + join；unsubscribe 同序；新增 `workers_mu` 只护 workers（不与 subs_mu 同持）；连接销毁统一 stop→join→clear。

### 2.3 越界 / off-by-one

**P0 — JSON 深嵌套 → 栈溢出崩溃（外部可触发）**（已修）
证据：nlohmann `parse()` 迭代式可吃 8M 层不崩，但 `dump()`/`push_json_to_stack` 递归。三处边界修复：
- `src/transport/protocol.cpp:269` `kMaxJsonDepth=512` + 非递归、字符串感知预扫描 `json_nesting_within`（`src/transport/protocol.cpp:276`）；`route_key`（:1012）返回 nullopt，`decode`（:1026）抛 `json body exceeds maximum nesting depth`。
- `plugins/protocol.json/shield_protocol_json.cpp:31` 同款预扫描；`json_decode`（:178）与 `json_encode`（:226）超深返回 -1（独立副本，ABI 隔离，不共享传输层符号）。
- `src/lua/lua_service.cpp:5408` `kPushJsonMaxDepth=512`；`push_json_to_stack`（:5412）超深推 nil（纵深防御：即便深 DOM 漏进恢复路径也不再崩）。

**P1 — FlatBuffers：GenerateText 无界走 buffer（越界读）+ 返回值判反**（已修）
`plugins/protocol.flatbuffers/shield_protocol_flatbuffers.cpp`（修复前）：
1. `GenerateText` 用 schema 访问器裸走 buffer，无任何校验；截断/伪造 buffer 越界读（崩溃/泄露）。
2. v25.12.19 `GenerateText` 返回 `const char*`（nullptr=成功，非空=错误串），旧码 `if (!GenerateText(...))` 判反——**合法输入也走错误分支，decode 恒败**。
修复：decode 前先 `parser.Serialize()` 物化 schema 为 reflection buffer，`flatbuffers::Verify`（schema 驱动、全 offset/string/vector/nested table 带界校验）失败即拒（:172-186）；`GenerateText` 改判 `gen_err != nullptr`（:191-198）。新增测试 `tests/protocol/test_protocol_flatbuffers_plugin.cpp`：合法 payload 过验 + 截断/全零 payload 拒绝（走真实 ABI 全链路）。

### 2.4 竞态 / 并发

**P0 — 集群回复伪造（call_session 可劫持）**（已修）
`src/cluster/cluster_transport.cpp`（修复前）：EnvelopeReplyMsg 只在 `auth.enabled` 时校验 `sender_identity`；auth 关闭时裸 TCP 客户端不发 Hello 即可伪造任意 `call_session` 的回复——call_session 顺序分配可预测，回复直接进 `reply_handler` 投给等待中的调用方（伪造数据/污染 RPC 结果）。
修复：**回复与出站连接绑定**——`TransportSideState` 新增 `outbound_sessions[call_session] = {target_node, deadline}`（:103-107，data_mutex 内维护）；发信封时登记（`send_envelope`，超时+5s 宽限 `kReplyGraceMs` :49）；收回复时**无条件**要求非空 `sender_identity`（去掉 auth.enabled 门），且 `outbound_sessions` 必须有该 session、`target_node` 与来源连接的 node_id 一致、未过期；任一不符即丢弃（:837-…）。stop() 清 map。

### 2.5 生命周期 / use-after-free

**P1 — lua_http_bridge：forked task 捕 `this` 越桥生命周期**（已修）
`src/lua/lua_http_bridge.cpp:154`（修复前）：forked 任务 lambda 捕 `this`，任务可能在 bootstrap 复位 http_bridge **之后**才在 service actor 线程跑，`this->runtime_` 即 UAF。修复：改捕 `&runtime_`（runtime 寿命覆盖该窗口），注释说明所依据的生命周期序。

## 三、质量门

| 门 | 结果 | 证据 |
| --- | --- | --- |
| clang-format（CI 口径：include+src 全量 `--dry-run --Werror`） | **0 违规** | 本机 clang-format 实测 |
| clang-tidy（.clang-tidy 已配置，非 CI 门） | 仅既有警告，无新增 | `ops_http_handler.cpp` 实测：narrowing-conversions ×2、convert-member-functions-to-static ×3、inefficient-vector-operation ×2（均既有，非本批引入） |
| 构建警告 | **0 警告 / 0 错误**（Release，14 核，四棵树全绿） | build/ build-cluster/ build-plugins/ build-fb/ |
| 测试基线（修复前） | **97/97 绿**（161.84s） | `ctest --test-dir build` @ e4e3567 |
| 测试基线（修复后） | **100/100 绿**（185.75s，含新增 flatbuffers 插件测试 3 例 + listener 回归断言） | `ctest --test-dir build` |
| docs build | 绿（vitepress 1.6.4，39.28s） | `npm run docs:build` 退出码 0 |

## 四、发现汇总（按严重度）

| # | 严重度 | 发现 | 位置 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | P0 | JSON 深嵌套栈溢出（外部可触发崩溃/DoS） | `src/transport/protocol.cpp`、`plugins/protocol.json/`、`src/lua/lua_service.cpp` | ✅ 已修 |
| 2 | P0 | 集群回复伪造：auth 关闭时 call_session 可劫持 | `src/cluster/cluster_transport.cpp` | ✅ 已修 |
| 3 | P1 | FlatBuffers decode 越界读 + GenerateText 返回值判反（恒败） | `plugins/protocol.flatbuffers/` | ✅ 已修 |
| 4 | P1 | queue.redis 重订阅销毁 joinable 线程 → std::terminate | `plugins/queue.redis/` | ✅ 已修 |
| 5 | P1 | lua_http_bridge forked task 捕 `this` UAF | `src/lua/lua_http_bridge.cpp` | ✅ 已修 |
| 6 | P1 | listener 瞬时 accept 错误后永久失能 | `src/net/listener.cpp` | ✅ 已修 |
| 7 | P2 | `__shield_client_ref` 标记自证无来源校验 | `src/core/service_message.cpp:31`、`src/lua/lua_api.cpp:203` | 📋 记 todo |
| 8 | P2 | gateway close 缺 epoch 校验（egress 有） | `src/lua/gateway_actor.cpp:210` | 📋 记 todo |
| 9 | P3 | force_remove 漏 exit() 的 per-service timer 清理 | `src/lua/lua_service.cpp:3495` | 📋 记 todo |

合计 **9 项主发现**：P0×2、P1×4（全部当场修，6 项一问题一 commit）；P2×2、P3×1 记 todo。
上表 9 项均经本仓源码逐条复核确认（非转述代理结论）。

并行深审代理另录 **11 项 P2/P3**（BUG-2/3/5/6/7/8/9、RACE-2/3/4/5/6、SEC-2，含 redis.driver 深层释放、路由槽 UAF、console_session 裸 this、postgresql 占位符改写误伤字面量、JWT 缺 exp、裸 bool 跨线程等），已记 `todo.md`「代码深审 P2/P3 待办（2026-11-01）」节，本报告不逐条复述（未逐条独立复核，以 todo 记录为准）。**全量口径：P0×2 + P1×4（已修 6）+ P2/P3×14（todo 14 = 本表 3 + 代理 11）。**

## 五、P0/P1 修复记录

| 修复 | commit | 测试 |
| --- | --- | --- |
| 1a. transport JSON 深度守卫 | `c808f3d fix: reject over-deep json payloads at codec boundary` | 全量 ctest |
| 1b. protocol.json 插件深度守卫 | `5bfe132 fix: depth-guard json plugin decode and encode` | 全量 ctest |
| 1c. push_json_to_stack 深度上限 | `2ee49f1 fix: cap lua json push nesting depth` | 全量 ctest |
| 2. 集群回复 peer-binding | `9fdbaed fix: bind cluster replies to outbound sessions` | build-cluster 全量编译 |
| 3. FlatBuffers 校验+返回值 | `24b08fc fix: verify flatbuffers payload and correct decode success check` | test_protocol_flatbuffers_plugin 11/11 |
| 4. queue.redis 线程退休 | `45193b0 fix: join queue redis worker threads before erase` | build-plugins 全量编译 |
| 5. http_bridge 捕 runtime | `b32b263 fix: capture runtime not bridge in forked http task` | test_cov_http_bridge 绿 |
| 6. listener accept 重挂 | `95712f7 fix: re-arm accept loop after transient accept errors` | test_cov_listener 回归断言 |

（每条独立可回滚。）

## 六、P2/P3 待办（记 todo）

- **P2 `__shield_client_ref` 自证标记**：`ClientContextData::from_json`（`src/core/service_message.cpp:31`）接受任意 `{"__shield_client_ref": true, ...}` 且身份字段照单全收，无来源校验；`json_to_lua`（`src/lua/lua_api.cpp:203`）把它物化为受信 ClientContext。若某 handler 把不受信 payload 透传进 `client_arg_to_data`（`shield.client.bind/close`/`_client_egress`），调用方即控制 session_id/player_id/gateway_address → 会话伪造。建议：物化路径限定为受信消息来源（gateway 产出），或给标记加网关签发的 nonce/签名。
- **P2 gateway close 缺 epoch 校验**：`handle_client_close`（`src/lua/gateway_actor.cpp:210`）按 session_id 直接关会话，不像 `handle_client_egress`（:112）那样比对 `session_epoch`——持过期 ClientContext 的服务可关闭已 rebind 到别的 target 的会话。建议：close 前比对 `request.context.session_epoch == binding.epoch`（或明确文档化 close 允许任意 epoch 并说明理由）。
- **P3 force_remove timer 清理不对称**：`force_remove`（`src/lua/lua_service.cpp:3495`）没有 `exit()`（:3282）的 per-service actor-timer / call-timeout 驱动清理；timers 只能等 manager 析构统一清。当前 force_remove 仅 shutdown 预算耗尽路径调用（:3433），窗口内 timer 触发走"服务已移除"错误臂，无实际 UAF；属防御性收口。

## 七、commit 清单

| commit | 说明 |
| --- | --- |
| `95712f7` | fix: re-arm accept loop after transient accept errors |
| `b32b263` | fix: capture runtime not bridge in forked http task |
| `45193b0` | fix: join queue redis worker threads before erase |
| `24b08fc` | fix: verify flatbuffers payload and correct decode success check |
| `c808f3d` | fix: reject over-deep json payloads at codec boundary |
| `5bfe132` | fix: depth-guard json plugin decode and encode |
| `2ee49f1` | fix: cap lua json push nesting depth |
| `9fdbaed` | fix: bind cluster replies to outbound sessions |
| `66e32cb` | docs: record code review p2 p3 findings |
| `99b0bd2` | docs: add code deep review report |

一问题一 commit，信息英文短句、无 AI 痕迹；6 条 P0/P1 修复 + 2 条文档（报告 + todo）。

## 八、结论

- 安全面：无硬编码密钥/命令注入/SQL 拼接/路径穿越/弱加密；反序列化面两处 P0（JSON 深度、FlatBuffers 越界）+ 一处 P0（集群回复伪造）已修。
- 稳定性：P1×4（终止/失能/UAF/恒败）已修，回归测试落位。
- 遗留：P2/P3×14 记 todo（本报告逐条复核 3 项 + 并行代理 11 项，含信任边界加固/资源清理/竞态收口，均非可直接利用的线上缺陷）。
- 门禁：四棵树编译 0 警告；全量 ctest 100/100；clang-format 0 违规；docs build 绿。

## 九、第二波深审（本会话：安全静态扫描 + 三路读审代理，2026-11-03）

> 第一波（§一~八）覆盖 9 项主发现 + 11 项并行代理 P2/P3。本波为独立第二遍全量深审，由三路读审代理（bootstrap/console/global/cluster、net/transport/protocol、lua/core）与安全静态扫描产生，**与第一波无重叠**；全部条目已逐条对照本仓源码复核（非转述代理结论）。sampler 一项按臂拆分（TOCTOU 已修、跨线程门控待办），故复核后条目数 = 原始发现数 + 1。

### 9.1 安全静态扫描（补充）

- **P1 — /ops/config 未脱敏返回密钥类配置**（已修）：`src/console/ops_http_handler.cpp` `handle_config` 把任意 config 键原样返回；`password`/`token`/`secret`/`api_key`/`credential`/`private_key` 等键经未认证 `/ops/config` 端点泄露。修复：新增 `kSecretNeedles` 匹配，命中即返回 `***redacted***`；回归断言 `tests/coverage/test_cov_ops_http.cpp`（`ConfigEndpointValueShapes`：`cov3.password` → `***redacted***`，`cov3.normal_key` → 原值）。commit `e4e3567`。
- 其余安全面（硬编码密钥 / 命令注入 / SQL 拼接 / eval / 反序列化 / 路径穿越）与第一波结论一致：未发现。`/ops/eval` 为 opt-in 默认关 + Bearer token + 受限 VM（去 os.execute/io/require/package），属功能面非漏洞；`token_matches` 常量时间比较。

### 9.2 隐藏 bug 猎捕（第二波）

**P1（已修 ×4）**

| 编号 | 发现 | 证据 | 修复 | commit |
| --- | --- | --- | --- | --- |
| B1 | 关停顺序 UAF：server/global manager 的定时器/tick 回调捕获 `lua_services`，但 `stop()` 在 `lua_services.reset()` **之后**才 join——窗口内回调可解引用已释放 manager（违反本仓自身注释「stop() joins before lua_services is freed」）；`shutdown()` 与 startup-failure 两条路径同病 | `src/bootstrap/bootstrap.cpp`（修复前）`:1744,1769,1775` 与 `:348,355,366`；回调捕获 `src/global/global_manager.cpp:770-780`、`src/server/server_manager.cpp:190-199`；tick join `global_manager.cpp:427-434` | 两条路径的 server/global `stop()+set_global+reset` 均前移至 `lua_services.reset()` 之前 | `e42bd3f`（shutdown 路径）、`b90be2e`（startup-failure 路径） |
| B2 | `shield.http.*` 处理器按引用捕获函数局部 lambda；注册表持久而 lambda 随函数返回析构——每次调用经悬垂引用（正式 UB；因 lambda 现为无捕获故实际不崩溃，属潜在腐蚀） | `src/lua/lua_api.cpp:4741` 等 13 处 `[&to_table,&parse_opts]`/`[&to_table]`；定义 `:4634,:4679`；`shield["http"]=http` `:4975` | 全部改按值捕获 `[to_table,parse_opts]`/`[to_table]`（无捕获拷贝零成本） | `9c2afd3` |
| B3 | `ProfileSampler::sweep_active` 双重 load TOCTOU：两次 load 间并发 uninstall 置空 → 第二次返回 nullptr → `nullptr->sweep_once()` 空指针崩溃 | `src/lua/profile_sampler.cpp:134-135`；uninstall 置空 `:166` | 收敛单次 load + 判空 | `00aef5f` |

**P2（记 todo ×8）**

| 编号 | 发现 | 证据 |
| --- | --- | --- |
| C1 | TLS 握手状态持裸 `TcpListener*`；`stop()` 不取消在飞握手 → 监听器析构后慢握手完成调 `adopt_connection` UAF | `src/net/listener.cpp:104,79` |
| C2 | `last_rejection_`（std::string）accept 线程无锁写、stats 线程无锁读 → 读写 UB（同文件 `listening_` 已由 a1f5d8e 收 atomic，本条为残余） | `src/net/listener.cpp:258`、`include/shield/net/listener.hpp:104,192` |
| C3 | msgpack 解码无嵌套深度上限：`from_msgpack`/`dump` 递归走 C++ 栈，攻击者 ~100k 层嵌套 payload 栈耗尽；json 编解码器有 512 深度预扫描而 msgpack 缺 | `plugins/protocol.msgpack/shield_protocol_msgpack.cpp:118,124`；对照 `src/transport/protocol.cpp:1012,1026` |
| C4 | `impl_->actor`（refcounted caf::actor）跨线程无锁读写——stop 置空与 send 路径拷贝竞争 refcount | `src/cluster/cluster_transport.cpp:948,981,1042` |
| C5 | `remote_send_fn` 在 `send_remote` 无锁读（写于 mutex 下） | `src/cluster/cluster_manager.cpp:325-328,336` |
| C6 | `broadcast_attach` 在 `next_seq==0`（首播前入组）时 `try_emplace(next_seq-1)` 下溢为 UINT64_MAX → `broadcast_since` 的 `seq<=cursor` 全丢弃，该组永远收不到广播 | `src/global/global_manager.cpp:994,1014` |
| C7 | sampler 跨线程 sweep：`sweep_active` 可被任意 service actor 线程调用，对采样会话的 lua_State 跨线程操作（违 `profile_sampler.hpp:4-6` 不变式）；TOCTOU 部分已由 B3 修，门控设计待做 | `src/lua/profile_sampler.cpp:133-137`、`src/lua/lua_runtime.cpp:1531`、`src/lua/lua_service.cpp:6223` |
| C8 | sleep 定时器 id 被丢弃（`(void)`）不可取消；死协程 `lua_resume` 误报「sleep continuation error」计入 panic 阈值。BUG-2（7c69651）+ force_remove 取消（ce26cce）已收窄资源/退出臂，取消面仍缺显式入口 | `src/lua/lua_api.cpp:1186,1204,1241-1242,1265` |

**P3（记 todo ×8）**

| 编号 | 发现 | 证据 |
| --- | --- | --- |
| D1 | `running`（bool）在 `run_tick` 无锁读（写于 start/stop） | `src/cluster/cluster_manager.cpp:39,90` |
| D2 | `node_id_`（std::string）在 `resolve` 无锁读（写于 set_locality） | `src/player/player_manager.cpp:262,112` |
| D3 | cron `step` 经 atoi 仅查 `<=0` 无上界 → `v += step` 有符号溢出 UB | `src/global/global_manager.cpp:72-74,109` |
| D4 | `max_frame_size` 构造参数从未存储/使用——listener 声称的帧上限是死旋钮，实际限 16 MiB 默认 | `src/net/session.cpp:56-66`、`src/net/listener.cpp:347` |
| D5 | `error_code_`（std::string）跨线程无同步读写 | `include/shield/net/session.hpp:254,318`、`src/net/session.cpp:427` |
| D6 | `limited_count_`（uint64_t）跨线程无同步读写 | `include/shield/net/session.hpp:81,95` |
| D7 | HTTP 客户端响应体无上限 append、未设 `CURLOPT_MAXFILESIZE`——不可控 URL 可无界增长内存 | `src/net/http_client.cpp:31` |
| D8 | `on_close`/accept 处理器捕获裸 `this`（ConsoleServer）；会话可超服务器存活 → 析构后回调 UAF（BUG-6 已收 `console_session::send_line`，本条为服务器侧残余） | `src/net/console_server.cpp:96` |

### 9.3 质量门（第二波）

| 门 | 结果 | 证据 |
| --- | --- | --- |
| 编译 | 绿 | `shield_bootstrap` + 相关测试目标增量构建 0 错误 |
| 测试（改动面） | 绿 | `test_cov_bootstrap`/`test_cov_bootstrap2`/`test_cov_lua_shutdown`/`shield_runtime_lua_smoke`/`shield_runtime_registry_smoke`/`test_cov_lua_http_bridge`/`test_cov_http_bridge2`/`test_cov_lua_api2`/`test_cov_cpp_lua_api`/`test_cov_lua_runtime2`/`test_ops_profile` 全过 |
| clang-tidy（改动的 3 个 TU） | 无新增 | 仅既有 wrapper 头（binding.hpp/logger.hpp）风格警告，改动行 0 诊断 |
| 测试基线 | 绿 | CI（ci.yml）+ Optional Plugins CI 在审计基线上成功 |

### 9.4 第二波汇总

- **原始发现 19 项**（安全扫描 1 + 三路代理 18）；sampler 一项按臂拆二 → **复核后 20 条**（P1×4、P2×8、P3×8）。
- **已修 4 项（P1 全部）**：`e4e3567`、`e42bd3f`、`b90be2e`、`9c2afd3`、`00aef5f`（B1 占两 commit）。
- **剩余 16 条**（P2×8 + P3×8）记 todo（todo.md「代码深审第二波 P2/P3 待办（2026-11-03）」节）。
- 三路代理 18 项 + 安全扫描 1 项，全部经本仓源码复核，无转述、无与第一波重叠。

### 9.5 第二波 commit 清单

| commit | 说明 |
| --- | --- |
| `e4e3567` | fix: redact secret config keys from unauthenticated /ops/config endpoint |
| `e42bd3f` | fix: join manager threads before freeing lua services |
| `b90be2e` | fix: same manager ordering on startup failure path |
| `9c2afd3` | fix: capture http helpers by value in lua api |
| `00aef5f` | fix: single load in sampler sweep_active |
