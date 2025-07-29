# Gateway 设计文档

本文档旨在为 `shield` 项目设计一个高性能、可扩展的 Gateway（网关）服务。设计参考了业界成熟的开源游戏服务器框架 [Pitaya](https://pitaya.dfjoy.com/) 的架构思想。

## 1. Pitaya Gateway 功能与设计优缺点分析

Pitaya 是一个用 Go 语言编写的、面向分布式游戏服务器的框架。它的 Gateway 是整个架构中至关重要的一环，是客户端流量的唯一入口。

### 主要功能

1.  **连接管理 (Connection Management)**：作为服务器的门户，Gateway 负责监听端口，接收并维持与客户端的 TCP 或 WebSocket 长连接。
2.  **协议解析 (Protocol Parsing)**：处理底层的网络协议，例如对数据包进行分包、粘包处理，并将原始的字节流解析成 Pitaya 内部可以识别的消息格式。
3.  **会话管理 (Session Management)**：当一个客户端成功连接后，Gateway 会为其创建一个会话（Session）。这个会话包含了客户端的连接信息、用户ID（绑定后）等状态数据，并在客户端断开时负责清理。
4.  **消息路由与转发 (Message Routing & Forwarding)**：这是 Gateway 的核心职责。它根据消息中携带的路由信息（例如 `room.join`），决定应该将这个消息转发给哪一个类型的后端服务器（如 `room` 服务器）。
5.  **服务发现 (Service Discovery)**：Gateway 需要动态地知道后端哪些服务器是可用的。它通过服务发现机制（如 etcd）来获取后端服务器的地址列表和它们提供的服务类型。
6.  **响应下发与广播 (Response & Broadcast)**：Gateway 接收来自后端服务器的响应数据，然后根据会话信息找到对应的客户端连接，并将数据序列化后发送给客户端。它也是实现大规模广播（如全服公告）的理想执行点。
7.  **认证与握手 (Authentication & Handshake)**：客户端连接后的第一步通常是握手和身份认证，这一过程由 Gateway 处理，以确保只有合法的客户端才能进入系统。

### 设计优缺点

#### 优点 (Pros)

*   **关注点分离 (Separation of Concerns)**：将网络处理（连接、协议、会话）与业务逻辑（游戏逻辑、聊天等）完全解耦。后端服务器开发者可以专注于业务，而无需关心底层的网络细节。
*   **水平扩展 (Horizontal Scaling)**：Gateway 和后端业务服务器都可以独立地进行水平扩展。如果连接数成为瓶颈，可以增加 Gateway 实例；如果某种业务逻辑压力大，可以增加对应类型的后端服务器实例。
*   **安全屏障 (Security Barrier)**：Gateway 是系统的唯一入口，可以作为一道安全屏障，将内部的业务服务器保护起来，避免直接暴露在公网。可以在 Gateway 层集中实现防攻击策略（如 DDoS 缓解、消息频率限制）。
*   **集中管理 (Centralized Management)**：客户端的连接和会话被集中管理，方便实现一些全局功能，例如在线玩家统计、踢人、全服广播等。

#### 缺点 (Cons)

*   **单点故障风险 (Single Point of Failure)**：虽然可以部署多个 Gateway 实例，但对于单个客户端而言，它所连接的那个 Gateway 实例一旦宕机，连接就会中断。需要客户端有可靠的重连机制，并且通常要在 Gateway 前面再加一层负载均衡（如 LVS/HAProxy）。
*   **性能瓶颈 (Performance Bottleneck)**：所有进出系统的流量都要经过 Gateway，使其成为潜在的性能瓶颈。Gateway 的实现必须是高性能的。
*   **网络延迟增加 (Increased Latency)**：相比客户端直连业务服务器的单体架构，分布式架构引入了额外的网络跳数（Client -> Gateway -> Backend Server），这会带来微小的延迟增加。
*   **架构复杂性 (Increased Complexity)**：引入了新的组件，使得整个系统的部署、监控和维护变得更加复杂。

---

## 2. 针对 Shield 项目的 Gateway 设计

`shield` 项目已经有了很好的基础，特别是 `net` 目录下的 `master_reactor`、`slave_reactor` 和 `session`，以及 `core` 下的 `actor_system`。这表明项目采用了 **Reactor 模式** 和 **Actor 模型**，这是构建高性能网络服务的经典组合。

本设计将充分利用这些现有组件。Gateway 不一定是一个独立的进程，它可以是 `shield` 服务进程中的一个核心**组件(Component)**。通过配置，你可以启动一个只包含 Gateway 组件的 `shield` 进程，让它成为专职的网关。

### 设计方案

1.  **`GatewayComponent`**:
    *   创建一个新的组件 `shield::gateway::GatewayComponent`，它继承自 `shield::core::Component`。
    *   在它的 `initialize` 方法中，启动网络层，即 `MasterReactor`。

2.  **网络层 (利用现有 `net` 模块)**:
    *   **`MasterReactor`**: 职责不变，作为主 Reactor，只负责监听端口和接受(accept)新的客户端连接。
    *   **`SlaveReactor`**: 职责不变，`MasterReactor` 将新接受的连接分发给某个 `SlaveReactor`。`SlaveReactor` 负责处理该连接上的所有 I/O 事件（读/写）。
    *   **`Session`**: 当一个新连接被 `SlaveReactor`接管后，为其创建一个 `shield::net::Session` 实例。这个 Session 就是 Pitaya Gateway 中会话概念的对应实现。

3.  **协议处理**:
    *   在 `Session` 类中，当接收到数据时（`onRead` 事件），调用一个 `Protocol` 类来解析数据包。你需要定义好客户端和服务端之间的通信协议（例如：`包头[包体长度+命令ID] + 包体[Protobuf/JSON]`）。
    *   解析出完整的消息后，`Session` 并不直接处理它，而是将其封装成一个 `Message` 对象，并投递给 `GatewayComponent`。

4.  **消息路由 (核心逻辑)**:
    *   `GatewayComponent` 内部持有一个 `Router` 对象。
    *   当 `GatewayComponent` 从 `Session` 收到一个 `Message` 后，它将 `Message` 交给 `Router`。
    *   `Router` 根据 `Message` 中的路由信息（例如，从消息头或消息体中解析出的命令ID或服务名称），决定消息的目标 Actor。
    *   为了知道目标 Actor 在哪里，我们需要一个**服务发现模块**。

5.  **服务发现 (`ServiceDiscoveryComponent`)**:
    *   创建一个 `shield::core::ServiceDiscoveryComponent`。
    *   后端的业务 Actor（例如 `ChatActor`, `RoomActor`）在启动时，需要向 `ServiceDiscoveryComponent` **注册**自己能处理的服务类型（或命令ID）。
    *   `Router` 在路由消息时，向 `ServiceDiscoveryComponent` **查询**能够处理该消息的 Actor 地址，然后通过 `ActorSystem` 将消息转发过去。
    *   消息转发时，必须附带上来源的 `SessionID`，以便业务 Actor 处理完毕后，能将响应发回给正确的客户端。

6.  **响应下发**:
    *   业务 Actor 处理完逻辑后，构建一个响应 `Message`。
    *   它通过 `ActorSystem` 将这个响应 `Message` 发送给 `GatewayComponent`，`Message` 中包含目标的 `SessionID`。
    *   `GatewayComponent` 根据 `SessionID` 找到对应的 `Session` 对象，并调用其 `send` 方法，将数据通过网络发送给客户端。

### 流程图

```
[Client]
   |
   | TCP/WebSocket Connection
   v
[MasterReactor] --accepts--> [SlaveReactor]
                                  |
                                  | creates
                                  v
                               [Session] --parses & forwards--> [GatewayComponent]
                                  ^                                  |
                                  |                                  | uses
                                  | send response                    v
                               (finds by SessionID)              [Router] --queries--> [ServiceDiscovery]
                                  ^                                  |
                                  |                                  | forwards via ActorSystem
                                  |                                  v
                               (response)                      [Backend Business Actor]
```

---

## 3. 设计对比与优缺点

| 特性 | Pitaya Gateway | Shield Gateway (设计) |
| :--- | :--- | :--- |
| **实现语言** | Go | C++ |
| **并发模型** | Goroutine + Channel | Reactor (Proactor) + Actor Model |
| **核心角色** | 独立进程类型 | 可配置的组件 (Component) |
| **集成度** | 松耦合，通过 RPC/消息队列通信 | 紧耦合，通过内部 `ActorSystem` 通信 |
| **灵活性** | 部署模式固定 | 部署灵活（可独立部署，也可与业务逻辑混合部署） |

### `shield` 设计方案的优缺点

#### 优点 (Pros)

*   **极致性能**: 基于 C++ 和 Reactor 模式，可以精细地控制内存和线程，避免 GC 带来的延迟抖动，理论上可以达到比 Go 更高的性能天花板和更低的网络延迟。
*   **架构一致性**: 设计完全融入了项目现有的 `Component` 和 `ActorSystem` 体系，代码风格和设计模式高度统一，易于维护和扩展。
*   **部署灵活性**:
    *   **开发/小型部署**: 可以将 `GatewayComponent` 和业务 `Actor` 部署在同一个进程中，简化部署，降低成本。
    *   **大型部署**: 可以将 `GatewayComponent` 部署在独立的进程中，作为专职网关，实现与 Pitaya 类似的大规模分布式架构。
*   **无缝通信**: Gateway 和 Backend 之间通过 `ActorSystem` 直接传递消息（指针或引用），在混合部署模式下几乎没有序列化和网络传输的开销，效率极高。

#### 缺点 (Cons)

*   **开发复杂性**: C++ 的开发、调试和内存管理比 Go 更加复杂，对开发人员的要求更高。要保证系统的稳定性需要付出更多努力。
*   **生态系统**: Go 在云原生和分布式系统方面有更成熟的生态（如 gRPC, etcd, Prometheus），很多库开箱即用。在 C++ 中，类似的服务发现、RPC 框架可能需要自行实现或集成第三方库，工作量更大。
*   **容错性**: 在混合部署模式下，如果一个业务 Actor 的 Bug 导致进程崩溃，那么整个进程（包括 Gateway 功能）都会受到影响，隔离性不如 Pitaya 的独立进程模型。当然，通过独立部署可以规避此问题。
