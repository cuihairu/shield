# 框架对比分析

### Shield vs Pitaya

| 特性 | Shield | Pitaya | 说明 |
|--- |--- |--- |--- |
| **编程语言** | C++ + Lua | Go | Shield 性能更高，Pitaya 开发更快 |
| **并发模型** | Actor + 协程 | Goroutine | Shield 更适合 CPU 密集型任务 |
| **分布式架构** | ✅ 借鉴 Pitaya | ✅ 原生设计 | 两者都支持水平扩展 |
| **脚本支持** | Lua (热重载) | Go (编译) | Shield 支持动态更新，Pitaya 类型安全 |
| **网络性能** | 极高 (C++/Asio) | 高 (Go runtime) | Shield 在高并发下性能更优 |
| **开发效率** | 中等 | 高 | Go 生态更完善，开发更快 |
| **内存使用** | 低 | 中等 | C++ 内存控制更精确 |
| **学习曲线** | 陡峭 | 平缓 | C++ 复杂度更高 |

### Shield vs Skynet

| 特性 | Shield | Skynet | 说明 |
|--- |--- |--- |--- |
| **架构范围** | 分布式集群 | 单节点 | Shield 支持多节点，Skynet 专注单机 |
| **Actor 系统** | CAF 分布式 | 自定义轻量 | CAF 功能更丰富，Skynet 更轻量 |
| **网络层** | Boost.Asio | 自定义 epoll | Boost.Asio 更通用，跨平台 |
| **消息传递** | 类型化消息 | 二进制消息 | Shield 类型安全，Skynet 性能极致 |
| **服务发现** | 多后端支持 | 无 | Shield 支持生产级服务发现 |
| **协议支持** | HTTP/WS/TCP | TCP | Shield 协议支持更丰富 |
| **可扩展性** | 水平扩展 | 垂直扩展 | Shield 适合大规模部署 |
| **资源占用** | 中等 | 极低 | Skynet 资源使用更少 |

### Shield vs 其他主流框架

#### vs Unreal Engine Server

| 特性 | Shield | UE Server |
|--- |--- |--- |
| **定位** | 专用游戏服务器 | 游戏引擎服务器 |
| **性能** | 专为服务器优化 | 客户端引擎改造 |
| **可定制性** | 高度可定制 | 引擎框架限制 |
| **学习成本** | 需要 C++ 经验 | 需要 UE 经验 |

#### vs Unity Netcode

| 特性 | Shield | Unity Netcode |
|--- |--- |--- |
| **架构** | 服务器权威 | P2P + 服务器 |
| **扩展性** | 水平扩展 | 有限扩展 |
| **适用场景** | MMO/大型游戏 | 中小型游戏 |
| **开发语言** | C++ + Lua | C# |