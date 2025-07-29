# Gateway 网关组件 API 文档

网关组件是 Shield 框架的入口层，负责处理客户端连接、协议转换、消息路由和负载均衡。提供统一的接入点，支持多种网络协议。

## 📋 模块概览

网关模块包含以下主要类：

- `GatewayComponent`: 网关核心组件
- `ConnectionManager`: 连接管理器
- `RouteManager`: 路由管理器
- `ProtocolAdapter`: 协议适配器

## 🌐 GatewayComponent 网关核心

网关核心组件整合网络层、协议层和路由功能，提供完整的网关服务。

### 类定义

```cpp
namespace shield::gateway {

struct GatewayConfig {
    // 网络配置
    struct NetworkConfig {
        std::string tcp_host = "0.0.0.0";
        uint16_t tcp_port = 8080;
        std::string http_host = "0.0.0.0";
        uint16_t http_port = 8081;
        std::string websocket_host = "0.0.0.0";
        uint16_t websocket_port = 8082;
        size_t worker_threads = 8;
        size_t max_connections = 50000;
    } network;
    
    // 协议配置
    struct ProtocolConfig {
        bool enable_tcp = true;
        bool enable_http = true;
        bool enable_websocket = true;
        std::chrono::seconds session_timeout{300};
        size_t max_message_size = 1024 * 1024;  // 1MB
    } protocol;
    
    // 路由配置
    struct RouteConfig {
        std::string default_service = "game-logic";
        std::string load_balance_strategy = "round_robin";
        bool enable_service_discovery = true;
        std::chrono::seconds route_cache_ttl{60};
    } route;
    
    // 监控配置
    struct MonitorConfig {
        bool enable_metrics = true;
        std::chrono::seconds metrics_interval{30};
        bool enable_access_log = true;
        std::string log_format = "json";
    } monitor;
};

class GatewayComponent : public core::Component {
public:
    explicit GatewayComponent(const GatewayConfig& config);
    virtual ~GatewayComponent();
    
    // 组件生命周期
    void on_init() override;
    void on_start() override;
    void on_stop() override;
    
    // 依赖注入
    void set_service_discovery(std::shared_ptr<discovery::IServiceDiscovery> discovery);
    void set_actor_system(std::shared_ptr<actor::DistributedActorSystem> actor_system);
    void set_lua_vm_pool(std::shared_ptr<script::LuaVMPool> vm_pool);
    
    // 路由管理
    void add_route(const std::string& path, const std::string& service_name);
    void remove_route(const std::string& path);
    void set_default_service(const std::string& service_name);
    
    // 中间件管理
    using Middleware = std::function<bool(std::shared_ptr<net::Session>, const protocol::ProtocolMessage&)>;
    void add_middleware(const std::string& name, Middleware middleware);
    void remove_middleware(const std::string& name);
    
    // 连接管理
    std::vector<std::shared_ptr<net::Session>> get_all_sessions();
    std::shared_ptr<net::Session> find_session(uint64_t session_id);
    void broadcast_message(const std::string& message);
    void close_session(uint64_t session_id);
    
    // 统计信息
    struct Statistics {
        std::atomic<uint64_t> total_connections{0};
        std::atomic<uint64_t> active_connections{0};
        std::atomic<uint64_t> total_requests{0};
        std::atomic<uint64_t> total_responses{0};
        std::atomic<uint64_t> total_bytes_received{0};
        std::atomic<uint64_t> total_bytes_sent{0};
        std::atomic<uint64_t> error_count{0};
        
        // 按协议统计
        std::atomic<uint64_t> tcp_connections{0};
        std::atomic<uint64_t> http_requests{0};
        std::atomic<uint64_t> websocket_connections{0};
        
        // 性能指标
        std::atomic<double> avg_response_time{0.0};
        std::atomic<uint64_t> peak_connections{0};
    };
    
    const Statistics& get_statistics() const;
    void reset_statistics();
    
    // 监控和健康检查
    bool is_healthy() const;
    nlohmann::json get_health_report() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::gateway
```

### 使用示例

```cpp
// 创建网关配置
shield::gateway::GatewayConfig config;

// 网络配置
config.network.tcp_port = 8080;
config.network.http_port = 8081;
config.network.websocket_port = 8082;
config.network.worker_threads = std::thread::hardware_concurrency();
config.network.max_connections = 100000;

// 协议配置
config.protocol.enable_tcp = true;
config.protocol.enable_http = true;
config.protocol.enable_websocket = true;
config.protocol.session_timeout = std::chrono::seconds(600);

// 路由配置
config.route.default_service = "game-logic-service";
config.route.load_balance_strategy = "least_connections";
config.route.enable_service_discovery = true;

// 创建网关组件
auto gateway = std::make_unique<shield::gateway::GatewayComponent>(config);

// 设置依赖
auto service_discovery = std::make_shared<shield::discovery::EtcdDiscovery>(etcd_config);
auto actor_system = std::make_shared<shield::actor::DistributedActorSystem>(system, service_discovery, actor_config);
auto lua_vm_pool = std::make_shared<shield::script::LuaVMPool>(vm_config);

gateway->set_service_discovery(service_discovery);
gateway->set_actor_system(actor_system);
gateway->set_lua_vm_pool(lua_vm_pool);

// 配置路由
gateway->add_route("/api/user/*", "user-service");
gateway->add_route("/api/game/*", "game-service");
gateway->add_route("/api/chat/*", "chat-service");
gateway->set_default_service("default-service");

// 添加认证中间件
gateway->add_middleware("auth", [](std::shared_ptr<shield::net::Session> session,
                                  const shield::protocol::ProtocolMessage& message) -> bool {
    // 检查认证头
    std::string auth_header = message.get_header("Authorization");
    if (auth_header.empty()) {
        // 发送 401 响应
        shield::protocol::ProtocolResponse response;
        response.status_code = 401;
        response.set_json_body({{"error", "Authentication required"}});
        
        auto response_data = serialize_response(response);
        session->send(response_data);
        return false;  // 阻止继续处理
    }
    
    // 验证 token
    return validate_auth_token(auth_header);
});

// 添加限流中间件
gateway->add_middleware("rate_limit", [](std::shared_ptr<shield::net::Session> session,
                                        const shield::protocol::ProtocolMessage& message) -> bool {
    std::string client_ip = session->get_remote_endpoint();
    
    // 检查限流
    if (!check_rate_limit(client_ip)) {
        shield::protocol::ProtocolResponse response;
        response.status_code = 429;
        response.set_json_body({{"error", "Rate limit exceeded"}});
        
        auto response_data = serialize_response(response);
        session->send(response_data);
        return false;
    }
    
    return true;
});

// 启动网关
gateway->init();
gateway->start();

SHIELD_LOG_INFO << "网关启动完成";
SHIELD_LOG_INFO << "TCP 端口: " << config.network.tcp_port;
SHIELD_LOG_INFO << "HTTP 端口: " << config.network.http_port;
SHIELD_LOG_INFO << "WebSocket 端口: " << config.network.websocket_port;

// 运行时监控
std::thread monitor_thread([&gateway]() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        
        auto stats = gateway->get_statistics();
        SHIELD_LOG_INFO << "网关统计: "
                       << "活跃连接: " << stats.active_connections.load()
                       << ", 总请求: " << stats.total_requests.load()
                       << ", 错误数: " << stats.error_count.load();
        
        // 健康检查
        if (!gateway->is_healthy()) {
            SHIELD_LOG_ERROR << "网关健康检查失败!";
            auto health_report = gateway->get_health_report();
            SHIELD_LOG_ERROR << "健康报告: " << health_report.dump(2);
        }
    }
});
```

## 🔗 ConnectionManager 连接管理器

管理客户端连接的生命周期、状态和元数据。

### 类定义

```cpp
namespace shield::gateway {

struct ConnectionInfo {
    uint64_t session_id;
    std::string client_ip;
    uint16_t client_port;
    protocol::ProtocolType protocol_type;
    std::chrono::system_clock::time_point connect_time;
    std::chrono::system_clock::time_point last_activity;
    std::map<std::string, std::string> metadata;
    
    // 统计信息
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> message_count{0};
};

class ConnectionManager {
public:
    ConnectionManager();
    virtual ~ConnectionManager();
    
    // 连接管理
    void add_connection(std::shared_ptr<net::Session> session, protocol::ProtocolType type);
    void remove_connection(uint64_t session_id);
    std::shared_ptr<net::Session> find_connection(uint64_t session_id);
    
    // 连接查询
    std::vector<std::shared_ptr<net::Session>> get_all_connections();
    std::vector<std::shared_ptr<net::Session>> get_connections_by_protocol(protocol::ProtocolType type);
    std::vector<std::shared_ptr<net::Session>> get_connections_by_ip(const std::string& ip);
    
    // 连接信息
    std::optional<ConnectionInfo> get_connection_info(uint64_t session_id);
    void update_connection_metadata(uint64_t session_id, const std::string& key, const std::string& value);
    void update_last_activity(uint64_t session_id);
    
    // 批量操作
    void broadcast_to_all(const std::vector<uint8_t>& data);
    void broadcast_to_protocol(protocol::ProtocolType type, const std::vector<uint8_t>& data);
    void close_idle_connections(std::chrono::seconds idle_timeout);
    void close_connections_by_ip(const std::string& ip);
    
    // 统计信息
    size_t get_connection_count() const;
    size_t get_connection_count_by_protocol(protocol::ProtocolType type) const;
    std::map<std::string, size_t> get_connections_by_country() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::gateway
```

### 使用示例

```cpp
// 创建连接管理器
auto connection_manager = std::make_unique<shield::gateway::ConnectionManager>();

// 在会话创建时添加连接
class GatewaySession : public shield::net::Session {
public:
    GatewaySession(boost::asio::ip::tcp::socket socket,
                  shield::gateway::ConnectionManager& conn_mgr,
                  shield::protocol::ProtocolType proto_type)
        : Session(std::move(socket)), m_conn_mgr(conn_mgr), m_protocol_type(proto_type) {}

protected:
    void on_connect() override {
        // 添加到连接管理器
        m_conn_mgr.add_connection(shared_from_this(), m_protocol_type);
        
        // 设置连接元数据
        m_conn_mgr.update_connection_metadata(get_session_id(), "user_agent", get_user_agent());
        m_conn_mgr.update_connection_metadata(get_session_id(), "protocol_version", "1.0");
        
        SHIELD_LOG_INFO << "新连接: " << get_session_id() 
                       << " 来自: " << get_remote_endpoint();
    }
    
    void on_message(const std::vector<uint8_t>& data) override {
        // 更新活跃时间
        m_conn_mgr.update_last_activity(get_session_id());
        
        // 处理消息
        handle_message(data);
    }
    
    void on_disconnect() override {
        // 从连接管理器移除
        m_conn_mgr.remove_connection(get_session_id());
        
        SHIELD_LOG_INFO << "连接断开: " << get_session_id();
    }

private:
    shield::gateway::ConnectionManager& m_conn_mgr;
    shield::protocol::ProtocolType m_protocol_type;
};

// 管理员操作示例
void admin_operations(shield::gateway::ConnectionManager& conn_mgr) {
    // 获取连接统计
    size_t total_connections = conn_mgr.get_connection_count();
    size_t tcp_connections = conn_mgr.get_connection_count_by_protocol(shield::protocol::ProtocolType::TCP);
    size_t http_connections = conn_mgr.get_connection_count_by_protocol(shield::protocol::ProtocolType::HTTP);
    size_t ws_connections = conn_mgr.get_connection_count_by_protocol(shield::protocol::ProtocolType::WEBSOCKET);
    
    SHIELD_LOG_INFO << "连接统计: 总计=" << total_connections 
                   << ", TCP=" << tcp_connections 
                   << ", HTTP=" << http_connections 
                   << ", WebSocket=" << ws_connections;
    
    // 清理闲置连接
    conn_mgr.close_idle_connections(std::chrono::minutes(30));
    
    // 广播系统公告
    nlohmann::json announcement = {
        {"type", "system_announcement"},
        {"message", "服务器将在 10 分钟后重启维护"},
        {"timestamp", std::time(nullptr)}
    };
    
    std::string announcement_str = announcement.dump();
    std::vector<uint8_t> announcement_data(announcement_str.begin(), announcement_str.end());
    conn_mgr.broadcast_to_all(announcement_data);
    
    // 获取特定连接信息
    auto connections = conn_mgr.get_all_connections();
    for (auto& session : connections) {
        auto conn_info = conn_mgr.get_connection_info(session->get_session_id());
        if (conn_info) {
            SHIELD_LOG_INFO << "连接 " << conn_info->session_id 
                           << ": IP=" << conn_info->client_ip 
                           << ", 协议=" << static_cast<int>(conn_info->protocol_type)
                           << ", 消息数=" << conn_info->message_count.load();
        }
    }
}
```

## 🚦 RouteManager 路由管理器

负责消息路由和负载均衡，将客户端请求转发到合适的后端服务。

### 类定义

```cpp
namespace shield::gateway {

enum class LoadBalanceStrategy {
    ROUND_ROBIN,        // 轮询
    LEAST_CONNECTIONS,  // 最少连接数
    WEIGHTED_RANDOM,    // 加权随机
    CONSISTENT_HASH,    // 一致性哈希
    IP_HASH            // IP 哈希
};

struct RouteRule {
    std::string path_pattern;           // 路径模式 (支持通配符)
    std::string service_name;           // 目标服务名
    LoadBalanceStrategy strategy;       // 负载均衡策略
    std::map<std::string, std::string> headers;  // 添加的请求头
    std::chrono::seconds timeout{30};  // 超时时间
    int priority = 0;                   // 优先级 (数字越大优先级越高)
    bool enabled = true;                // 是否启用
};

struct RouteTarget {
    std::string service_id;
    std::string endpoint;
    int weight = 1;
    std::atomic<int> active_connections{0};
    std::chrono::system_clock::time_point last_used;
};

class RouteManager {
public:
    RouteManager();
    virtual ~RouteManager();
    
    // 路由规则管理
    void add_route(const RouteRule& rule);
    void remove_route(const std::string& path_pattern);
    void update_route(const std::string& path_pattern, const RouteRule& rule);
    std::vector<RouteRule> get_all_routes() const;
    
    // 路由匹配
    std::optional<RouteRule> match_route(const std::string& path) const;
    std::optional<RouteTarget> select_target(const std::string& service_name,
                                           const std::string& client_key = "");
    
    // 服务发现集成
    void set_service_discovery(std::shared_ptr<discovery::IServiceDiscovery> discovery);
    void update_service_targets(const std::string& service_name,
                               const std::vector<discovery::ServiceInstance>& instances);
    
    // 负载均衡策略
    void set_default_strategy(LoadBalanceStrategy strategy);
    LoadBalanceStrategy get_default_strategy() const;
    
    // 健康检查
    void mark_target_healthy(const std::string& service_id);
    void mark_target_unhealthy(const std::string& service_id);
    bool is_target_healthy(const std::string& service_id) const;
    
    // 统计信息
    struct Statistics {
        std::atomic<uint64_t> total_routes{0};
        std::atomic<uint64_t> successful_routes{0};
        std::atomic<uint64_t> failed_routes{0};
        std::map<std::string, uint64_t> routes_per_service;
    };
    
    const Statistics& get_statistics() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::gateway
```

### 使用示例

```cpp
// 创建路由管理器
auto route_manager = std::make_unique<shield::gateway::RouteManager>();

// 设置服务发现
route_manager->set_service_discovery(service_discovery);

// 添加路由规则
shield::gateway::RouteRule user_route;
user_route.path_pattern = "/api/user/*";
user_route.service_name = "user-service";
user_route.strategy = shield::gateway::LoadBalanceStrategy::LEAST_CONNECTIONS;
user_route.timeout = std::chrono::seconds(10);
user_route.priority = 10;
route_manager->add_route(user_route);

shield::gateway::RouteRule game_route;
game_route.path_pattern = "/api/game/*";
game_route.service_name = "game-service";
game_route.strategy = shield::gateway::LoadBalanceStrategy::CONSISTENT_HASH;
game_route.headers["X-Game-Version"] = "1.0";
game_route.priority = 20;
route_manager->add_route(game_route);

// 实时路由匹配示例
void handle_client_request(const shield::protocol::ProtocolMessage& message,
                          shield::gateway::RouteManager& route_mgr) {
    // 匹配路由规则
    auto route_rule = route_mgr.match_route(message.path);
    if (!route_rule) {
        SHIELD_LOG_WARN << "未找到路由规则: " << message.path;
        send_404_response();
        return;
    }
    
    // 选择目标服务
    std::string client_key = message.get_header("X-Client-ID");
    auto target = route_mgr.select_target(route_rule->service_name, client_key);
    if (!target) {
        SHIELD_LOG_ERROR << "无可用的目标服务: " << route_rule->service_name;
        send_503_response();
        return;
    }
    
    // 转发请求
    forward_request_to_service(message, *target, *route_rule);
}

// 服务发现集成示例
void integrate_service_discovery(shield::gateway::RouteManager& route_mgr,
                                std::shared_ptr<shield::discovery::IServiceDiscovery> discovery) {
    // 监听服务变化
    discovery->watch_service("user-service", [&route_mgr](const std::vector<shield::discovery::ServiceInstance>& instances) {
        SHIELD_LOG_INFO << "用户服务实例更新: " << instances.size() << " 个实例";
        route_mgr.update_service_targets("user-service", instances);
    });
    
    discovery->watch_service("game-service", [&route_mgr](const std::vector<shield::discovery::ServiceInstance>& instances) {
        SHIELD_LOG_INFO << "游戏服务实例更新: " << instances.size() << " 个实例";
        route_mgr.update_service_targets("game-service", instances);
    });
}
```

## 📚 最佳实践

### 1. 网关高可用配置

```cpp
// ✅ 高可用网关部署
class HighAvailabilityGateway {
public:
    void deploy_ha_gateway() {
        // 1. 多端口监听
        shield::gateway::GatewayConfig config;
        config.network.tcp_port = 8080;
        config.network.http_port = 8081;
        config.network.websocket_port = 8082;
        
        // 2. 健康检查端点
        config.monitor.enable_metrics = true;
        config.monitor.enable_access_log = true;
        
        auto gateway = std::make_unique<shield::gateway::GatewayComponent>(config);
        
        // 3. 故障转移机制
        gateway->add_middleware("failover", [](auto session, const auto& message) -> bool {
            try {
                return process_request(session, message);
            } catch (const std::exception& e) {
                SHIELD_LOG_ERROR << "请求处理失败，启用故障转移: " << e.what();
                return handle_failover(session, message);
            }
        });
        
        // 4. 优雅关闭
        std::signal(SIGTERM, [&gateway](int) {
            SHIELD_LOG_INFO << "收到关闭信号，开始优雅关闭...";
            
            // 停止接受新连接
            gateway->stop_accepting_connections();
            
            // 等待现有连接处理完成
            auto stats = gateway->get_statistics();
            while (stats.active_connections.load() > 0) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                stats = gateway->get_statistics();
                SHIELD_LOG_INFO << "等待连接关闭，剩余: " << stats.active_connections.load();
            }
            
            gateway->stop();
            std::exit(0);
        });
    }

private:
    bool handle_failover(std::shared_ptr<shield::net::Session> session,
                        const shield::protocol::ProtocolMessage& message) {
        // 实现故障转移逻辑
        return try_backup_service(session, message);
    }
};
```

### 2. 智能路由和限流

```cpp
// ✅ 智能路由和限流
class SmartRouting {
public:
    void configure_smart_routing(shield::gateway::RouteManager& route_mgr) {
        // 基于响应时间的智能路由
        route_mgr.set_target_selector([](const std::vector<shield::gateway::RouteTarget>& targets) 
            -> std::optional<shield::gateway::RouteTarget> {
            
            if (targets.empty()) return std::nullopt;
            
            // 选择响应时间最短且连接数最少的服务
            auto best = std::min_element(targets.begin(), targets.end(),
                [](const auto& a, const auto& b) {
                    double score_a = get_service_score(a.service_id);
                    double score_b = get_service_score(b.service_id);
                    return score_a < score_b;
                });
            
            return *best;
        });
        
        // 自适应限流
        route_mgr.add_middleware("adaptive_rate_limit", [this](auto session, const auto& message) -> bool {
            std::string client_ip = session->get_remote_endpoint();
            
            // 获取当前负载
            double current_load = get_system_load();
            
            // 动态调整限流阈值
            int rate_limit = calculate_dynamic_rate_limit(current_load);
            
            if (!check_rate_limit(client_ip, rate_limit)) {
                send_rate_limit_response(session);
                return false;
            }
            
            return true;
        });
    }

private:
    double get_service_score(const std::string& service_id) {
        // 综合响应时间、错误率、连接数等指标计算得分
        double response_time = get_avg_response_time(service_id);
        double error_rate = get_error_rate(service_id);
        int connections = get_active_connections(service_id);
        
        return response_time * (1 + error_rate) * (1 + connections / 1000.0);
    }
    
    int calculate_dynamic_rate_limit(double system_load) {
        // 根据系统负载动态调整限流阈值
        if (system_load < 0.5) return 1000;      // 低负载
        else if (system_load < 0.8) return 500;  // 中负载
        else return 100;                          // 高负载
    }
};
```

### 3. 监控和日志

```cpp
// ✅ 完善的监控和日志
class GatewayMonitoring {
public:
    void setup_monitoring(shield::gateway::GatewayComponent& gateway) {
        // 性能指标收集
        std::thread metrics_thread([&gateway]() {
            while (true) {
                collect_metrics(gateway);
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
        });
        
        // 访问日志
        gateway.add_middleware("access_log", [](auto session, const auto& message) -> bool {
            auto start_time = std::chrono::high_resolution_clock::now();
            
            // 处理请求
            bool result = true;  // 实际的处理结果
            
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            // 记录访问日志
            nlohmann::json access_log = {
                {"timestamp", std::time(nullptr)},
                {"client_ip", session->get_remote_endpoint()},
                {"method", message.method},
                {"path", message.path},
                {"status_code", result ? 200 : 500},
                {"response_time", duration.count()},
                {"user_agent", message.get_header("User-Agent")},
                {"bytes_sent", session->get_statistics().bytes_sent.load()}
            };
            
            SHIELD_LOG_INFO << "ACCESS: " << access_log.dump();
            return result;
        });
        
        // 错误监控和告警
        gateway.set_error_handler([](const std::exception& e, auto session, const auto& message) {
            nlohmann::json error_log = {
                {"timestamp", std::time(nullptr)},
                {"error_type", "gateway_error"},
                {"error_message", e.what()},
                {"client_ip", session->get_remote_endpoint()},
                {"request_path", message.path},
                {"request_method", message.method}
            };
            
            SHIELD_LOG_ERROR << "ERROR: " << error_log.dump();
            
            // 发送告警 (如果错误率过高)
            if (should_send_alert()) {
                send_alert_notification(error_log);
            }
        });
    }

private:
    void collect_metrics(shield::gateway::GatewayComponent& gateway) {
        auto stats = gateway.get_statistics();
        
        // 发送指标到监控系统 (如 Prometheus)
        send_metric("gateway_active_connections", stats.active_connections.load());
        send_metric("gateway_total_requests", stats.total_requests.load());
        send_metric("gateway_error_rate", calculate_error_rate(stats));
        send_metric("gateway_avg_response_time", stats.avg_response_time.load());
    }
};
```

---

网关组件是 Shield 框架的关键入口层，提供了高性能、高可用的客户端接入能力。通过合理的架构设计和配置，可以支持大规模并发访问和复杂的路由需求。