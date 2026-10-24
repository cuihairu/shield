# Shield 架构与声明事实核查（2026-10-19）

> 开发者笔记。基线：`6a4aec3`（包含上一轮 `fd1b490`）；核查范围是该提交下的工作树。行号以本提交为准。外部资料单独标注，检索日期：2026-10-19。

## 结论摘要

Shield 的单节点 actor/Lua 主干和客户端 TCP RPC 已有代码与集成测试支撑；Phases 0–4 的路线图条目大体可追到具体实现。README/roadmap 的整体完成口径仍过宽：Phase 5 的 `shield_ops` 虽有运维功能，但 `shield_ops` target 本身是空壳，官方文档又明确标注其未实现。覆盖率是本次实跑项，结论见下文；README 的覆盖率命令与 CI 当前门禁已不一致。UDP/KCP/WebSocket 当前在配置校验处被拒绝，未发现能由正式配置启用的半成品传输路径。

## 1. 声明事实核查

核查口径：一条是可被代码/测试反驳的独立声明；“路线图 Phase 完成”按 Phase 分项计。结果统计在本节末。仅文档勾选不视为实现证据。

| 声明 | 结果 | 代码/测试证据与限定 |
| --- | --- | --- |
| Phase 0：边界/API 契约冻结 | 成立（文档性） | [roadmap.md](../roadmap.md#L39) 列出 core 非目标与 API 契约；CMake 有 core 依赖边界检查：[CMakeLists.txt](../../../CMakeLists.txt#L904)。这是文档/静态边界完成，不证明所有设计决策永久无冲突。 |
| Phase 1：目录/target 收敛 | 部分成立 | target 可见于 [CMakeLists.txt](../../../CMakeLists.txt#L397) 与 [CMakeLists.txt](../../../CMakeLists.txt#L623)；`shield_core` 不接 DB/Redis。物理目录仍保留旧分类，这是路线图明确取舍；`shield_ops` 空壳使模块归属并未收敛。 |
| Phase 2：Lua 服务/API 主路径 | 成立（有界） | CAF service 消息类型见 [service_message.hpp](../../../include/shield/core/service_message.hpp#L36)；coroutine call/timer 的实现和 LAPI 测试入口列于 [roadmap.md](../roadmap.md#L60)。结论只覆盖标注的单节点 API，不等于所有 Lua API 都完整。 |
| Phase 3：入口和配置验证 | 成立（最小范围） | [`shield::run`](../../../src/shield.cpp#L1) 是统一入口；UDP/KCP/WebSocket 配置拒绝见 [config.cpp](../../../src/config/config.cpp#L1338)；CLI/config smoke tests 已注册于 tests CMake。 |
| Phase 4：示例与验收 | 成立（限定范围） | 真实 TCP RPC acceptance 说明其验证 login→bind→move→s2c 且 body 无 route 字段：[test_client_rpc_e2e.cpp](../../../tests/acceptance/test_client_rpc_e2e.cpp#L1)。hello-world 不代表全部生产部署形态。 |
| Phase 5：官方 optional modules P0 全落地 | 部分成立 | Cluster/player/server/global 各有实现与测试路径；Ops health/metrics/console/profile 功能也存在，但编在 bootstrap；`shield_ops` target 本身只有 TODO：[CMakeLists.txt](../../../CMakeLists.txt#L941)、[ops_http_handler.cpp](../../../src/console/ops_http_handler.cpp#L1)。配置 owner 文档明确 `ops:` 未接入、运维面由 bootstrap 承载：[optional-modules.md](../optional-modules.md#L315)。因此能力部分落地，模块边界未落地。 |
| README：`src/` 覆盖率 98%+ | 成立（本机复跑） | 全量 106/106 通过后 gcovr 8.6 实测 `src/` 行覆盖率 99%（17,130/17,131 行）；branch/function 未同时过门槛，详见 §1.1。 |
| README：gcc `--coverage` + gcovr 8.6 | 成立 | CMake 确实加 `--coverage`：[CMakeLists.txt](../../../CMakeLists.txt#L240)；CI pin gcovr 8.6。本机安装至 `/tmp/shield-gcovr-8.6` 实际执行 8.6。 |
| CI 每次 push 执行 Coverage | 部分成立 | workflow 仅对 `main` 的 push/PR 触发；Coverage job 对该事件运行全量 ctest，并上传 Codecov：[ci.yml](../../../.github/workflows/ci.yml#L5)、[ci.yml](../../../.github/workflows/ci.yml#L272)。README 把它描述成 coverage-focused suites；实际已不止 `-L coverage`。 |
| CI 行覆盖门槛 98%、branch/function 100% | 成立（CI 配置） | [`ci.yml`](../../../.github/workflows/ci.yml#L338) 同时统计 `src/` 与 `include/shield/`，并配置 98/100/100。CI 配置存在不等于本地这次门禁通过。 |
| Codecov 上传/徽章 | 部分成立 | 上传步骤及 `fail_ci_if_error: true` 可查 [ci.yml](../../../.github/workflows/ci.yml#L358)；徽章只展示上报状态，不是本地实测证据。 |
| `shield_net` Phase 1 TCP | 成立 | TCP listener/session 接入 bootstrap；真实 TCP 客户端闭环见 [test_client_rpc_e2e.cpp](../../../tests/acceptance/test_client_rpc_e2e.cpp#L1)。 |
| UDP deferred | 成立 | 配置存在 `network.udp` 即失败：[config.cpp](../../../src/config/config.cpp#L1340)。未找到该配置通往 UDP listener 的正式构建路径。 |
| KCP deferred | 成立 | `network.kcp` 同一 fail-fast 分支拒绝：[config.cpp](../../../src/config/config.cpp#L1340)。未发现可运行的 KCP transport target。 |
| WebSocket deferred | 成立 | `network.websocket` 同一 fail-fast 分支拒绝：[config.cpp](../../../src/config/config.cpp#L1343)。未发现 WebSocket listener 实现路径。 |
| deferred 项无残留半成品 | 部分成立 | 没有可启用的 listener/target；保留的配置拒绝分支、设计文档和协议层通用 parser 是边界/基础设施，不等于完整协议实现。不能据此声称仓库里零相关代码。 |
| README：ClientContext/ClientRef 未以 userdata 暴露 | **不成立** | README 仍写“not exposed to Lua as a userdata”：[README.md](../../../README.md#L220)；代码注册了两类 userdata，公共契约亦定义：[architecture.md](../architecture.md#L184)、[lua-api.md](../lua-api.md#L986)。 |

### 1.1 覆盖率与测试实跑

执行环境：`pwd` 为 `/home/cui/workspaces/shield`；Linux/GCC；工作区已有 `build-cov-local` 覆盖率构建缓存，cache 明确显示 `SHIELD_ENABLE_CLUSTER/GLOBAL/PLAYER/SERVER=ON`（CI 默认树这些开关默认 OFF），故本次报告不能冒充 CI 默认开关矩阵。README 命令所需 `VCPKG_ROOT` 原先未设置；本机找到 `/home/cui/vcpkg` 后继续。`gcovr 8.6` 安装在临时目录。

```text
export VCPKG_ROOT=/home/cui/vcpkg
cmake -B build-cov-local -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DSHIELD_BUILD_TESTS=ON -DSHIELD_BUILD_EXAMPLES=OFF -DSHIELD_ENABLE_COVERAGE=ON -DSHIELD_ENABLE_CRASHPAD=OFF
# 结果：配置成功；缓存原有 optional 开关仍为 CLUSTER/GLOBAL/PLAYER/SERVER=ON。
cmake --build build-cov-local --parallel 8
# 结果：232/232 build steps 完成，退出码 0。
ctest --test-dir build-cov-local --build-config Debug --output-on-failure
# 结果：100% tests passed, 0 tests failed out of 106；总用时 266.04s。
ctest --test-dir build-cov-local -L coverage --output-on-failure
# 结果：100% tests passed, 0 tests failed out of 53；总用时 278.64s。
cd build-cov-local && PYTHONPATH=/tmp/shield-gcovr-8.6 python3 -m gcovr -r . --exclude-directories '^\.(?!$)' --filter '\.\./src/' --fail-under-line 98 --fail-under-branch 100 --fail-under-function 100
# 结果：src TOTAL 17131 lines / 17130 executed / 99%；line 门槛通过；branch 58.1% 与 function 99.9% 未过此 README 命令的门槛，退出码非 0。
cd build-cov-local && PYTHONPATH=/tmp/shield-gcovr-8.6 python3 -m gcovr -r . --exclude-directories '^\.(?!$)' --filter '\.\./src/' --filter '\.\./include/shield/' --txt /tmp/shield-coverage-ci.txt --txt-metric branch --exclude-unreachable-branches --exclude-throw-branches --gcov-ignore-parse-errors negative_hits.warn --fail-under-line 98 --fail-under-branch 100 --fail-under-function 100
# 结果：line 门槛通过；branch 99.3%、function 99.1%，两个 100% 门槛未过，退出码非 0。此结果来自上列 optional modules ON 的复用缓存，不代表 CI 默认树。
```

CI 与 README 的实质差异：CI 在覆盖率树中运行全量 `ctest`，不是 `ctest -L coverage`；CI 把 `include/shield/` 加入覆盖统计，README 当前命令只 filter `src/`；README 的 gcovr 参数也未体现 CI 的 branch 排除与 `negative_hits.warn` 解析策略。故即使 `src/` 本机结果达标，也不能用它替代当前 CI 完整门禁结果。

**声明统计（17 条，按上表逐行）**：成立 11 条 / 部分成立 5 条 / 不成立 1 条。成立例：TCP Phase 1 由真实 TCP acceptance 测试锚定（[test_client_rpc_e2e.cpp](../../../tests/acceptance/test_client_rpc_e2e.cpp#L1)）；部分成立例：Ops 功能可用但 `shield_ops` 是空 target（[CMakeLists.txt](../../../CMakeLists.txt#L941)）；不成立例：README 称 ClientContext/ClientRef 未以 userdata 暴露，与 binding/API 文档直接冲突（[README.md](../../../README.md#L220)、[architecture.md](../architecture.md#L184)）。

## 2. 上轮评审闭环（`fd1b490`）

上一轮文档末尾列出的主要风险、能力建议、文档漂移逐项追踪。已修是有对应实现/提交；部分修复只按覆盖范围记为部分，不将 DB 的 SQLite/MySQL/PostgreSQL 异步化外推至 Redis/MongoDB。

| 上轮问题 | 状态 | 当前证据 |
| --- | --- | --- |
| 无 frame 上限 | 已修（`7b395c2`） | 0 表示默认 16 MiB：[protocol.hpp](../../../include/shield/transport/protocol.hpp#L119)。 |
| TCP_NODELAY 缺失 | 已修（`7b395c2`） | session 接入时 best-effort 设置：[session.cpp](../../../src/net/session.cpp#L91)。 |
| DB ABI 同步阻塞 | 部分修（`dc2700a`, `46634d5`, `d2b68ef`, `10133a1`） | C vtable 保持同步 primitive，但 Lua SQLite/MySQL/PostgreSQL 可协程等待 worker；Redis/Mongo/global 路径不因此变异步。 |
| sandbox 键无效 | 已修（`7b395c2`） | `allow_io/os` 条件开库并有测试：[lua_runtime.cpp](../../../src/lua/lua_runtime.cpp#L196)。注意仍不是 CPU/内存隔离。 |
| cluster 数据面缺节点认证/来源绑定 | 已修（PSK 挑战认证） | `cluster.auth` 已消费，完成 HMAC 双向挑战后绑定 CAF sender；Envelope 在 dispatch 前拒绝未认证/身份不符。四类入站集成测试覆盖正常、伪造 HMAC、nonce 重放和未握手 peer。认证默认关闭并告警；mTLS、流量加密及热轮换仍推迟，见 [服务间认证](../runtime-security.md#服务间认证)。 |
| 外部存储可能阻塞 service actor | 未修 | Redis/global Lua API 仍同步访问后端；例如 [lua_api.cpp](../../../src/lua/lua_api.cpp#L4071)、[global_rank_backend.cpp](../../../src/global/global_rank_backend.cpp#L583)。 |
| 每 service Lua 指令/内存/权限限额缺失 | 未修 | 当前 sandbox 只门控 `os/io`：[lua_runtime.cpp](../../../src/lua/lua_runtime.cpp#L196)；安全文档列为未实现：[runtime-security.md](../runtime-security.md#L12)。 |
| 持久化队列与 scheduler | 未修（按明确范围） | global 文档把 Redis 队列/scheduler 后端留在 Phase 2+：[runtime-global.md](../runtime-global.md#L760)。 |
| Blue-green service replacement 编排 | 未修 | 只有设计/原语，无端到端升级编排：[runtime-lua-vm.md](../runtime-lua-vm.md#L67)。 |
| 架构评估文档仍保留过时 frame/NODELAY/sandbox 风险 | 未修 | [`architecture-review.md`](../architecture-review.md#L17) 汇总与当前实现相冲突。 |
| rate limiter Redis 存储描述错误 | 未修 | security 写 Redis：[runtime-security.md](../runtime-security.md#L128)，global 说明进程内：[runtime-global.md](../runtime-global.md#L760)。 |
| cluster 文档内部发现能力口径矛盾 | 未修（文档） | 静态 peers 口径：[runtime-cluster.md](../runtime-cluster.md#L39)；后段又标 Redis 服务发现已实现：[runtime-cluster.md](../runtime-cluster.md#L176)。应区分实现代码是否接入 bootstrap。 |
| README 称 client context 不暴露 userdata | 未修 | README 与已定义的 ClientContext/ClientRef userdata 冲突：[README.md](../../../README.md#L220)、[architecture.md](../architecture.md#L184)。 |

**闭环统计（13 项）**：已修 4 / 未修 8 / 部分修 1。已修例：`7b395c2` 将 frame cap 改成 16 MiB 默认（[protocol.hpp](../../../include/shield/transport/protocol.hpp#L119)）；新增已修例：cluster PSK 挑战认证与入站 Envelope 身份绑定（[cluster_transport.cpp](../../../src/cluster/cluster_transport.cpp)）；部分修例：DB 异步化目前只覆盖部分 provider/调用形态。

## 3. 对标 Skynet（Skynet 官方资料，检索日期 2026-10-19）

这是仓库功能边界对照，不是性能结论。Skynet 官方 README 自述为多用户 Lua actor framework；机制以下面官方源码为准：[`skynet.lua`](https://github.com/cloudwu/skynet/blob/master/lualib/skynet.lua)、[`skynet_start.c`](https://github.com/cloudwu/skynet/blob/master/skynet-src/skynet_start.c)、[`skynet_timer.c`](https://github.com/cloudwu/skynet/blob/master/skynet-src/skynet_timer.c)、[`skynet/cluster.lua`](https://github.com/cloudwu/skynet/blob/master/lualib/skynet/cluster.lua)、[`service_harbor.c`](https://github.com/cloudwu/skynet/blob/master/service-src/service_harbor.c)、[官方 README](https://github.com/cloudwu/skynet)。

| 机制 | Skynet | Shield | 对照 |
| --- | --- | --- | --- |
| actor/消息 | service handle + mailbox，`send/call/dispatch`；协议由注册的 protocol handler 解包 | CAF actor/mailbox 内部传输，service 用 module table 和 `shield.send/call`；TCP 入站有结构化 `ClientIngress` | 同属 actor/message 思路；Shield 更强调固定 RPC descriptor 与 gateway 单 target。 |
| 定时器 | C timer 队列向 service 投递 timeout；Lua `skynet.timeout/sleep` 通过 coroutine 挂起/唤醒 | CAF delayed message/actor timer 与 Lua coroutine resume | 机制相似，底层调度实现不同。 |
| service 模型 | C service/module 加 Lua `snlua` service；service 有 mailbox/地址，worker pool 消费队列 | 一个 Lua VM 对应一个 Shield service actor；CAF scheduler 可并行不同 actor | 相似的隔离单元；Shield 明确服务调用不能同步跨 VM 重入。 |
| Lua 调度 | `skynet.dispatch_message` 创建/复用 coroutine；call/sleep yield 后由 response/timer resume，fork 队列在 dispatch 后处理 | service handler coroutine-aware call/sleep/timer/fork 由 CAF 消息回投恢复 | 核心协作模型接近；API 和消息契约不兼容。 |
| cluster/gateway | cluster 库与 harbor service 支持跨节点 service address/消息；gate/watchdog 及协议由服务/示例组合 | `node:service` 路由已实现；客户端 Gateway 有 route descriptor、SessionBinding epoch、typed ingress/egress | Shield 客户端边界更强类型化；两者 cluster 皆需独立安全配置审视。 |

**Shield 有、Skynet 官方仓库没有以同样内建契约提供的能力（3 条）**：

1. RPC route descriptor 的静态编译、方向/认证要求校验及按 route 缓存 Lua handler（`docs/roadmap.md:19-23`）。Skynet 的核心是 protocol id 与 dispatch 注册，参考官方 [`skynet.lua`](https://github.com/cloudwu/skynet/blob/master/lualib/skynet.lua)。
2. 插件 manifest/package/instance/binding + 稳定 C ABI host（`docs/plugin-system.md`、`include/shield/plugin/host_api.h`）；Skynet 官方 README/源码采用 service/module 与 Lua/C 扩展模型，未发现等价的统一 manifest binding host 契约。此处“没有”限定为官方主仓库的一等内建契约。
3. gateway session epoch CAS 与只读 `ClientContext`/`ClientRef` userdata 防止旧引用写入重连连接（`docs/gateway.md:29-35`）；Skynet 使用 gate/agent/watchdog 组合，但未见 Shield 这套 descriptor+epoch API 的同构内建机制。此为仓库 API 对照，不宣称 Skynet 无法由业务实现。

**Skynet 有、Shield 当前没有或选择不提供的能力（3 条）**：

1. Skynet 的运行时服务模块/协议注册较自由，含 `skynet.register_protocol`、`skynet.dispatch` 与服务动态装载；Shield 客户端 RPC 固定 descriptor 路径，不提供通用 wire protocol 任意注册入口（`docs/gateway.md:37-47`）。
2. Skynet 官方 harbor/cluster 支持其成熟的 harbor 地址/节点通信生态；Shield 当前 cluster 只实现静态 peer/可选 Redis discovery 的自身协议，已支持可选 PSK 认证但尚无传输加密，不能视作 Skynet harbor 兼容替代（`docs/runtime-cluster.md:39-51`、`docs/runtime-security.md:303-312`）。
3. Skynet 支持 `skynet.coroutine` 对原生 Lua coroutine 的封装、fork/wakeup 等自由调度工具；Shield 暴露更受限的 `shield.sleep/call/fork` service 语义，不保证可替换 Skynet 的 coroutine API（官方 [`coroutine.lua`](https://github.com/cloudwu/skynet/blob/master/lualib/skynet/coroutine.lua)；Shield `docs/lua-api.md`）。

对标条数：Shield 有 3 / Skynet 有 3（以上均为边界差异，不按行数或性能计）。

## 4. 缺口与风险（严重度排序）

| 严重度 | 缺口 | 证据 | 最小可证伪验收形态 |
| --- | --- | --- | --- |
| 已修（原高） | 集群 PSK 双向挑战认证及 Envelope 身份绑定已落地 | [cluster_transport.cpp](../../../src/cluster/cluster_transport.cpp)、[服务间认证](../runtime-security.md#服务间认证) | `PskAuthenticatedInboundEnvelope` / `PskForgedHmacRejected` / `PskNonceReplayRejected` / `PskUnregisteredUnhandshakenPeerRejected`；mTLS、传输加密和热轮换不在本轮范围。 |
| 高 | Redis/global 同步 I/O 可阻塞服务 actor | [lua_api.cpp](../../../src/lua/lua_api.cpp#L4071)、[shield_redis_driver.cpp](../../../plugins/redis.driver/shield_redis_driver.cpp#L300) | 用可控延迟 backend 阻塞一次操作，同时向同一 VM 投递 ping；ping 在操作等待期间仍由 coroutine 让出并按 deadline 返回。 |
| 中高 | Lua 只有 os/io 全局开关，无每 service CPU/内存硬预算 | [lua_runtime.cpp](../../../src/lua/lua_runtime.cpp#L196)、[runtime-security.md](../runtime-security.md#L12) | 单 service 无限循环/超额分配被预算机制终止或隔离；其他 service 在固定时限内仍响应。 |
| 中 | `shield_ops` target 空壳，但 roadmap/README 将 optional P0 全部写为落地 | [CMakeLists.txt](../../../CMakeLists.txt#L941)、[optional-modules.md](../optional-modules.md#L315) | `SHIELD_ENABLE_OPS=ON` 构建后能由该 target 提供文档列出的最小健康/metrics 接口测试；否则文档将其降格为 bootstrap 托管能力且调整完成声明。 |
| 中 | Redis 业务限流的存储口径冲突 | [runtime-security.md](../runtime-security.md#L128)、[runtime-global.md](../runtime-global.md#L760) | 用配置/调用测试断言单实例内存状态；文档与实现一致声明“跨进程未实现”。 |
| 中 | cluster 文档同时称仅静态 peers、又称 Redis discovery 已实现，范围不清 | [runtime-cluster.md](../runtime-cluster.md#L39)、[runtime-cluster.md](../runtime-cluster.md#L176) | bootstrap 集成测试以 Redis 临时实例发现一个未列入静态 peers 的节点；否则统一文档标成未接线/非当前路径。 |
| 中 | 旧 architecture-review 和 README 保留过时安全/用户 API 结论 | [architecture-review.md](../architecture-review.md#L17)、[README.md](../../../README.md#L220) | 对照代码更新两处表述；文档检查断言 frame 默认 16 MiB、TCP_NODELAY 开启且 client userdata 声明一致。 |
| 低 | reliable/delayed queue 与 scheduler 未持久化，重启会丢进程内状态 | [runtime-global.md](../runtime-global.md#L760) | 启动临时 Redis，入队后杀进程再启动；消息可按声明的至少一次语义恢复且重复可识别。 |
| 低 | 没有端到端 blue-green service replacement/rollback 流程 | [runtime-lua-vm.md](../runtime-lua-vm.md#L67) | 自动化用例启动 v2、迁移状态、原子切名、drain v1；故意令迁移失败并证明回滚后旧服务仍可接收请求。 |

缺口共 9 条。此处没有提出数据外发或云备份能力；仓库未声明默认数据外发/云备份功能。

## 5. 推进次序（已代决）

1. 先收口事实与模块边界：修复 README、roadmap、architecture-review、runtime-security、runtime-cluster 的口径，并将 `shield_ops` 归属做成可验收的实现/target 或明确从“模块 P0 已落地”移除。
2. cluster 身份认证与来源绑定已完成 PSK 实现；继续推进 mTLS/传输加密与凭证轮换，在此之前不把公网/不可信网段 cluster listener 视为可发布能力。
3. 把 Redis 与 global 同步 I/O 迁到 worker + coroutine resume，并覆盖 MongoDB 与非协程调用者的策略。
4. 为 Lua service 加 CPU/内存硬限额及隔离验收。
5. 最后处理可选持久化队列/scheduler 与受控 blue-green 编排；先冻结恢复、重复执行和回滚语义再实现。

## 核查统计

- 声明核查：17 条；成立 11 / 部分成立 5 / 不成立 1。
- 上轮评审：已修 3 / 未修 9 / 部分修 1（共 13 条）。
- Skynet 对标：Shield 有 3 条差异，Skynet 有 3 条差异；引用 6 项官方仓库源码/README，检索日期 2026-10-19。
- 缺口 9 条；本文档共 134 行（提交前统计）。
