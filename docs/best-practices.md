# 最佳实践

本文档汇总了使用 Shield 框架开发游戏服务器的最佳实践和建议。

## 🏗️ 架构设计最佳实践

### 1. 服务拆分原则

**按业务功能拆分服务:**
```yaml
# 推荐的服务拆分
services:
  user-service:      # 用户管理服务
    - 用户注册登录
    - 用户信息管理
    - 好友系统
    
  game-service:      # 游戏逻辑服务  
    - 游戏房间管理
    - 游戏状态同步
    - 游戏规则处理
    
  chat-service:      # 聊天服务
    - 频道管理
    - 消息分发
    - 内容过滤
    
  item-service:      # 物品服务
    - 物品管理
    - 背包系统
    - 交易系统
```

**避免过度拆分:**
```cpp
// ❌ 不好的拆分 - 过于细粒度
class UserNameService {};
class UserAvatarService {};
class UserLevelService {};

// ✅ 好的拆分 - 合理的粒度
class UserService {
    void manage_profile();
    void handle_level_system();  
    void process_avatar_changes();
};
```

### 2. Actor 设计模式

**单一职责原则:**
```lua
-- ✅ 好的 Actor 设计
-- scripts/player_combat_actor.lua
local combat_state = {
    hp = 100,
    mp = 50,
    skills = {},
    buffs = {}
}

function handle_combat_action(action)
    if action.type == "attack" then
        return process_attack(action)
    elseif action.type == "use_skill" then
        return process_skill(action)
    end
end

-- ❌ 不好的设计 - 职责过多
-- scripts/player_everything_actor.lua (避免这样做)
-- 包含战斗、背包、好友、聊天等所有功能
```

**状态管理:**
```lua
-- ✅ 状态封装和验证
local player_state = {
    -- 私有状态
    _internal = {
        last_save_time = 0,
        dirty_flags = {}
    },
    
    -- 公开状态
    player_id = "",
    level = 1,
    experience = 0
}

function update_experience(exp_gain)
    -- 验证输入
    if exp_gain <= 0 then
        return false, "经验值必须大于0"
    end
    
    -- 更新状态
    player_state.experience = player_state.experience + exp_gain
    player_state._internal.dirty_flags.experience = true
    
    -- 检查升级
    check_level_up()
    
    return true
end
```

## 🚀 性能优化最佳实践

### 1. 内存管理

**对象池化:**
```cpp
// ✅ 使用对象池减少内存分配
class MessagePool {
public:
    template<typename T>
    std::unique_ptr<T> acquire() {
        if (!pool_.empty()) {
            auto obj = std::move(pool_.back());
            pool_.pop_back();
            return obj;
        }
        return std::make_unique<T>();
    }
    
    template<typename T>
    void release(std::unique_ptr<T> obj) {
        obj->reset();  // 重置对象状态
        pool_.push_back(std::move(obj));
    }

private:
    std::vector<std::unique_ptr<Message>> pool_;
};
```

**避免频繁的字符串操作:**
```cpp
// ❌ 频繁的字符串拼接
std::string build_response() {
    std::string result = "{";
    result += "\"status\":\"ok\",";
    result += "\"data\":{";
    result += "\"id\":" + std::to_string(id) + ",";
    result += "\"name\":\"" + name + "\"";
    result += "}}";
    return result;
}

// ✅ 使用字符串流或预分配
std::string build_response() {
    std::ostringstream oss;
    oss << "{\"status\":\"ok\",\"data\":"
        << "{\"id\":" << id << ",\"name\":\"" << name << "\"}}";
    return oss.str();
}
```

### 2. 网络优化

**批量处理:**
```cpp
// ✅ 批量发送消息
class BatchMessageSender {
public:
    void queue_message(const std::string& message) {
        batch_buffer_.push_back(message);
        
        if (batch_buffer_.size() >= batch_size_ || 
            should_flush_by_time()) {
            flush();
        }
    }
    
private:
    void flush() {
        if (batch_buffer_.empty()) return;
        
        // 合并所有消息
        std::string combined;
        for (const auto& msg : batch_buffer_) {
            combined += msg + "\n";
        }
        
        // 一次性发送
        send_to_network(combined);
        batch_buffer_.clear();
    }
    
    std::vector<std::string> batch_buffer_;
    size_t batch_size_ = 50;
};
```

**连接池化:**
```cpp
// ✅ 数据库连接池
class DatabaseConnectionPool {
public:
    class ConnectionGuard {
    public:
        ~ConnectionGuard() {
            if (conn_ && pool_) {
                pool_->return_connection(std::move(conn_));
            }
        }
        
        DatabaseConnection* operator->() { return conn_.get(); }
        
    private:
        std::unique_ptr<DatabaseConnection> conn_;
        DatabaseConnectionPool* pool_;
    };
    
    ConnectionGuard acquire_connection() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (!available_connections_.empty()) {
            auto conn = std::move(available_connections_.back());
            available_connections_.pop_back();
            return ConnectionGuard(std::move(conn), this);
        }
        
        // 创建新连接
        return ConnectionGuard(create_new_connection(), this);
    }
};
```

## 🛡️ 安全最佳实践

### 1. 输入验证

**Lua 脚本输入验证:**
```lua
-- ✅ 严格的输入验证
function handle_player_move(msg)
    -- 验证消息结构
    if not msg.data or type(msg.data) ~= "table" then
        return create_error("invalid_message_format")
    end
    
    -- 验证坐标
    local x = tonumber(msg.data.x)
    local y = tonumber(msg.data.y)
    
    if not x or not y then
        return create_error("invalid_coordinates")
    end
    
    -- 范围检查
    if x < 0 or x > MAX_X or y < 0 or y > MAX_Y then
        return create_error("coordinates_out_of_bounds")
    end
    
    -- 移动速度检查
    local distance = calculate_distance(current_pos, {x = x, y = y})
    local time_delta = get_time() - last_move_time
    local max_distance = MAX_SPEED * time_delta
    
    if distance > max_distance then
        return create_error("movement_too_fast")
    end
    
    -- 执行移动
    return execute_move(x, y)
end
```

**C++ 层数据验证:**
```cpp
// ✅ 多层验证
class RequestValidator {
public:
    bool validate_json_request(const nlohmann::json& request) {
        // 基础结构验证
        if (!request.contains("type") || !request.contains("data")) {
            return false;
        }
        
        // 类型验证
        if (!request["type"].is_string()) {
            return false;
        }
        
        // 大小限制
        if (request.dump().size() > MAX_REQUEST_SIZE) {
            return false;
        }
        
        // 业务层验证
        return validate_business_logic(request);
    }
    
private:
    static constexpr size_t MAX_REQUEST_SIZE = 64 * 1024; // 64KB
};
```

### 2. 防作弊机制

**服务器端验证:**
```lua
-- ✅ 服务器权威验证
function validate_action(action)
    -- 权限检查
    if not has_permission(player_id, action.type) then
        log_warning("权限不足: player=" .. player_id .. ", action=" .. action.type)
        return false
    end
    
    -- 冷却时间检查
    local last_action_time = get_last_action_time(action.type)
    local cooldown = get_action_cooldown(action.type)
    
    if get_time() - last_action_time < cooldown then
        log_warning("动作冷却中: player=" .. player_id .. ", action=" .. action.type)
        return false
    end
    
    -- 资源消耗检查
    if not has_enough_resources(action.cost) then
        log_warning("资源不足: player=" .. player_id .. ", required=" .. json_encode(action.cost))
        return false
    end
    
    return true
end
```

**频率限制:**
```cpp
// ✅ 请求频率限制
class RateLimiter {
public:
    bool allow_request(const std::string& client_id) {
        auto now = std::chrono::steady_clock::now();
        auto& client_data = client_requests_[client_id];
        
        // 清理过期请求
        while (!client_data.empty() && 
               now - client_data.front() > time_window_) {
            client_data.pop();
        }
        
        // 检查频率限制
        if (client_data.size() >= max_requests_) {
            return false;
        }
        
        client_data.push(now);
        return true;
    }
    
private:
    std::unordered_map<std::string, std::queue<std::chrono::steady_clock::time_point>> client_requests_;
    std::chrono::seconds time_window_{60};  // 时间窗口
    size_t max_requests_ = 100;             // 最大请求数
};
```

## 📊 监控和日志最佳实践

### 1. 结构化日志

**统一的日志格式:**
```cpp
// ✅ 结构化日志记录
class StructuredLogger {
public:
    template<typename... Args>
    void log_event(const std::string& event_type, Args&&... args) {
        nlohmann::json log_entry = {
            {"timestamp", get_timestamp()},
            {"level", "info"},
            {"event_type", event_type},
            {"server_id", server_id_},
            {"thread_id", std::this_thread::get_id()}
        };
        
        // 添加额外字段
        add_fields(log_entry, std::forward<Args>(args)...);
        
        SHIELD_LOG_INFO << log_entry.dump();
    }
    
    void log_player_action(const std::string& player_id, 
                          const std::string& action,
                          const nlohmann::json& details = {}) {
        log_event("player_action",
            "player_id", player_id,
            "action", action,
            "details", details);
    }
};
```

### 2. 性能指标收集

**关键指标监控:**
```cpp
// ✅ 指标收集
class MetricsCollector {
public:
    void record_request_duration(const std::string& endpoint, 
                                std::chrono::milliseconds duration) {
        request_durations_[endpoint].push_back(duration.count());
        
        // 定期计算统计信息
        if (should_calculate_stats()) {
            calculate_and_report_stats();
        }
    }
    
    void increment_counter(const std::string& metric_name) {
        counters_[metric_name]++;
    }
    
    void set_gauge(const std::string& metric_name, double value) {
        gauges_[metric_name] = value;
    }
    
private:
    void calculate_and_report_stats() {
        for (const auto& [endpoint, durations] : request_durations_) {
            if (durations.empty()) continue;
            
            auto sorted = durations;
            std::sort(sorted.begin(), sorted.end());
            
            nlohmann::json stats = {
                {"endpoint", endpoint},
                {"count", sorted.size()},
                {"avg", calculate_average(sorted)},
                {"p50", calculate_percentile(sorted, 0.5)},
                {"p95", calculate_percentile(sorted, 0.95)},
                {"p99", calculate_percentile(sorted, 0.99)}
            };
            
            report_metrics(stats);
        }
    }
};
```

## 🧪 测试最佳实践

### 1. 单元测试

**Actor 测试:**
```cpp
// ✅ Actor 单元测试
BOOST_AUTO_TEST_CASE(test_player_level_up) {
    // 准备测试环境
    auto vm_pool = create_test_vm_pool();
    auto actor_system = create_test_actor_system();
    
    // 创建测试 Actor
    auto player_actor = create_lua_actor(
        system, *vm_pool, *actor_system, 
        "test_scripts/player_actor.lua", "test_player");
    
    // 测试升级逻辑
    LuaMessage exp_msg;
    exp_msg.type = "add_experience";
    exp_msg.data = {{"exp", "1000"}};
    
    auto response = send_message_sync(player_actor, exp_msg);
    
    BOOST_CHECK(response.success);
    BOOST_CHECK_EQUAL(response.data.at("level"), "2");
}
```

### 2. 集成测试

**端到端测试:**
```cpp
// ✅ 集成测试
BOOST_AUTO_TEST_CASE(test_game_flow_integration) {
    // 启动测试服务器
    auto test_server = start_test_server();
    
    // 模拟客户端连接
    auto client1 = create_test_client("player1");
    auto client2 = create_test_client("player2");
    
    // 测试完整游戏流程
    client1.send_login_request();
    client2.send_login_request();
    
    // 验证登录成功
    BOOST_CHECK(client1.wait_for_login_success());
    BOOST_CHECK(client2.wait_for_login_success());
    
    // 测试游戏互动
    client1.send_attack_action("player2");
    
    // 验证双方都收到战斗事件
    BOOST_CHECK(client1.wait_for_combat_event());
    BOOST_CHECK(client2.wait_for_combat_event());
}
```

## 🚀 部署最佳实践

### 1. 配置管理

**环境特定配置:**
```yaml
# config/production.yaml
gateway:
  listener:
    host: "0.0.0.0"
    tcp_port: 8080
    
  threading:
    io_threads: 16  # 生产环境使用更多线程
    
logger:
  level: info       # 生产环境使用 info 级别
  file_output: true
  max_file_size: 100MB
  
# config/development.yaml  
gateway:
  threading:
    io_threads: 4   # 开发环境使用较少线程
    
logger:
  level: debug      # 开发环境使用 debug 级别
  console: true
```

### 2. 健康检查

**完整的健康检查端点:**
```cpp
// ✅ 健康检查实现
class HealthChecker {
public:
    nlohmann::json get_health_status() {
        nlohmann::json health = {
            {"status", "healthy"},
            {"timestamp", get_timestamp()},
            {"version", get_version()},
            {"uptime", get_uptime_seconds()}
        };
        
        // 检查各个组件
        health["components"] = {
            {"database", check_database_health()},
            {"redis", check_redis_health()},
            {"actor_system", check_actor_system_health()},
            {"lua_vm_pool", check_lua_vm_pool_health()}
        };
        
        // 系统资源检查
        health["resources"] = {
            {"memory_usage", get_memory_usage_percent()},
            {"cpu_usage", get_cpu_usage_percent()},
            {"disk_usage", get_disk_usage_percent()}
        };
        
        // 确定整体状态
        health["status"] = determine_overall_health(health);
        
        return health;
    }
};
```

遵循这些最佳实践，可以帮助您构建出高性能、安全可靠的游戏服务器系统。