# 架构评估（Architecture Review）

评估口径：本项目的目标是**开箱即用的单节点优先游戏服务器运行时**（skynet 启
发、Lua-first、C++ 基础设施）。因此每项判断的标准不是"设计是否精致"，而是
**"对游戏服务器场景是否必要、是否服务产品化"**。评估基于当前代码实测阅读
（非文档转述），证据均给出 file:line。

总体结论：**架构骨架是健康的，方向正确**——CAF 只作内部 actor 运行时且对业务
完全隐藏、one-VM-per-service 的确定性单线程语义、客户端 RPC 的路由描述符 +
session 单目标绑定、分层可 mock 的时间。主要问题不在骨架，而在**默认值不安全/
不友好、少数声明与实现不符、以及两处对游戏场景真正欠缺的能力（DB 异步路径、
热更新）**。不需要为重构而重构；需要的是把"已经对的设计"补上安全的默认值，
把"声明了但没做"的东西要么做掉要么摘掉声明。

各维度判断速览：

| 维度 | 判断 | 主要风险 |
| --- | --- | --- |
| 进程/线程模型 | 合理 | net 单线程默认值是吞吐天花板（低） |
| 网络层与协议 | 合理 | max_frame_size 默认无限（高）、无 TCP_NODELAY（中） |
| 会话与状态管理 | 合理且必须 | — |
| 数据存储与持久化 | 分层合理，执行欠缺 | DB ABI 全同步阻塞（高） |
| 定时器与任务调度 | 合理且优秀 | — |
| 热更新与重启策略 | 诚实欠缺 | blue-green 只有设计稿（中） |
| 扩展点 | 合理 | manifest 仪式感偏重（低） |
| 容错与监控 | 良好，超出同类默认 | 进程级 abort 粒度粗（已接受的设计） |
| 横向扩展能力 | 诚实的 P0 | global 仅进程内（已声明，低） |

---

## 1. 进程/线程模型

**现状**：单进程多线程。CAF 调度线程池（`caf.scheduler.max-threads`）承载全部
service actor；boost::asio `io_context` + 专属 net 线程承载客户端面网络
（bootstrap.cpp:1246-1255：`net.threads > 0` 时开 N 线程，否则**单线程 legacy
模式**）；另有 spawn worker 线程、以及总预算看门狗（detached 线程，超时
`std::_Exit(70)`）。客户端网络与 CAF IO middleman 彻底分离——middleman 只用于
集群，这是对的：客户端面协议（4 种封包 + codec 插件）不该被 CAF 序列化格式
绑架。

**判断：合理**。单节点优先的游戏服不需要多进程；actor 模型 + 每 service 单线程
执行语义给了业务最值钱的东西——**状态无锁确定性**。skynet 同构（worker 线程
+ socket 线程）。

**风险**：
- 【低】net 默认单线程（`net.threads` 未配置时 bootstrap.cpp:1255 只跑一个
  `net_io.run()`）。连接数大时读写与 console/http 共用一个 io_context 会互相
  挤占。建议默认改 2（或按核数），并文档写明调优。产品影响：中小规模无感，
  上量前要改配置。

## 2. 网络层与协议

**现状**：asio TcpListener/Session；4 种封包（lenprefix/idlen/typed_len/
delimiter），线头 `route_id` + 纯业务体；codec 走 C ABI 插件
（`shield.protocol.codec.v1`）；Gateway 做 5 道出站校验 + 入站路由校验
（存在/方向/requires_auth）。

**判断：合理**。route_id 走线头而非 body 内嵌，让转发路径零解码——这对网关类
服务是必须的设计；插件化 codec 把 protobuf/flatbuffers 等重依赖挡在核心之外，
服务产品化（核心发行版可以不带它们）。

**风险**：
- 【高】`max_frame_size` 默认 0 = **不限**（include/shield/net/listener.hpp:88
  `// 0 = unlimited`；session.hpp:153 同）。恶意/异常客户端发一个巨大长度前缀
  即可推动服务端按其分配缓冲。对公网监听这是明确的 DoS 面。**建议：默认给
  16MB 上限（config 校验层兜底），显式配置可覆盖**。这是"安全默认值"问题，
  不是架构问题，修复成本低、产品收益直接。
- 【中】全代码库无 `TCP_NODELAY`（src/net/ 零命中）。Nagle 开着，小包回路
  延迟对回合制/实时游戏是可感知劣化。asio socket 默认即 Nagle on。建议 accept
  后统一 `set_option(tcp::no_delay(true))`。
- ~~【中】无连接级限流/黑名单。~~ **已解决（2026-09-26）**：
  `network.rate_limit`（每连接令牌桶，按解码后消息计费、超限丢帧不断连）
  与 `network.blocklist.deny`（accept 时按地址/CIDR 拒绝、启动期校验条目）
  均已落地，语义见 [安全运行时语义](runtime-security.md)。

## 3. 会话与状态管理

**现状**：session 建立 → 初始 target = 监听器声明的入口服务；认证经
`shield.client.bind` 走 Gateway **epoch CAS** 原子切换单一 target（auth.lua 可
见用法）；此后 c2s RPC 全部直投该 target 服务；s2c 回包经 5 道出站校验。
one-player-one-service（每玩家一个 Lua VM）。

**判断：合理且对游戏场景必须**。单一 target + epoch 防的是"认证切换瞬间的新旧
会话竞态"，这是游戏服特有的正确性问题（切房/重连/顶号）；CAS 语义 + 出站方向
强制（同 route_id 回包被 `egress_direction_rejected` 拒绝，有测试锚点）把"协
议方向写错"变成显式失败。one-player-one-service 换来的是玩家状态无锁——这正
是 skynet 的核心卖点，Shield 保留得干净。

**风险**：无重大。顶号/重连语义细节（踢旧连接的时序）建议后续在 gateway 文档
中显式化，属文档项。

## 4. 数据存储与持久化

**现状**：持久化明确为业务层关注点（runtime-persistence.md 列 6 种存储形态）；
运行时只提供 DB 插件 ABI（`shield.database.v1`：
connect/disconnect/ping/query/execute/begin/commit/rollback/free_result）。

**判断：分层判断合理，执行有欠缺**。

- 分层合理：不造 ORM、不造内置存储（repo 已明确删除 ORM 方向），游戏品类差异
  大，把存储留给业务 + 插件是对"开箱即用"正确的取舍——产品可以不带 MySQL 出
  发行版。
- 【高】**ABI 全同步阻塞，无异步/回调路径**（include/shield/plugin/database.h
  全表无 callback 参数）。后果：任何 Lua service 里发起的 DB 调用会**卡住该
  service 的整个 VM**——单玩家 service 卡自己尚可忍，共享 service（world/
  ranking）里一次慢查询 = 全服卡顿。skynet 同样没有内置 DB，但它的惯例
  （agent 挂起 + driver 服务）让阻塞被隔离在独立服务里；Shield 的文档还没有
  把"DB 调用必须放进独立 service / 必须配超时"写成硬规则。**建议两步走：
  (a) 立即在文档写明使用纪律 + 示例（把 DB 封装进专职 service，业务经
  shield.call 访问）；(b) Phase 2 给 ABI 加异步入口（callback 或 future +
  协程恢复）**。这不是重构，是补一个已知缺口。

## 5. 定时器与任务调度

**现状**：timer 由 CAF delayed send 驱动（ActorTimerState）；业务时间分层
（Clock/MockClock 可 mock 业务钟，真实单调钟只用于调度）；`shield.call`/
`shield.sleep`/timer callback/fork task/客户端 RPC handler 全部协程化，可在其
中 sleep/call 而不阻塞 actor；pending_calls 带超时与 requeue 计数。

**判断：合理且优秀**。协程化的阻塞语义是 skynet 系最难做对的部分，Shield 把
"handler 里可以睡"做成了默认路径而不是高级特性；业务钟可 mock 让"活动开服/
跨天"类逻辑可测——这是超出 skynet 基线的设计（skynet 无此分层）。

**风险**：无结构性风险。看门狗 `_Exit(70)` 的硬切在极端场景丢日志，已有
forensics 弥补，可接受。

## 6. 热更新与重启策略

**现状**：进程内无热更新命令；设计走 blue-green service replacement（新实例
拉起 → 迁移 → 旧实例退出，runtime-lua-vm.md）；容错层面 per-service
on_error → 连续错误计数（阈值 10）→ on_panic → `exit("panic")`；进程级
`lua_atpanic` = forensics 转储 + abort（lua_panic.hpp）。

**判断：诚实欠缺**。skynet 的 `clear` 热更新是它的招牌；Shield 用 blue-green
替代原地打补丁，模型上更适合 one-VM-per-service（状态随实例走，无代码/状态
纠缠），**但当前只有设计稿**。对产品化而言这是 P2：新用户 10 分钟不需要它，
运维期一定会要。落地前文档必须保持当前的不承诺口径（现状做到了）。

容错侧判断**良好**：per-service panic 隔离（坏脚本只退出自己的 service）+
进程级兜底 abort + forensics，比 skynet 默认（错误一路 printf）更适合产品。
abort 粒度粗（单 VM 不可恢复错误 = 全进程死）是已知取舍，配合 systemd/k8s
重启策略可接受，文档应写明。

## 7. 扩展点

**现状**：插件系统 v1——manifest 优先、稳定 C ABI（codec / database 等）、
显式实例与 binding、依赖注入；可选模块（cluster/global/player/server/ops）
编译期开关。核心禁止反向依赖 lua/net/plugin（架构总纲约束）。

**判断：合理**。C ABI dlopen 比 skynet 的"编译期挂 C 模块"更产品化（发行版可
带二进制插件、版本可对齐）；编译期可选模块控制了核心体积。

**风险**：
- 【低】manifest-first 仪式感偏重（对只想要"一个 JSON codec"的用户多了两层
  声明）。官方插件已内置足够示例，可暂不动；若日后反馈集中，可加"内置 codec
  免 manifest"快捷档。

## 8. 容错与监控

**现状**：见第 6 节容错侧 + 监控面默认开启（HTTP `/ops/*` 7 路由 + console
命令面：status/services/plugins/config/cluster/server/global/log.level/attach
REPL/eval/lua.inspect/snapshot/diff）；配置错误启动期 fail-fast；
`--check-config` 离线校验；bootstrap 拆除顺序经 ASan 实证修过三类悬垂
（todo.md 存档）。

**判断：良好，超出同类默认水平**。skynet 默认只有 debug console；Shield 的
默认监控面（health/metrics/inspect/snapshot）已经是"运维可用"而非"开发调试"
水准。拆除顺序的工程 rigor（monitor + down_msg + 5s 兜底阀）是少见的扎实。

**风险**：
- 【中】**声明与实现不符：sandbox**。config/app.yaml 声明
  `lua.sandbox.allow_os/allow_io: false`，但 VM 启动**无条件** open 了
  `sol::lib::io` 与 `sol::lib::os`（src/lua/lua_runtime.cpp:194-202），
  runtime-security.md 也自认未实现。对产品这是诚实性问题：**配置键声明了
  却不生效**比"没有沙箱"更糟（用户以为关了，实际开着）。建议：实现该开关
  （按配置条件 open，成本低）或删键。倾向前者——脚本来自策划/外包的团队
  需要它。**本轮落地**。
- 【低】metrics 口径无文档（有 /ops/metrics 端点，内容语义未成文）。

## 9. 横向扩展能力

**现状**：cluster 可选模块（静态 peers + CAF middleman，M1-M5 已落地）；
global 模块当前为**进程内 P0 后端**（内存 KV/锁/排行榜/队列/cron），Redis
后端声明 Phase 2。

**判断：诚实的 P0**。单节点优先是明示的产品定位，global 先给单进程语义让
API 先稳定、后换分布式后端，顺序正确。风险仅在预期管理：文档已写清
（global_manager.hpp 注释 + roadmap），可接受。

---

## 哪些设计是必须的 / 过度的 / 欠缺的（游戏服务器视角）

**必须且已做对**（保留，勿动）：
- 每 service 单线程 + one-VM-per-player（状态无锁，skynet 核心价值）
- session 单目标绑定 + epoch CAS + 出站方向强制（游戏特有的正确性）
- 协程化 call/sleep/timer/RPC handler（业务写法自然 + 不阻塞 actor）
- route_id 走线头 + codec 插件化（转发零解码 + 核心轻依赖）
- 配置 fail-fast + --check-config（产品级配置体验）
- 默认开启的 /ops/* 与 console（运维开箱）

**过度设计**：基本没有。若一定要挑：插件 manifest 的声明仪式对极简用户偏重
（低优先级观察项）；typed_len 与 delimiter 两种封包的长期维护成本需要用户量
证明（暂保留）。

**欠缺**（按优先级）：
1. 安全默认值：max_frame_size 默认无限（本轮修）；TCP_NODELAY（本轮修）；
   连接限流 + 地址黑名单（本轮修，见 §2 已解决项）
2. sandbox 开关声明未实现（本轮修——产品诚实性）
3. DB 异步路径（先文档纪律，Phase 2 ABI 扩展）
4. 热更新 blue-green 落地（P2 另立项）
5. net.threads 默认值与调优文档（本轮文档带出）

## 改进路线（服务产品化，不重构骨架）

| 阶段 | 内容 | 取舍理由 |
| --- | --- | --- |
| 本轮 P0 | 安全默认值（frame 上限/NODELAY）+ sandbox 开关实现 + 默认配置可观测 + 模板工程 + 上手路径线性化（详见 product-gap.md） | 全部是小改动大收益的"默认值/诚实性"问题；不动架构骨架 |
| Phase 1 | DB 使用纪律文档与示例；metrics 口径文档；net.threads 调优指引；限流设计 | 文档与低风险改动为主，先把已知坑标出来 |
| Phase 2 | DB ABI 异步入口；连接级限流；blue-green 热更新落地；TLS | 各自独立立项，均有既有设计稿或文档位次；按产品反馈排期 |

**明确不做**（本轮）：改用多进程模型、引入 ORM/事件总线/中间件（已删除方向）、
把 CAF 暴露给业务、为覆盖率而重构。架构骨架与产品定位当前匹配，评估结论是
"补默认值与缺口"，不是"改结构"。
