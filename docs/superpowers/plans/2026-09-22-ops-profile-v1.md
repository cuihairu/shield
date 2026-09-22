# shield_ops /ops/profile v1 立项计划

> **状态：已立项，未实施**（2026-09-22）。本文档是实施计划；执行时按 Task
> 顺序推进，checkbox 跟踪。前置核实点见 Task 1（Lua 5.5 debug hook 语义）。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 落地 `/ops/profile`：对单个 Lua service 的采样式 CPU 热点分析
（POST start/stop/status/report），外加慢调用追踪（slow call ring）作为报告
附带段。补齐 runtime-ops.md 能力表中 Profile 一行在 ops 端点面的最后缺口。

**Architecture:** 采样器 = Lua debug count hook（`lua_sethook` +
`LUA_MASKCOUNT`），**只在目标 service 的 owner 线程装卸与累积**——hook 触发
于 owner 线程的字节码执行中，采样状态 owner 线程私有、零锁。会话装卸复用
L2 inspect 的成熟通道：`LuaServiceManager::enqueue_forked_task`（模板见
`src/console/lua_commands.cpp:175`，bounded wait 回 HTTP 线程）。聚合树在
stop（或 duration 到时自停）时经 `std::promise` 一次性移交。慢调用挂在
`resume_caller`（`src/lua/lua_service.hpp:491`）完成路径，atomic gate 保护
热路径零开销（默认关）。

**Tech Stack:** C++20，Lua 5.5 C API（debug hook），nlohmann::json，现有
`OpsHttpHandler` 路由与安全基线（`token_matches` 常量时间比较），Boost.Test。

**Reference spec:**
- 定位与能力口径：`docs/runtime-ops.md`（能力表 Profile 行、运维端点表
  `/ops/profile` 行、安全基线「profile 和 dump 需要速率限制」「profile 必须显式启用」）
- owner 线程执行链路先例：`docs/ops-lua-console.md` L2 inspect
- 铁律约束：绝不跨线程触碰 `lua_State`（所有 VM 操作必须经 owner 线程）

---

## Scope

**本计划包含：**

- **Phase A：采样式 CPU 热点分析**
  - `ProfileSession`（采样状态 + 聚合树，纯逻辑，可单测）
  - `ProfileSessionRegistry`（全局单会话、owner 线程装卸、duration 自停、
    service teardown 清理）
  - `POST /ops/profile`（`action: start|stop|status|report`）+ 安全门
    （opt-in 不注册路由、Bearer token、速率限制）
  - 集成测试（忙循环 service 真实采样出样本）
- **Phase B：慢调用追踪**
  - `SlowCallRing`（进程级有界环形，mutex 保护，低频写入）
  - `resume_caller` 完成路径耗时打点（atomic gate，配置阈值默认关）
  - 报告 `slow_calls` 段 + `status`/`report` 独立暴露

**明确排除（不进本计划）：**

- 消息延迟剖面（mailbox 入队→出队延迟）：需要给 CAF envelope 加时间戳，
  改动面覆盖全部 atom 与投递路径，另行立项评估。报告中的近似指标
  （`pending_tasks` 等）已由 L1 覆盖。
- 多服务并行采样 / 全进程采样：P0 全局同时只允许一个活动会话，目标单
  service（owner 线程 hook 天然 per-VM）。多服务循环轮采留后续。
- C/C++ 帧采样、系统级 profiler 集成（perf/VTune）：超出 runtime 范畴。
- Lua 层 `shield.profile.*` 业务 API：shield_ops 不定义业务 Lua API
  （runtime-ops.md Public Surface 既有约束）。

## 关键设计决策

1. **采样机制 = count hook，而非时钟采样。** SIGPROF/时钟采样需要暂停或
   闯入 owner 线程读 VM——违反不跨线程触碰 `lua_State` 的铁律。count hook
   由 VM 在执行字节码时主动回调，天然运行在 owner 线程，且只采样"正在
   执行"的栈（挂起在 sleep/call yield 的协程不计入——CPU 热点语义正确）。
   默认 interval = `1e6` 条指令触发一次（可配 `http.profile_interval`），
   单次 hook 成本 = 一次栈展开（`lua_getstack`+`lua_getinfo`，典型栈深
   < 32），按需短时会话下开销可接受；profile 本就是显式启用的诊断行为。

2. **hook 的作用域与协程覆盖（Task 1 实测定案）。** handler 执行跑在
   协程上（M4 协程化）。start 时对 main `lua_State` sethook，并枚举
   live 协程注册表逐个补装（`lua.inspect coroutines` 已有枚举通道）。
   **Lua 5.5 实测新建协程不继承创建者 hook**——因此采样 hook 每 K 次
   触发（默认 K=100）对 live 协程做一次补装扫描（扫描本身在 owner
   线程 hook 本体内运行，开销可忽略；短命协程的采样盲区可接受）。
   **不修改** 任何现有 hook 用户（全仓库当前无 `lua_sethook` 使用，
   已核实，采样器是唯一 owner）。

3. **样本聚合在 hook 内增量进行，stop 时整体移交。** hook 内把当前栈
   （frames: source/linedefined/name/what 的自栈顶向下链）累加进聚合树
   （同前缀共享节点，节点计 hit 数）。owner 线程私有状态 = 无锁。
   栈深截断 64 帧、聚合节点预算 20000（超出并入 `truncated` 计数），
   保证 hook 单次成本有界、报告体积有界。

4. **会话生命周期三重收口（对齐 bootstrap-teardown 铁律）：**
   - 正常 stop（action=stop 或 duration 到时定时器触发）：卸 hook（含
     live 协程逐个恢复原 hook/空 hook）、提取报告、擦除 registry 条目。
   - service teardown 时：registry 清理钩子由 `LuaServiceManager` 服务
     退出路径调用（与 lua.snapshot 的 incarnation 清除同位），丢弃会话
     而非悬垂——`report` 语义为"会话不存在"。
   - HTTP 侧 bounded wait（stop/report 2s，同 L2 inspect 惯例）：owner
     忙时返回 `profile dispatch timeout (owner busy)`，不挂死端点。

5. **安全门完全对齐 `/ops/eval` 先例：**
   - `http.profile_enabled != "true"` 时路由**不注册**（请求 404），
     启动日志说明启用方式——与 eval 的 opt-in 模式一致。
   - `http.profile_token` 非空强制（空 token 启动报错且不注册，同
     eval 先例）；请求校验 `Authorization: Bearer <token>`，复用
     `token_matches` 常量时间比较，未授权 401。
   - 速率限制（runtime-ops.md 安全基线）：全局单活动会话（start 时已有
     会话 → 409 `profile session active`）+ start 冷却（默认两次 start
     间隔 ≥ 10s，可配 `http.profile_cooldown_seconds`，冷却内 429）。
   - 报告只含代码位置（source/line/function name）与计数——无 payload、
     无密钥面，符合「不默认输出完整 payload」基线。

6. **慢调用打点默认零开销。** `resume_caller` 是全量 call 的热路径。
   打点 = 进程级 `std::atomic<bool>` gate（首次启用时置位）+ 启用时的
   `steady_clock` 差值与 mutex ring 写入（容量 64，覆盖最旧）。阈值
   `http.slow_call_threshold_ms` 默认 0 = 关闭（gate 不置位，完成路径
   只多一次 relaxed load）。被记录者：caller service、callee service、
   elapsed_ms、ok/failure、时间戳。call 超时（`call_timeout`）路径已有
   失败语义，不重复记录为 slow。

7. **模块归属：跟随现状放 `shield_bootstrap` 的 console 组件。**
   runtime-ops.md 已声明「能力当前编译在 `shield_bootstrap` 而非
   `shield_ops` 空壳 target 内；模块归属对齐是后续工作」——归属对齐是
   独立一刀（涉及 target 搬迁与 CMake 边界），不与功能刀混合。
   `SlowCallRing` 打点虽在 `src/lua/lua_service.cpp`，但它是可读取的
   runtime counters（core 只提供可读取的 counters，不反向依赖 ops——
   ring 头文件落 `include/shield/lua/`，ops 端只是读取方，依赖方向
   core → 被读，合规）。

## File Structure

- **Create** `include/shield/console/profile_session.hpp` —
  `ProfileSample`/`ProfileAggNode`/`ProfileReport`/`ProfileSessionConfig` 与
  `ProfileSession`（纯逻辑：聚合树增采样、报告导出 JSON）。
- **Create** `src/console/profile_session.cpp` — 上述实现。
- **Create** `include/shield/console/profile_registry.hpp` —
  `ProfileSessionRegistry`（进程级单例：会话表按 service 名 + incarnation、
 装卸入口以 `std::promise<ProfileReport>` 回交、teardown 清理钩子）。
- **Create** `src/console/profile_registry.cpp` — 实现 + owner 线程
  hook 安装/卸载辅助（栈展开采样函数即 hook 本体）。
- **Modify** `include/shield/console/ops_http_handler.hpp` +
  `src/console/ops_http_handler.cpp` — `POST /ops/profile` 路由
  （opt-in 注册）、action 分发、鉴权、限速、bounded wait、报告 JSON 信封。
- **Modify** `src/lua/lua_service.cpp`（或 manager 对应文件，实现时按
  teardown 落点定）— 服务退出路径调用 `ProfileSessionRegistry::on_service_exit(name)`。
- **Create** `include/shield/lua/slow_calls.hpp` — `SlowCallRing`
  （header-only，mutex ring + atomic gate + record 结构）。
- **Modify** `src/lua/lua_service.cpp` — `resume_caller` 完成路径打点。
- **Modify** `tests/CMakeLists.txt` — 注册 `test_ops_profile`、
  `test_slow_calls`。
- **Create** `tests/console/test_ops_profile.cpp` — Phase A 全部用例。
- **Create** `tests/lua_api/test_slow_calls.cpp`（落点按现有测试目录惯例
  调整）— Phase B 用例。
- **Modify** `docs/runtime-ops.md` — 端点表 `/ops/profile` 改「已提供」+
  新端点小节（请求/响应示例、安全基线、配置键）；状态行更新。
- **Modify** `docs/roadmap.md` — shield_ops 段 `/ops/profile` 表述从
  「留后续」改为落地口径。

---

## Phase A

### Task 1: Lua 5.5 debug hook 语义核实（spike）✅ 已完成（2026-09-22，lua5.5 5.5.0 实测，脚本验证后已删除）

- [x] **Step 1:** 一次性脚本验证四组语义（结论见 Step 2）。
- [x] **Step 2:** 实测结论（全部以 5.5.0 运行时为准）：

  | 语义点 | 实测结论 |
  |---|---|
  | (a) count hook | 可用；mask `""` + count=N 时触发频率≈每 N 条指令（10000 次循环 count=100 → 208 次采样）；count=0 不触发 |
  | (b1) 新建协程继承 | **不继承**（协程内 co_hits=0）——与 5.4 「新线程继承 hook」的文档口径相反。**设计定案：周期补装兜底**（见决策 2 修订） |
  | (b2) 已存在/未启动协程 sethook | 可行且生效（co_hits=2173） |
  | (b3) 挂起中协程补装 | 可行，resume 后触发（co_hits=4347）——补装方案成立 |
  | (b1-fix) hook 内周期扫描 | 每 K 次触发扫一次 live 协程的方案在 hook 本体内可行 |
  | (c1) 具名函数 name | 正常取得（`name="workloop"`） |
  | (c2) name 缺失场景 | main chunk `name=nil`；**局部赋值的匿名函数可被推断出 name**（`local anon = function()…` → `name="anon"`）——降级场景比预期少 |
  | (c3) 尾调用 | **尾调用帧 `name=nil`** 且 `istailcall=true` 可标注（`getinfo` 的 `t` 选项在 5.5 存在，大写 `T` 无效）——`return foo(...)` 风格下 name 降级是常态 |
  | (d1) hook 内 yield | 被拒绝：`attempt to yield across a C-call boundary`（hook 只做采样记录，绝不 yield/长阻塞） |
  | (d2) hook 内栈枚举 | `lua_getstack`/`getinfo` 全深度枚举完整可用 |

  **对设计的直接影响：** 聚合 key 采用 `what|name?|source:line`（name 缺失
  用 `?` + source:linedefined，为 main chunk 与尾调用帧的常态路径）；帧
  结构附带 `tail`（istailcall）可选标注；hook 单次成本 = 一次受限深度栈
  展开，与既有设计一致。

### Task 2: ProfileSession 纯逻辑（聚合树 + 报告）

**Files:** Create `include/shield/console/profile_session.hpp`、
`src/console/profile_session.cpp`

- [ ] **Step 1:** 数据结构：`ProfileSessionConfig{service, duration_ms,
  interval, max_depth=64, max_nodes=20000}`；
  `ProfileAggNode{key(source|line|name), hits, children}`；
  `ProfileReport{config 镜像, total_samples, elapsed_ms, dropped_samples,
  truncated_frames, root children 树, started_at/stopped_at}`。
- [ ] **Step 2:** `add_sample(frames)`：自栈顶向下同前缀共享累加；节点
  预算超限时样本计入 `truncated_frames`（不丢样本总数，丢展开深度）；
  超深截断（> max_depth）计入 `dropped_samples` 的深度截断子计数。
- [ ] **Step 3:** `to_json()`：报告树按 hits 降序、每节点
  `{source, line, name, hits, pct, children}`；`pct` = hits/total_samples。
- [ ] **Step 4:** 单测（`test_ops_profile.cpp` 前半）：空会话、单栈重复
  累加合并、前缀共享、深度截断、节点预算、JSON 形状与 pct 和≈1。

### Task 3: ProfileSessionRegistry（owner 线程装卸 + 生命周期）

**Files:** Create `include/shield/console/profile_registry.hpp`、
`src/console/profile_registry.cpp`；Modify service teardown 落点

- [ ] **Step 1:** registry：`start(service, config, promise)`——全局单
  会话检查（active → refuse）、 incarnation 记录；`on_service_exit(name)`
  ——擦除会话（正在等 promise 的 stop/report 由 bounded wait 超时兜底，
  registry 擦除时以 `abandoned` 结果 fulfill，绝不悬垂 promise）。
- [ ] **Step 2:** owner 线程装卸（fork task 函数体）：install = 对
  main L sethook + live 协程补装（按 Task 1 结论处理新协程）；每样本
  写 session 的聚合树（owner 线程私有）；uninstall = 恢复原 hook 状态
  （install 时先 `lua_gethook` 保存）、逐协程恢复、聚合树移交 promise。
- [ ] **Step 3:** duration 自停：install 时向该 service 注册一次性
  actor timer（复用 `timer_once` 的 actor timer 通道）触发 uninstall；
  手动 stop 与自停竞态以 registry 单会话状态机仲裁（先到者执行，后者
  no-op）。
- [ ] **Step 4:** teardown 清理接入：服务 exit/respawn 路径调用
  `on_service_exit`（实现时确认与 lua.snapshot incarnation 清除同位）；
  单测：start 后 exit service → registry 空且 promise 收到 abandoned。

### Task 4: HTTP 端点 + 安全门

**Files:** Modify `include/shield/console/ops_http_handler.hpp`、
`src/console/ops_http_handler.cpp`

- [ ] **Step 1:** 路由注册与 opt-in：`http.profile_enabled == "true"`
  且 `http.profile_token` 非空才 `server.post("/ops/profile", ...)`；
  否则启动日志说明原因（两种禁用原因分别成句），路由不注册（404 语义
  同 `/ops/eval`）。
- [ ] **Step 2:** 鉴权与限速：Bearer 校验复用 `token_matches`（401）；
  冷却内 start 429；已有会话时 start 409；`stop` 无会话 409；
  `status` 恒 200（active/false + 剩余时长 + 目标 service）。
- [ ] **Step 3:** action 分发：`start{service, duration_ms(默认 5000,
  上限 60000), interval}` → 经 manager 查 service 存在（404
  `service not found`，口径同 `/ops/services/:name`）→ enqueue fork task
  install；`stop` / `report` → fork task uninstall + `promise.wait_for(2s)`
  → 200 报告或 504 `profile dispatch timeout (owner busy)`；
  `status` 纯 registry 读。
- [ ] **Step 4:** 响应统一 `type`/`data` 信封；错误对象走既有
  `{"type":"error","error":{code,message}}` 形态（与现有端点一致）。
- [ ] **Step 5:** 用例：未启用 404（路由未注册断言）、启用但空 token
  拒绝启动（日志断言）、401（无/错 token）、409（重复 start、无会话
  stop）、429（冷却内）、404（未知 service）、start→status→report
  幸福路径（fixture 忙循环 service，报告 `total_samples > 0`）、
  stop 幂等竞态、owner 忙 504（占住 owner 的慢 task 期间 report）。

### Task 5: 集成测试 + Phase A 验收

- [ ] **Step 1:** 集成用例（真 Lua VM）：service 跑计数忙循环 →
  start(1s) → report 断言：`total_samples > 0`、热点帧落在忙循环函数
  （source/linedefined 匹配 fixture 脚本）、`pct` 降序、空闲对照
  service 采样 `total_samples == 0`（挂起不采样的语义锚）。
- [ ] **Step 2:** 全量验收：build-cov 与 build-plugins 两树重编 +
  全量测试绿；clang-format；覆盖率按 build-cov 口径维持双 100%
  （新增分支全测，不可测臂按 GCOVR 标注规则豁免并写论证）；提交推送，
  等 CI 三平台绿（不打 tag、不发 release）。

---

## Phase B：慢调用追踪

### Task 6: SlowCallRing + resume_caller 打点

**Files:** Create `include/shield/lua/slow_calls.hpp`；Modify
`src/lua/lua_service.cpp`（resume_caller）

- [ ] **Step 1:** `SlowCallRing`：`maybe_record(begin_time, callee, ok)`
  （gate 未置位时一次 relaxed load 返回）；容量 64 环形、`std::mutex`；
  `snapshot()` 返回按时间倒序副本。配置：`http.slow_call_threshold_ms`
  （默认 0 = 关）。gate 由 ops 端点启动时置位（配置非零即置位，进程
  生命周期内不回收——避免 gate 抖动）。
- [ ] **Step 2:** 打点接入：call 挂起点已有 session 注册
  （`suspend_for_call`），在其记录 begin 时戳（仅 gate 置位时）；
  `resume_caller` 完成路径计算 elapsed，超阈值记录
  `{caller, callee, elapsed_ms, ok, at}`。call_timeout 失败路径不记录
  （超时已有独立语义与日志）。
- [ ] **Step 3:** 单测：gate 关闭零记录、超阈值记录、环形覆盖最旧、
  snapshot 一致性、阈值边界（== 阈值记/不记，取「≥ 记录」并写死用例）。

### Task 7: 报告集成 + 文档同步

- [ ] **Step 1:** `POST /ops/profile` 的 `status`/`report` 附带
  `slow_calls` 段（`snapshot()` 前 16 条 + `total_recorded` 累计）；
  slow call 会话与采样会话解耦（无采样会话时 report action 亦可只取
  slow_calls——action 语义：无活动/已完成采样会话时 report 返回仅含
  meta 与 slow_calls 的报告）。
- [ ] **Step 2:** `docs/runtime-ops.md`：端点表行改「已提供」；新增
  「### Profile」小节（四 action 请求/响应 JSON 示例、配置键表：
  `http.profile_enabled/profile_token/profile_interval/profile_cooldown_seconds/
  slow_call_threshold_ms`、安全基线对照、开销与边界声明：按需短时、
  单会话、挂起协程不计入、消息延迟剖面留后续另立项）；
  「可观测性现状」状态行同步。
- [ ] **Step 3:** `docs/roadmap.md` shield_ops 段同步落地口径
  （P0 完成 + 本计划边界）。

### Task 8: Phase B 验收

- [ ] **Step 1:** 全量测试（含新增）两树绿 + clang-format + 覆盖率
  双 100% 维持；提交推送等 CI 三平台绿。
- [ ] **Step 2:** 记忆沉淀：Lua 5.5 hook 继承结论（若与 5.4 文档口径
  有差异）与采样器 owner 线程模式，写入 memory。

---

## Self-Review

- **Spec coverage:** runtime-ops.md 能力表 Profile =「采样式热点分析、
  消息延迟剖面、慢调用追踪」三项中，本计划落热点分析（Phase A）与慢调用
  追踪（Phase B），消息延迟剖面明确排除并声明另立项——能力表行在
  Task 7 同步为三分项的如实口径。安全基线四条（显式启用/速率限制/控制类
  单独权限=token/无 payload 输出）逐条有对应设计（决策 5）。✓
- **铁律核对:** 不跨线程触碰 lua_State（hook 与装卸全在 owner 线程，
  HTTP 线程只经 promise/future）；promise 悬垂收口（abandoned fulfill +
  bounded wait 双保险）；teardown 清理与 snapshot incarnation 同位。✓
- **Placeholder scan:** Task 1 是显式声明的前置核实 spike（继承语义、
  name 覆盖面），不是未设计——其两种结论分支都有既定处理路径；teardown
  接入点标注「实现时按 teardown 落点定」是读代码确认，非设计缺口。✓
- **Type consistency:** `ProfileReport`/`ProfileSessionConfig` 跨 Task 2/3/4
  一致；`SlowCallRecord{caller, callee, elapsed_ms, ok, at}` 跨 Task 6/7
  一致；路由与错误信封复用现有 `OpsHttpHandler` 形态。✓
