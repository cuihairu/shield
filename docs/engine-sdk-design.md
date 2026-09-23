# 游戏引擎 SDK 设计（总纲）

> 状态：设计提案，待评审拍板；本文不动任何代码。
>
> 本文回答三个问题：面向常见游戏引擎的客户端 SDK **共享什么**（wire 契约
> 与会话语义）、**各自实现什么**（引擎接入形态与平台适配）、**按什么顺序
> 落地**（分阶段路径与打样选择）。协议本身的权威定义不在本文——本文只
> 引用既有契约文档，凡本文与契约文档冲突，以契约为准。

## 1. 为什么做 SDK

Shield 服务端的客户端协议已经闭环：descriptor 声明路由、Gateway 校验
`route_id`、session 单一 target 绑定、`ClientIngress/ClientEgress` 类型化
消息、端到端真实 TCP 验收（见
[protocol-routing-design.md](protocol-routing-design.md) M6）。但这套闭环
的「客户端」目前只有测试里的裸 socket：真实项目接入时，客户端团队要面对
envelope 切帧、`route_id` 字节序、认证后 target 切换、断线重连语义——这些
都是服务端语义的正确镜像，却没有任何可复用的实现载体。

没有 SDK 的三个直接后果：

1. **协议正确性靠每个接入方重新证明。** 切帧错一个字节序、`route_id`
   宽度配错、body 里多包一层 `{route, payload}`（header-route 协议明确
   禁止的遗留包装），都要等联调期才暴露。
2. **服务端被迫为接入方做兼容。** 客户端实现错了，压力最终会变成「服务端
   能不能容忍这种包」的讨论——而
   [protocol-routing-design.md](protocol-routing-design.md) 的禁止项清单
   正是为了拒绝这类演化。
3. **descriptor 的价值止步于服务端。** descriptor 是唯一静态来源，但它的
   客户端消费（route 常量、DTO、typed API）目前只有一份 draft 的
   [Unity generator 规范](xmldef-unity-generator-spec.md)。

SDK 的目标因此不是「再发明一层协议」，而是把既有契约**实现一次、验收一
次、多引擎复用**。

### 非目标

- 不定义新 wire 协议、新 header 字段、新错误码。SDK 是
  [runtime-network.md](runtime-network.md) 所冻结契约的客户端镜像。
- 不在 SDK 里做业务路由（room/scene/map 是 PlayerService 私有状态，
  SDK 不感知）。
- 不做协议协商（codec 按监听器锁定，per-session 协商本来就是
  [protocol-codec-plugins.md](protocol-codec-plugins.md) 的 Non-Goal）。
- 不做服务端到服务端的新通道（见 §8 开放决策点）。

## 2. 组织结构：引擎为主，语言为辅

SDK 文档与实现仓库按**引擎**切分：`unity/`、`unreal/`、`cocos/`、
`laya/`、`minigame-wechat/`、`server/`，语言只作为每节内的实现细节。

为什么不是按语言切（C# / C++ / TS / Go 各一章）：

1. **接入者的检索词是引擎。** 团队说「我们是 Unity 项目」，不说「我们是
   C# 项目」。同一个 C# 在 Unity 与服务端（NAOT/IL2CPP 差异）的约束完全
   不同；同一个 TS 在 Cocos 原生、LayaAir 浏览器、微信小游戏的运行环境
   也完全不同。语言无法预测约束，引擎可以。
2. **共享关系是引擎间的，不是语言内的。** Cocos Creator 与 LayaAir 共享
   一个 TS 核心，是因为两者的宿主 API 形状相近（场景生命周期、字节数组、
   WebSocket 可用性），不是因为它们同语言。按语言组织会把这条最重要的
   共享关系藏进两章的重复段落里。
3. **发布物按引擎分发。** Unity 走 UPM 包、Unreal 走 `.uplugin`、
   TS 走 npm 子入口——包管理生态本来就是按引擎划分的。

## 3. 统一会话语义：所有引擎共享的那一层

每个引擎 SDK 无论实现语言，都实现**同一份**会话语义。这份语义不是新设
计，而是逐条镜像服务端既有契约；每条都标注契约出处，证明它不是本文编造
的。

| 语义 | 客户端 SDK 的责任 | 契约出处 |
| --- | --- | --- |
| 切帧 | 按 listener 的 envelope 配置实现（`idlen`/`typelen`，`route_id_bytes`/`length_bytes`/`endian` 由 profile 配置驱动） | [runtime-network.md](runtime-network.md) ProtocolProfile |
| 路由 | 请求写 header `route_id`（来自生成代码的 route 常量，不手写 magic number）；body 只含业务数据 | [protocol-routing-design.md](protocol-routing-design.md) 决策节 |
| 序列化 | v1 只绑 `json` body codec；protobuf/msgpack/flatbuffers/xmldef 跟随 codec 插件生态后置 | [protocol-codec-plugins.md](protocol-codec-plugins.md) |
| 认证 | 先走 `requires_auth: false` 的预登录路由；认证成功由**服务端** `shield.client.bind` 切换 target——客户端 SDK 无感知，不实现 epoch 逻辑 | [gateway.md](gateway.md) Session 绑定 |
| 重连 | 重连=新连接新 session；SDK 负责退避重试+重放登录；服务端负责新 epoch 与旧包拒绝 | [runtime-network.md](runtime-network.md) 断线与重连 |
| 出站推送 | s2c 路由按生成的 handler 接口回调；SDK 不提供按 `route`/`method` 字符串的通用发送口（服务端明确拒绝的形态，SDK 不镜像它） | [protocol-routing-design.md](protocol-routing-design.md) 出站路径 |
| 背压边界 | 出站发送失败即报错给上层；SDK 不做无界发送队列、不隐式重试——与服务端 `ClientEgress` 的 fire-and-forget 语义对齐 | [runtime-network.md](runtime-network.md) 背压与限制 |

两条特别说明，因为它们直接决定 SDK 的**薄度**：

**认证为什么客户端无感知。** epoch、target CAS、旧 `ClientRef` 失效全部
是 Gateway 侧的防陈旧机制。客户端 SDK 需要做的只有两件事：连上后能调预
登录路由；登录请求把凭证交给认证服务。`shield.client.bind` 成功后服务端
Lua 拿到新 `ClientRef`，后续业务路由自然放行——客户端没有任何需要维护的
会话状态机。这是这套协议设计给 SDK 的最大红利：**会话状态在服务端，客户
端 SDK 薄到只剩「连接 + 编解码 + 回调派发」**。

**重连为什么可以自动重放登录。** 服务端语义是重连认证成功后由玩家 owner
决定恢复原 PlayerService 还是新建实例。这意味着客户端自动重连+重放登录
永远是安全的（最坏情况是拿到一个全新实例），SDK 不需要实现「会话恢复」
这类复杂状态。唯一的例外是 in-flight `call`：重连后它们按失败处理而不是
静默重放——写操作的重放是不安全的，这条与服务端「不隐式重试」的立场
一致，SDK 必须同样保守。

### 共享实现拓扑：哪些真共享，哪些不共享

| SDK | 实现形态 | 为什么 |
| --- | --- | --- |
| Cocos Creator + LayaAir + 微信小游戏 | **一个纯 TS 核心**（零平台 API 依赖），三个薄平台适配层（socket、存储、生命周期注入） | 三者宿主能力同构（TS 运行时 + WebSocket 可用 + 事件循环驱动），差异全部收敛在适配层接口后（见 §4 各节）。一份核心 = 一致性测试跑一次 |
| Unity（C#） | 独立实现，协议面代码由 generator 生成 | Unity 的价值线（主线程模型、UPM 分发、IL2CPP/AOT 约束）全是 C# 生态特有的；且 [xmldef-unity-generator-spec.md](xmldef-unity-generator-spec.md) 已定义了 C# 生成契约，复用它是顺水推舟 |
| Unreal（C++） | 独立实现（引擎插件形态） | 服务端 C++ 核心是 CAF actor 运行时，**不能**作为客户端库拖进 UE——客户端要的是 socket 子系统 + 游戏线程派发，与服务端执行模型正交。强行共享 C++ 核只会把服务端依赖（CAF、sol2）泄漏进客户端构建 |
| 服务端（Go/Node/Lua） | 后置，定位见 §8 | 是「服务端调用方」而不是游戏客户端，接入通道是开放决策点，不在 v1 承诺内 |

判据一句话：**共享的永远是「语义 + 测试向量」，代码只在宿主能力同构时才
共享。** 强行跨异构宿主抽公共层（比如把 TS 核心硬套 Unity）会制造一层
人人都得理解但没人受益的间接层；反之，同构宿主不共享（三份 TS 实现）会
让协议修复改三处。

### 一致性验收：SDK 的「绿」由什么定义

每个引擎 SDK 必须通过同一组**协议一致性测试向量**：同一份 descriptor、
同一组字节级 fixture（正帧、错 `route_id`、超长 frame、body 夹带
`route` 字段的包、断线重连序列），断言各 SDK 产出的字节与解析行为逐字节
一致。向量的基准来源是服务端已验收的 wire 形状
（`tests/acceptance/test_client_rpc_e2e.cpp` 的真实 TCP 闭环所定义的）。
这样「某个引擎的 SDK 合格」永远不是该引擎维护者的自我声明，而是与服务
端闭环的机器比对。

## 4. 各引擎接入形态

以下每节回答四件事：包管理与分发形态、类库还是组件、生命周期钩子、
平台差异与限制。与核心的通信全部遵循 §3 的统一语义，各节只写**该引擎
特有**的部分。

### 4.1 Unity3D（C#）

- **包管理**：UPM 包（git URL 或 registry），配合 Unity Package
  Manager 的 sample 导入最小示例。选 UPM 而不是 `.unitypackage` 的原因：
  generator 产物（DTO/route 常量）与 SDK 运行时需要独立演进版本，UPM 的
  版本锁才能让「协议升级」变成可审计的 diff。
- **形态：纯类库 + 可选 Host 组件**。核心是纯 C# 类库（`ShieldClient`），
  不强制挂 MonoBehaviour——服务端游戏的后台逻辑、编辑器工具、服务器
  机器人都能用；同时提供一个可选的 `ShieldRuntimeHost` 组件，负责在
  `Update` 泵 IO 与派发回调，满足「拖一个 prefab 就能跑」的接入预期。
  这与 [xmldef-unity-generator-spec.md](xmldef-unity-generator-spec.md)
  的 Threading Boundary 一节完全同构：IO 线程收包 → 主线程派发适配层 →
  用户 handler，是否切主线程由 Host 决定而不是写死在 codec/DTO。
- **生命周期钩子**：`Application.focusChanged`（切后台暂停读、恢复后
  补心跳）、`Application.quitting`（优雅 close）、场景切换**不**断连
  （连接生命周期独立于场景是默认语义，避免「加载场景就掉线」这类接入
  事故）。
- **协议面**：route 常量/DTO/typed send-call API/handler 接口直接采用
  Unity generator 规范已定义的产物（`RouteIds.cs`、DTO、
  `GatewayClient.cs`、`IXmldefHandler.cs`、partial stubs）——SDK 运行时
  是这些产物的**宿主**，generator 是它们的**来源**，两者用生成的
  `SchemaInfo`（package id/version/schema root hash）对账。
- **平台差异**：iOS/Android 原生 TCP 可用（IL2CPP 下注意 socket API
  子集）；**WebGL 无原生 TCP**，必须等 WebSocket transport（见 §7 前置
  依赖）；IL2CPP 禁用 `Reflection.Emit`，生成代码不做运行时反射。

### 4.2 Unreal（C++ / 蓝图）

- **包管理**：`.uplugin` 插件，运行时模块 + 可选编辑器模块。
- **形态：GameSubsystem 为根的纯类库 + 蓝图门面**。`UShieldClientSubsystem`
  持有连接与派发；蓝图可见的只有 `UShieldClientSubsystem` 上的
  `UFUNCTION`（connect/login/send/dispatch 委托绑定）。为什么子系统而
  不是 Actor 组件：连接的生命周期属于游戏实例而不是某个关卡 Actor，
  用 Actor 挂连接会重演 Unity 生态里「切场景掉线」的常见事故。
- **生命周期钩子**：`FCoreDelegates::ApplicationWillEnterBackground` /
  `ApplicationHasEnteredForeground`（移动端后台心跳暂停）、
  `EndPlay`（优雅关闭）、World 切换不断连（同 Unity 的理由）。
- **线程模型**：socket IO 在 `FSocket` + 非阻塞轮询线程；handler 一律
  `AsyncTask(Lambda_GameThread)` 派发——UE 的游戏对象只能游戏线程触碰，
  这条没有选择余地，所以 SDK 不提供「关线程派发」的开关，避免使用者
  踩出竞态。
- **平台差异**：Win/Mac/Linux/Android/iOS 走原生 socket；主机平台私有
  SDK（GDK/NP）不在 v1 范围；蓝图类型边界只暴露 POD 与 TArray/TMap，
  不把 SDK 内部类型漏进蓝图。

### 4.3 Cocos Creator（TS）

- **包管理**：npm 包 `@shield/sdk-ts`（核心）+ `@shield/sdk-ts/cocos`
  子入口。不做成 Cocos 扩展（extension）插件：SDK 是游戏运行时依赖，
  不是编辑器工具。
- **形态：纯类库，组件可选**。与 Unity 同构的决策：核心 `ShieldClient`
  不依赖场景树；提供一个挂 `Component` 的可选封装用于编辑器配置。
- **生命周期钩子**：`game.on(Game.EVENT_HIDE/SHOW)`（切后台暂停/恢复）、
  对应微信小游戏侧见 §4.5。
- **传输差异**：原生平台（Android/iOS via jsb）理论上有原生 socket，
  但**统一走 WebSocket 后端**——原生/Web/小游戏三种构建用同一条传输
  路径，SDK 的传输层分支数从 3 降为 1。原生 socket 的性能优势对
  卡牌/SLG 类项目的消息量级不构成瓶颈，而少一条分支省下的是全部平台
  组合的测试矩阵。

### 4.4 LayaAir（TS）

- **包管理**：同一个 `@shield/sdk-ts`，`/laya` 子入口。
- **形态**：纯类库（LayaAir 3.x 的组件体系与 Cocos 不同构，不做组件
  封装，事件挂接交给业务代码）。
- **生命周期**：跟随 TS 核心的可见性/焦点钩子，由 `/laya` 适配层桥接到
  Laya 的 stage 事件。
- **为什么与 Cocos 共享核心还是独立入口**：两者共享的是协议与会话核心
  （切帧、认证、重连、handler 派发），不共享 UI/生命周期桥——所以是
  「一个核心、两个入口」，而不是「一个包」。

### 4.5 微信小游戏（JS，平台约束单列）

- **分发**：同一 npm 包，`/wechat` 子入口；SDK 核心体积预算 **≤ 60KB
  min+gzip**（主包体积是小游戏项目的硬约束，SDK 超预算 = 接不进来，
  这条是验收项不是愿景）。
- **平台差异（全部是硬约束）**：
  - 只有 `wx.connectSocket`（WebSocket），**无原生 TCP**——服务端
    WebSocket transport 是这个平台的**硬前置**（§7）；
  - 域名必须备案且在小游戏后台白名单（wss）；
  - 无 `localStorage`，重连所需凭证缓存走 `wx.setStorageSync`（适配层
    注入）；
  - 切后台由 `wx.onAppHide/onAppShow` 通知，且 iOS 上后台 socket 会在
    数秒内被系统回收——SDK 必须把「回前台即重连+重放登录」做成默认
    行为而不是可选策略。
- **为什么单列而不并入 TS 节**：上面每一条都会改变 SDK 的默认行为而不
  只是适配代码——平台约束写到行为层，就该在结构上可见。

### 4.6 服务端调用方（Go / Node / Lua，按需）

定位先行：**这不是游戏客户端 SDK，是机器人/AI 压测/外部运营服务的接入
载体**。它们走 Gateway 的普通客户端协议（机器账号登录认证），复用与游戏
客户端完全相同的 wire 语义——不为服务端调用方发明第二条协议。

- Go：`go.mod` 单包，goroutine-per-connection，API 面向
  `context.Context` 取消语义。
- Node：同一 wire 实现，Promise API。
- Lua：服务端生态里已有 Lua 消费者（OpenResty 类），为工具链完整性
  保留，优先级最低。
- 为什么后置：游戏引擎客户端才是 descriptor 价值的主战场；服务端调用方
  的接入通道本身还有开放决策（§8），不该被 SDK 计划绑定期限。

## 5. 跨引擎的 API 语义对照

各语言 API **命名**按语言惯用（C# PascalCase、TS camelCase），**语义**
逐条对齐，对照表是每个 SDK 的一致性验收项：

| 语义 | C# | TS | UE（C++/BP） |
| --- | --- | --- | --- |
| 建连 | `ConnectAsync(uri, profile)` | `connect(uri, profile)` | `Connect(uri, profile)` |
| 预登录调用 | `SendXxx(req)` / `CallXxxAsync(req)` | `sendXxx(req)` / `callXxx(req)` | `SendXxx` / `CallXxx`（蓝图委托） |
| 登录 | 业务路由承载（认证服务 descriptor），无 SDK 内建 login 类型 | 同 | 同 |
| s2c 派发 | `IXxxHandler.OnXxx(msg)` | `onXxx(msg)` 回调注册 | `OnXxx` 动态多播委托 |
| 断线 | `Reconnecting`/`Reconnected`/`Closed` 事件 | 同名事件 | 同名多播委托 |
| 优雅关闭 | `CloseAsync(reason)` | `close(reason)` | `Close(reason)` |

注意「登录」一行：SDK **不提供**内建 `login()`。认证就是一条
`requires_auth: false` 的普通 c2s 路由，由 descriptor 声明、认证服务实
现——如果 SDK 内建登录类型，就把「认证方式」这个本该属于业务的决策冻结
进了 SDK。SDK 只提供重连时「重放登录」的钩子位：业务注册一个回调，SDK
在重连成功后调用它（业务在里面重发自己的认证路由）。

## 6. 参照与差异（chirp 对照）

组织结构上参考了 chirp SDK 的两点：**引擎为行、语言为列的兼容矩阵**（
本文 §4/§7 的波次表即它的形态）与**统一钩子接口 + 引擎适配层派发回游戏
线程**的分层（§3 线程模型、§4 各引擎生命周期节）。

有意不同的三点，避免抄错项目：

1. chirp 的 C++ 核心被 Unreal 与桌面共享；shield **不**做这件事——shield
   自己的 C++ 核心是 CAF 运行时本身，把它当客户端库复用等于让客户端拖走
   服务端的整个执行模型（理由见 §3 拓扑表）。
2. chirp 有统一的消息存储/拦截器钩子体系；shield 的对应物是**协议一致
   性测试向量**（§3）——游戏后端场景里，SDK 的可信度来自字节级一致性，
   不是运行时钩子。
3. chirp 按 SDK 目录组织并配 `sdk_compatibility.md`；shield 的等价物是
   本文档 + 未来 `docs/sdks/<engine>.md` 每引擎一页（与 §2 的引擎为主
   结构一致），成熟一个拆一页。

## 7. 分阶段落地路径

| 阶段 | 内容 | 为什么是它 |
| --- | --- | --- |
| **P0 打样：Unity** | `ShieldClient` 纯类库 + Host 组件 + json codec + 一致性向量 v1；generator 规范先落 `RouteIds`+DTO+`GatewayClient` 三件套 | ① [xmldef-unity-generator-spec.md](xmldef-unity-generator-spec.md) 已有完整草案，协议面零新增设计；② C# 客户端是受众最大单一群体；③ 主线程派发是所有引擎里价值最高的适配点，把它的分层在这里定型，其余引擎照抄结构；④ 纯 TCP，**无服务端前置依赖**，可以立即开工 |
| **P1：TS 核心 + WebSocket transport** | 服务端 WebSocket transport adapter（复用 session 绑定/epoch/route_id 语义，[runtime-network.md](runtime-network.md) 已为此预留条款）→ TS 核心 → Cocos/Laya 入口 → 微信适配层 | WebSocket 是三个 TS 平台的共同前置，先落服务端再落客户端，避免 SDK 做完没服务器可连；TS 核心一次覆盖三个分发渠道 |
| **P2：Unreal 插件** | 独立 C++ 实现 + 蓝图门面 | 受众小于移动端两派，但主机/重度项目是付费能力；实现模式已被 P0/P1 验证过两轮，照表施工 |
| **P3：服务端调用方** | 待 §8 决策点收敛后立项 | 依赖开放决策，不预设日期 |

排序的理由压缩成一句：**先做「无前置依赖 + 已有设计存量 + 最大受众」的
组合**——三者同时满足的只有 Unity。

### P0 之外的硬前置清单

- WebSocket transport adapter（P1 前置；UDP/KCP 优先级让位于它，因为
  小游戏平台根本不提供 UDP socket）。
- 协议一致性测试向量的基准化：从 `test_client_rpc_e2e.cpp` 抽出字节级
  fixture，成为跨仓库共享的 golden files。
- generator MVP（`RouteIds` + DTO + `GatewayClient`，Unity generator
  规范的 Recommended Delivery Order 前三步）。

## 8. 决策记录（2026-09-23 评审拍板）

以下四项在本文评审时定案；保留原分析供回溯。

1. **call 的响应关联键：选 a——schema 约定业务关联字段。** wire header
   只有 `route_id`；`GatewayClient` 形态的 `CallXxxAsync` 需要把 s2c 响应
   关联回请求，但当前契约**未定义**关联机制（[gateway.md](gateway.md)
   明确把「框架级 pending 表/future 关联」列为服务端非目标）。定案：不动
   wire，关联键由 descriptor/schema 按约定字段（如 `req_id`）声明，SDK
   按约定字段匹配 pending 表。代价是 call 语义依赖 descriptor 纪律——
   由一致性向量兜底。**待办**：此约定需补进
   [protocol-routing-design.md](protocol-routing-design.md) 的 descriptor
   契约（P0 打样前完成）。
2. **WebSocket transport：P1 启动前出边界文档。** 帧复用 idlen 语义还是
   独立 profile，在一份类似
   [udp-protocol-support.md](udp-protocol-support.md) 的文档里定案，作为
   P1 的开工前置。
3. **服务端调用方通道：单独立项。** 走 Gateway 机器账号 session（复用
   客户端协议）还是 CAF middleman 跨节点接入，涉及认证与信任模型，不随
   SDK 计划绑定期限（P3 排期跟随该立项）。
4. **仓库布局：monorepo 起步。** `sdks/` 子树随服务端同版本发布——一致性
   向量与 golden files 的跨仓同步成本在初期远大于单仓体积收益；分仓的
   触发条件（各引擎版本节奏实质分化）出现时再评估。

## 9. 验收标准（本设计自身的）

- 每个落地的 SDK 通过 §3 一致性向量（字节级比对，零豁免）。
- 任意 SDK 的「从 install 到第一条业务消息往返」接入步骤 ≤ 5 步。
- 服务端协议变更时，一致性向量先行失败——SDK 与契约的偏差在 CI 暴露，
  不进联调期。
- 本文的每一条客户端责任声明都能回指到一篇契约文档；回指断裂（契约
  删除/改语义）时本文同步修订，不出现无主声明。
