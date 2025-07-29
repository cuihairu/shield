# Prometheus 监控集成

Shield 游戏服务器框架集成了 Prometheus 监控系统，提供全面的服务器性能和业务指标监控。

## 功能特性

### 内置指标收集器

1. **系统指标 (SystemMetricsCollector)**
   - `shield_cpu_usage_percent`: CPU 使用率
   - `shield_memory_usage_bytes`: 内存使用量
   - `shield_memory_total_bytes`: 总内存

2. **网络指标 (NetworkMetricsCollector)**
   - `shield_active_connections`: 活跃连接数
   - `shield_bytes_sent_total`: 发送字节总数
   - `shield_bytes_received_total`: 接收字节总数
   - `shield_requests_total`: 请求总数
   - `shield_request_duration_seconds`: 请求处理时间

3. **游戏指标 (GameMetricsCollector)**
   - `shield_active_players`: 在线玩家数
   - `shield_active_rooms`: 活跃房间数
   - `shield_messages_processed_total`: 处理消息总数
   - `shield_actors_created_total`: 创建的 Actor 总数
   - `shield_actors_destroyed_total`: 销毁的 Actor 总数

## 配置

在 `config/shield.yaml` 中配置 Prometheus：

```yaml
prometheus:
  # 启用 Prometheus 指标收集
  enabled: true
  
  # HTTP 指标端点配置
  enable_exposer: true
  listen_address: "0.0.0.0"
  listen_port: 9090
  
  # 指标收集间隔（秒）
  collection_interval: 10
  
  # Push Gateway 配置（可选）
  pushgateway_url: "http://localhost:9091"
  job_name: "shield"
  
  # 为所有指标添加的标签
  labels:
    service: "shield"
    environment: "production"

components:
  prometheus: true  # 启用 Prometheus 组件
```

## 使用方法

### 1. 启用 Prometheus 组件

确保在配置文件中启用了 Prometheus 组件：

```yaml
components:
  prometheus: true
```

### 2. 在代码中使用指标

```cpp
#include "shield/metrics/metrics.hpp"

// 网络指标
SHIELD_METRIC_INC_CONNECTIONS();        // 增加连接数
SHIELD_METRIC_DEC_CONNECTIONS();        // 减少连接数
SHIELD_METRIC_ADD_BYTES_SENT(1024);     // 记录发送字节数
SHIELD_METRIC_ADD_BYTES_RECEIVED(512);  // 记录接收字节数
SHIELD_METRIC_INC_REQUESTS();           // 增加请求计数

// 游戏指标
SHIELD_METRIC_INC_PLAYERS();            // 增加玩家数
SHIELD_METRIC_DEC_PLAYERS();            // 减少玩家数
SHIELD_METRIC_INC_ROOMS();              // 增加房间数
SHIELD_METRIC_DEC_ROOMS();              // 减少房间数
SHIELD_METRIC_INC_MESSAGES();           // 增加消息处理计数
SHIELD_METRIC_INC_ACTORS_CREATED();     // 增加 Actor 创建计数
SHIELD_METRIC_INC_ACTORS_DESTROYED();   // 增加 Actor 销毁计数

// 请求耗时测量
void handle_request() {
    SHIELD_METRIC_TIME_REQUEST();  // 自动测量函数执行时间
    // ... 处理请求逻辑
    // 函数结束时自动记录耗时
}
```

### 3. 直接使用收集器

```cpp
// 获取 Prometheus 组件实例
auto& prometheus = shield::metrics::PrometheusComponent::instance();

// 获取特定的指标收集器
auto network_collector = prometheus.get_network_collector();
if (network_collector) {
    network_collector->increment_connections();
    network_collector->add_bytes_sent(1024);
}

auto game_collector = prometheus.get_game_collector();
if (game_collector) {
    game_collector->increment_active_players();
    game_collector->increment_messages_processed();
}
```

### 4. 添加自定义指标收集器

```cpp
class CustomMetricsCollector : public shield::metrics::MetricsCollector {
public:
    CustomMetricsCollector(std::shared_ptr<prometheus::Registry> registry) {
        // 初始化自定义指标
        custom_counter_family_ = &prometheus::BuildCounter()
            .Name("shield_custom_counter")
            .Help("Custom counter metric")
            .Register(*registry);
        custom_counter_ = &custom_counter_family_->Add({});
    }
    
    void collect() override {
        // 收集自定义指标数据
    }
    
    const std::string& name() const override {
        static std::string name = "custom";
        return name;
    }
    
    void increment_custom() {
        custom_counter_->Increment();
    }

private:
    prometheus::Family<prometheus::Counter>* custom_counter_family_;
    prometheus::Counter* custom_counter_;
};

// 注册自定义收集器
auto custom_collector = std::make_shared<CustomMetricsCollector>(registry);
prometheus.add_collector(custom_collector);
```

## 部署配置

### HTTP Exposer 模式

默认情况下，指标通过 HTTP 端点暴露：

- 地址: `http://localhost:9090/metrics`
- Prometheus 可以通过 scraping 获取指标

### Push Gateway 模式

对于短期运行的作业或防火墙后的服务：

```yaml
prometheus:
  pushgateway_url: "http://prometheus-pushgateway:9091"
  job_name: "shield-server"
```

### Prometheus 配置示例

在 Prometheus 配置文件中添加 scrape 配置：

```yaml
global:
  scrape_interval: 15s

scrape_configs:
  - job_name: 'shield'
    static_configs:
      - targets: ['localhost:9090']
    scrape_interval: 10s
    metrics_path: /metrics
```

## Grafana 仪表板

可以使用以下指标创建 Grafana 仪表板：

### 系统监控面板

```promql
# CPU 使用率
shield_cpu_usage_percent

# 内存使用率
shield_memory_usage_bytes / shield_memory_total_bytes * 100

# 内存使用量趋势
rate(shield_memory_usage_bytes[5m])
```

### 网络监控面板

```promql
# 活跃连接数
shield_active_connections

# 网络吞吐量
rate(shield_bytes_sent_total[5m])
rate(shield_bytes_received_total[5m])

# 请求处理时间分位数
histogram_quantile(0.95, rate(shield_request_duration_seconds_bucket[5m]))
histogram_quantile(0.50, rate(shield_request_duration_seconds_bucket[5m]))

# 请求速率
rate(shield_requests_total[5m])
```

### 游戏业务面板

```promql
# 在线玩家数
shield_active_players

# 房间数
shield_active_rooms

# 消息处理速率
rate(shield_messages_processed_total[5m])

# Actor 创建销毁速率
rate(shield_actors_created_total[5m])
rate(shield_actors_destroyed_total[5m])
```

## 告警规则

创建 Prometheus 告警规则：

```yaml
groups:
  - name: shield_alerts
    rules:
      - alert: HighCPUUsage
        expr: shield_cpu_usage_percent > 80
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "Shield server high CPU usage"
          description: "CPU usage is above 80% for more than 5 minutes"
      
      - alert: HighMemoryUsage
        expr: shield_memory_usage_bytes / shield_memory_total_bytes * 100 > 85
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "Shield server high memory usage"
          description: "Memory usage is above 85% for more than 5 minutes"
      
      - alert: TooManyConnections
        expr: shield_active_connections > 1000
        for: 2m
        labels:
          severity: critical
        annotations:
          summary: "Too many active connections"
          description: "Active connections exceed 1000"
      
      - alert: SlowRequestProcessing
        expr: histogram_quantile(0.95, rate(shield_request_duration_seconds_bucket[5m])) > 1
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "Slow request processing"
          description: "95th percentile request duration is above 1 second"
```

## 性能注意事项

1. **指标收集开销**: 指标收集会有少量性能开销，建议在生产环境中合理设置收集间隔
2. **内存使用**: 大量指标标签会增加内存使用，避免高基数标签
3. **网络流量**: Push Gateway 模式会产生额外的网络流量
4. **存储**: 高频率的指标收集会增加 Prometheus 的存储需求

## 故障排除

### 常见问题

1. **指标端点无法访问**
   - 检查防火墙设置
   - 确认端口配置正确
   - 验证 Prometheus 组件是否启动

2. **指标数据不更新**
   - 检查收集器是否正常运行
   - 查看日志中的错误信息
   - 确认配置文件语法正确

3. **Push Gateway 连接失败**
   - 验证 Push Gateway 地址和端口
   - 检查网络连接
   - 确认 Push Gateway 服务运行状态

### 调试命令

```bash
# 查看指标端点
curl http://localhost:9090/metrics

# 检查 Push Gateway 状态
curl http://localhost:9091/api/v1/metrics

# 验证 Prometheus 配置
promtool check config prometheus.yml
```