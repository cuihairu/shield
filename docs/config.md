# 配置文档

本文档描述了 `shield` 项目的核心配置文件 `config/shield.yaml` 的结构和选项。

## 顶层结构

配置文件采用 YAML 格式，其顶层结构包含两个主要部分：`components` 和各个组件的详细配置块。

```yaml
# config/shield.yaml

# 声明需要激活的组件列表
components:
  - logger
  - actor_system
  - gateway

# 各个组件的配置详情
logger:
  # ...

gateway:
  # ...
```

### `components`

这是一个字符串列表，用于声明在服务启动时需要加载和初始化的组件。这种设计允许同一个可执行文件通过不同的配置，扮演不同的服务器角色。

---

## Gateway 组件配置 (`gateway`)

当 `gateway` 在 `components` 列表中被声明时，应用会加载网关组件，并查找 `gateway:` 配置块。该组件负责处理客户端网络连接、协议解析和消息路由。

```yaml
# 网关组件 (GatewayComponent) 的专属配置块
gateway:
  # 是否启用此组件。如果为 false，即使在 components 列表中声明了，也不会启动。
  enabled: true

  # 监听器配置，定义了 Gateway 在哪里接收客户端连接
  listener:
    host: "0.0.0.0"    # 监听的 IP 地址。0.0.0.0 表示监听所有网络接口
    port: 3250           # 监听的 TCP 端口
    backlog: 128         # TCP accept 队列的最大长度

  # 线程模型配置
  threading:
    # SlaveReactor 的数量，即 I/O 线程数。
    # 0 表示自动根据 CPU 核心数设置，例如 std::thread::hardware_concurrency()
    io_threads: 0

  # 会话 (Session) 管理配置
  session:
    # 连接的最大空闲时间（毫秒）。超过此时间没有收发数据，服务器将主动断开连接。
    timeout_ms: 60000
    # 支持的最大并发连接数
    max_connections: 10000
    # 每个会话的读缓冲区初始大小（KB）
    read_buffer_size_kb: 64
    # 每个会话的写缓冲区初始大小（KB）
    write_buffer_size_kb: 64

  # 协议处理配置
  protocol:
    # 使用的协议解析器名称，允许未来扩展支持多种协议 (e.g., "json", "protobuf")
    name: "default_binary"
    # 允许接收的最大网络包体大小（KB），防止恶意的大数据包攻击
    max_packet_size_kb: 256

  # TLS/SSL 加密配置
  tls:
    enabled: false # 是否启用 TLS 加密
    # 以下两项在 enabled: true 时为必填
    cert_file: "/path/to/your/server.crt" # SSL 证书文件路径
    key_file: "/path/to/your/server.key"  # SSL 私钥文件路径
```

### 配置项详解

*   `enabled`: (boolean) 控制该组件是否激活。
*   `listener.host`: (string) 绑定的 IP 地址。
*   `listener.port`: (integer) 绑定的 TCP 端口。
*   `listener.backlog`: (integer) 等待接受连接的最大队列长度。
*   `threading.io_threads`: (integer) 用于处理网络 I/O 的线程数量。`0` 表示自适应 CPU 核心数。
*   `session.timeout_ms`: (integer) 会话超时时间，单位为毫秒。
*   `session.max_connections`: (integer) 服务器支持的最大并发连接数。
*   `session.read_buffer_size_kb`: (integer) 每个会话读缓冲区的初始大小，单位为 KB。
*   `session.write_buffer_size_kb`: (integer) 每个会话写缓冲区的初始大小，单位为 KB。
*   `protocol.name`: (string) 指定协议解析器的名称。
*   `protocol.max_packet_size_kb`: (integer) 允许的最大数据包大小，单位为 KB。
*   `tls.enabled`: (boolean) 是否启用 TLS 加密。
*   `tls.cert_file`: (string) SSL 证书文件的绝对路径。
*   `tls.key_file`: (string) SSL 私钥文件的绝对路径。
