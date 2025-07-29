# Serialization 序列化模块 API 文档

序列化模块为 Shield 框架提供高效的数据序列化和反序列化功能，支持 JSON、二进制、Protocol Buffers 等多种格式，用于网络传输和数据持久化。

## 📋 模块概览

序列化模块包含以下主要类：

- `ISerializer`: 序列化器接口基类
- `JsonSerializer`: JSON 序列化器
- `BinarySerializer`: 二进制序列化器
- `ProtobufSerializer`: Protocol Buffers 序列化器
- `SerializerFactory`: 序列化器工厂

## 🔧 ISerializer 序列化器接口

定义了所有序列化器的统一接口，支持多种数据格式的转换。

### 接口定义

```cpp
namespace shield::serialization {

enum class SerializationFormat {
    JSON,           // JSON 格式
    BINARY,         // 二进制格式
    PROTOBUF,       // Protocol Buffers
    MSGPACK,        // MessagePack
    CUSTOM          // 自定义格式
};

// 序列化错误类型
enum class SerializationError {
    SUCCESS = 0,
    INVALID_INPUT,
    INVALID_FORMAT,
    BUFFER_TOO_SMALL,
    UNSUPPORTED_TYPE,
    PARSING_ERROR,
    ENCODING_ERROR
};

// 序列化结果
struct SerializationResult {
    SerializationError error = SerializationError::SUCCESS;
    std::vector<uint8_t> data;
    std::string error_message;
    
    bool is_success() const { return error == SerializationError::SUCCESS; }
    explicit operator bool() const { return is_success(); }
};

// 反序列化结果
template<typename T>
struct DeserializationResult {
    SerializationError error = SerializationError::SUCCESS;
    T data;
    std::string error_message;
    
    bool is_success() const { return error == SerializationError::SUCCESS; }
    explicit operator bool() const { return is_success(); }
};

class ISerializer {
public:
    virtual ~ISerializer() = default;
    
    // 格式信息
    virtual SerializationFormat get_format() const = 0;
    virtual std::string get_format_name() const = 0;
    virtual std::string get_mime_type() const = 0;
    
    // 基础序列化/反序列化
    virtual SerializationResult serialize(const nlohmann::json& json) = 0;
    virtual DeserializationResult<nlohmann::json> deserialize(const std::vector<uint8_t>& data) = 0;
    
    // 类型化序列化 (模板方法)
    template<typename T>
    SerializationResult serialize_object(const T& object);
    
    template<typename T>
    DeserializationResult<T> deserialize_object(const std::vector<uint8_t>& data);
    
    // 流式序列化
    virtual bool serialize_to_stream(const nlohmann::json& json, std::ostream& stream) = 0;
    virtual DeserializationResult<nlohmann::json> deserialize_from_stream(std::istream& stream) = 0;
    
    // 配置选项
    virtual void set_option(const std::string& key, const std::string& value) {}
    virtual std::string get_option(const std::string& key) const { return ""; }
    
    // 性能统计
    struct Statistics {
        std::atomic<uint64_t> serialize_count{0};
        std::atomic<uint64_t> deserialize_count{0};
        std::atomic<uint64_t> serialize_bytes{0};
        std::atomic<uint64_t> deserialize_bytes{0};
        std::atomic<uint64_t> serialize_time_us{0};  // 微秒
        std::atomic<uint64_t> deserialize_time_us{0};
        std::atomic<uint64_t> error_count{0};
    };
    
    virtual const Statistics& get_statistics() const = 0;
    virtual void reset_statistics() = 0;
};

// 序列化特化模板 (用户可以特化)
template<typename T>
struct SerializationTraits {
    static nlohmann::json to_json(const T& obj);
    static T from_json(const nlohmann::json& json);
};

} // namespace shield::serialization
```

## 📄 JsonSerializer JSON 序列化器

基于 nlohmann::json 的高性能 JSON 序列化器。

### 类定义

```cpp
namespace shield::serialization {

struct JsonConfig {
    bool pretty_print = false;                  // 美化输出
    int indent = 2;                            // 缩进空格数
    bool allow_comments = false;               // 允许注释
    bool ignore_unknown_fields = true;        // 忽略未知字段
    size_t max_depth = 64;                     // 最大嵌套深度
    size_t max_string_length = 1024 * 1024;   // 最大字符串长度
    std::string null_value = "null";          // null 值表示
    std::string datetime_format = "iso8601";   // 日期时间格式
};

class JsonSerializer : public ISerializer {
public:
    explicit JsonSerializer(const JsonConfig& config = JsonConfig{});
    virtual ~JsonSerializer();
    
    // ISerializer 接口实现
    SerializationFormat get_format() const override;
    std::string get_format_name() const override;
    std::string get_mime_type() const override;
    
    SerializationResult serialize(const nlohmann::json& json) override;
    DeserializationResult<nlohmann::json> deserialize(const std::vector<uint8_t>& data) override;
    
    bool serialize_to_stream(const nlohmann::json& json, std::ostream& stream) override;
    DeserializationResult<nlohmann::json> deserialize_from_stream(std::istream& stream) override;
    
    void set_option(const std::string& key, const std::string& value) override;
    std::string get_option(const std::string& key) const override;
    
    const Statistics& get_statistics() const override;
    void reset_statistics() override;
    
    // JSON 特定方法
    SerializationResult serialize_pretty(const nlohmann::json& json);
    SerializationResult serialize_compact(const nlohmann::json& json);
    
    // 验证功能
    bool validate_json(const std::string& json_str);
    std::vector<std::string> get_validation_errors(const std::string& json_str);
    
    // Schema 验证 (可选)
    void set_schema(const nlohmann::json& schema);
    bool validate_with_schema(const nlohmann::json& json);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::serialization
```

### 使用示例

```cpp
// 创建 JSON 序列化器
shield::serialization::JsonConfig json_config;
json_config.pretty_print = true;
json_config.indent = 4;
json_config.allow_comments = true;

auto json_serializer = std::make_unique<shield::serialization::JsonSerializer>(json_config);

// 序列化游戏数据
nlohmann::json player_data = {
    {"player_id", "player_001"},
    {"name", "张三"},
    {"level", 15},
    {"experience", 2500},
    {"position", {
        {"x", 100.5},
        {"y", 200.3},
        {"z", 0.0}
    }},
    {"inventory", {
        {{"item_id", "sword_001"}, {"quantity", 1}},
        {{"item_id", "potion_health"}, {"quantity", 5}}
    }},
    {"stats", {
        {"health", 100},
        {"mana", 50},
        {"strength", 20},
        {"intelligence", 15}
    }}
};

// 序列化
auto serialize_result = json_serializer->serialize(player_data);
if (serialize_result.is_success()) {
    SHIELD_LOG_INFO << "序列化成功，大小: " << serialize_result.data.size() << " 字节";
    
    // 转换为字符串查看
    std::string json_str(serialize_result.data.begin(), serialize_result.data.end());
    SHIELD_LOG_INFO << "JSON 数据:\n" << json_str;
} else {
    SHIELD_LOG_ERROR << "序列化失败: " << serialize_result.error_message;
}

// 反序列化
auto deserialize_result = json_serializer->deserialize(serialize_result.data);
if (deserialize_result.is_success()) {
    SHIELD_LOG_INFO << "反序列化成功";
    
    // 验证数据
    assert(deserialize_result.data["player_id"] == "player_001");
    assert(deserialize_result.data["level"] == 15);
    assert(deserialize_result.data["position"]["x"] == 100.5);
} else {
    SHIELD_LOG_ERROR << "反序列化失败: " << deserialize_result.error_message;
}

// 使用类型化序列化
struct PlayerInfo {
    std::string player_id;
    std::string name;
    int level;
    double experience;
};

// 特化序列化特征
template<>
struct shield::serialization::SerializationTraits<PlayerInfo> {
    static nlohmann::json to_json(const PlayerInfo& player) {
        return nlohmann::json{
            {"player_id", player.player_id},
            {"name", player.name},
            {"level", player.level},
            {"experience", player.experience}
        };
    }
    
    static PlayerInfo from_json(const nlohmann::json& json) {
        PlayerInfo player;
        player.player_id = json["player_id"];
        player.name = json["name"];
        player.level = json["level"];
        player.experience = json["experience"];
        return player;
    }
};

// 使用类型化序列化
PlayerInfo player_info{"player_002", "李四", 20, 5000.0};
auto typed_result = json_serializer->serialize_object(player_info);
if (typed_result.is_success()) {
    auto restored_player = json_serializer->deserialize_object<PlayerInfo>(typed_result.data);
    if (restored_player.is_success()) {
        SHIELD_LOG_INFO << "类型化序列化成功: " << restored_player.data.name;
    }
}
```

## 📦 BinarySerializer 二进制序列化器

高效的二进制序列化器，适合网络传输和高性能场景。

### 类定义

```cpp
namespace shield::serialization {

struct BinaryConfig {
    bool use_compression = false;              // 使用压缩
    std::string compression_algorithm = "lz4"; // 压缩算法 (lz4, zstd, gzip)
    int compression_level = 1;                 // 压缩级别
    bool use_encryption = false;               // 使用加密
    std::string encryption_key;                // 加密密钥
    bool include_metadata = true;              // 包含元数据
    uint32_t magic_number = 0x53484C44;       // 魔数 "SHLD"
    uint16_t version = 1;                      // 版本号
};

// 二进制数据头部
struct BinaryHeader {
    uint32_t magic;                           // 魔数
    uint16_t version;                         // 版本
    uint16_t flags;                           // 标志位
    uint32_t data_size;                       // 数据大小
    uint32_t checksum;                        // 校验和
    uint64_t timestamp;                       // 时间戳
};

class BinarySerializer : public ISerializer {
public:
    explicit BinarySerializer(const BinaryConfig& config = BinaryConfig{});
    virtual ~BinarySerializer();
    
    // ISerializer 接口实现
    SerializationFormat get_format() const override;
    std::string get_format_name() const override;
    std::string get_mime_type() const override;
    
    SerializationResult serialize(const nlohmann::json& json) override;
    DeserializationResult<nlohmann::json> deserialize(const std::vector<uint8_t>& data) override;
    
    bool serialize_to_stream(const nlohmann::json& json, std::ostream& stream) override;
    DeserializationResult<nlohmann::json> deserialize_from_stream(std::istream& stream) override;
    
    const Statistics& get_statistics() const override;
    void reset_statistics() override;
    
    // 二进制特定方法
    SerializationResult serialize_with_header(const nlohmann::json& json);
    bool validate_header(const std::vector<uint8_t>& data);
    BinaryHeader extract_header(const std::vector<uint8_t>& data);
    
    // 原始数据序列化
    template<typename T>
    SerializationResult serialize_raw(const T& data);
    
    template<typename T>
    DeserializationResult<T> deserialize_raw(const std::vector<uint8_t>& data);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::serialization
```

### 使用示例

```cpp
// 创建二进制序列化器
shield::serialization::BinaryConfig binary_config;
binary_config.use_compression = true;
binary_config.compression_algorithm = "lz4";
binary_config.compression_level = 3;
binary_config.include_metadata = true;

auto binary_serializer = std::make_unique<shield::serialization::BinarySerializer>(binary_config);

// 序列化大量游戏状态数据
nlohmann::json game_state = {
    {"world", {
        {"time", 1234567890},
        {"weather", "sunny"},
        {"temperature", 25.5}
    }},
    {"players", nlohmann::json::array()},
    {"npcs", nlohmann::json::array()},
    {"objects", nlohmann::json::array()}
};

// 添加大量玩家数据
for (int i = 0; i < 1000; ++i) {
    game_state["players"].push_back({
        {"id", "player_" + std::to_string(i)},
        {"name", "Player" + std::to_string(i)},
        {"level", i % 100 + 1},
        {"position", {{"x", i * 10.0}, {"y", i * 5.0}, {"z", 0.0}}},
        {"stats", {{"hp", 100}, {"mp", 50}}}
    });
}

// 二进制序列化
auto binary_result = binary_serializer->serialize_with_header(game_state);
if (binary_result.is_success()) {
    SHIELD_LOG_INFO << "二进制序列化成功，压缩后大小: " << binary_result.data.size() << " 字节";
    
    // 验证头部
    if (binary_serializer->validate_header(binary_result.data)) {
        auto header = binary_serializer->extract_header(binary_result.data);
        SHIELD_LOG_INFO << "魔数: 0x" << std::hex << header.magic;
        SHIELD_LOG_INFO << "版本: " << header.version;
        SHIELD_LOG_INFO << "数据大小: " << header.data_size;
        SHIELD_LOG_INFO << "时间戳: " << header.timestamp;
    }
} else {
    SHIELD_LOG_ERROR << "二进制序列化失败: " << binary_result.error_message;
}

// 反序列化
auto binary_deserialize_result = binary_serializer->deserialize(binary_result.data);
if (binary_deserialize_result.is_success()) {
    SHIELD_LOG_INFO << "二进制反序列化成功";
    SHIELD_LOG_INFO << "玩家数量: " << binary_deserialize_result.data["players"].size();
} else {
    SHIELD_LOG_ERROR << "二进制反序列化失败: " << binary_deserialize_result.error_message;
}

// 原始数据序列化示例
struct NetworkPacket {
    uint32_t packet_id;
    uint16_t packet_type;
    uint16_t data_length;
    std::array<uint8_t, 1024> data;
};

NetworkPacket packet;
packet.packet_id = 12345;
packet.packet_type = 1;  // PLAYER_MOVE
packet.data_length = 16;

// 序列化原始结构
auto raw_result = binary_serializer->serialize_raw(packet);
if (raw_result.is_success()) {
    SHIELD_LOG_INFO << "原始数据序列化成功，大小: " << raw_result.data.size() << " 字节";
    
    // 反序列化
    auto restored_packet = binary_serializer->deserialize_raw<NetworkPacket>(raw_result.data);
    if (restored_packet.is_success()) {
        SHIELD_LOG_INFO << "还原包 ID: " << restored_packet.data.packet_id;
        SHIELD_LOG_INFO << "包类型: " << restored_packet.data.packet_type;
    }
}
```

## 🚀 SerializerFactory 序列化器工厂

管理多种序列化器实例，提供统一的创建和访问接口。

### 类定义

```cpp
namespace shield::serialization {

class SerializerFactory {
public:
    static SerializerFactory& instance();
    
    // 序列化器注册
    void register_serializer(SerializationFormat format, 
                            std::function<std::unique_ptr<ISerializer>()> creator);
    
    template<typename SerializerType>
    void register_serializer(SerializationFormat format);
    
    // 序列化器创建
    std::unique_ptr<ISerializer> create_serializer(SerializationFormat format);
    std::unique_ptr<ISerializer> create_serializer(const std::string& format_name);
    
    // 格式检测
    std::optional<SerializationFormat> detect_format(const std::vector<uint8_t>& data);
    std::optional<SerializationFormat> detect_format_by_mime_type(const std::string& mime_type);
    
    // 支持的格式
    std::vector<SerializationFormat> get_supported_formats();
    std::vector<std::string> get_supported_format_names();
    
    // 全局配置
    void set_default_format(SerializationFormat format);
    SerializationFormat get_default_format() const;
    
    // 统计信息
    struct FactoryStatistics {
        std::map<SerializationFormat, uint64_t> serializer_usage;
        std::atomic<uint64_t> total_created{0};
    };
    
    const FactoryStatistics& get_statistics() const;

private:
    SerializerFactory() = default;
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// 便捷的全局函数
std::unique_ptr<ISerializer> create_json_serializer(const JsonConfig& config = {});
std::unique_ptr<ISerializer> create_binary_serializer(const BinaryConfig& config = {});

// 快速序列化/反序列化函数
SerializationResult quick_serialize(const nlohmann::json& json, 
                                   SerializationFormat format = SerializationFormat::JSON);

DeserializationResult<nlohmann::json> quick_deserialize(const std::vector<uint8_t>& data,
                                                       SerializationFormat format = SerializationFormat::JSON);

} // namespace shield::serialization
```

### 使用示例

```cpp
// 注册序列化器
auto& factory = shield::serialization::SerializerFactory::instance();

// 注册内置序列化器
factory.register_serializer<shield::serialization::JsonSerializer>(
    shield::serialization::SerializationFormat::JSON);

factory.register_serializer<shield::serialization::BinarySerializer>(
    shield::serialization::SerializationFormat::BINARY);

// 注册自定义序列化器
factory.register_serializer(shield::serialization::SerializationFormat::CUSTOM, 
    []() -> std::unique_ptr<shield::serialization::ISerializer> {
        return std::make_unique<MyCustomSerializer>();
    });

// 设置默认格式
factory.set_default_format(shield::serialization::SerializationFormat::JSON);

// 多格式序列化测试
void test_multiple_formats(const nlohmann::json& test_data) {
    auto formats = factory.get_supported_formats();
    
    for (auto format : formats) {
        auto serializer = factory.create_serializer(format);
        if (!serializer) continue;
        
        SHIELD_LOG_INFO << "测试格式: " << serializer->get_format_name();
        
        // 序列化
        auto start_time = std::chrono::high_resolution_clock::now();
        auto serialize_result = serializer->serialize(test_data);
        auto serialize_time = std::chrono::high_resolution_clock::now() - start_time;
        
        if (serialize_result.is_success()) {
            SHIELD_LOG_INFO << "  序列化成功，大小: " << serialize_result.data.size() 
                           << " 字节，耗时: " 
                           << std::chrono::duration_cast<std::chrono::microseconds>(serialize_time).count() 
                           << " μs";
            
            // 反序列化
            start_time = std::chrono::high_resolution_clock::now();
            auto deserialize_result = serializer->deserialize(serialize_result.data);
            auto deserialize_time = std::chrono::high_resolution_clock::now() - start_time;
            
            if (deserialize_result.is_success()) {
                SHIELD_LOG_INFO << "  反序列化成功，耗时: " 
                               << std::chrono::duration_cast<std::chrono::microseconds>(deserialize_time).count() 
                               << " μs";
            } else {
                SHIELD_LOG_ERROR << "  反序列化失败: " << deserialize_result.error_message;
            }
        } else {
            SHIELD_LOG_ERROR << "  序列化失败: " << serialize_result.error_message;
        }
    }
}

// 自动格式检测
void handle_unknown_data(const std::vector<uint8_t>& data) {
    auto& factory = shield::serialization::SerializerFactory::instance();
    
    // 尝试自动检测格式
    auto detected_format = factory.detect_format(data);
    if (detected_format) {
        SHIELD_LOG_INFO << "检测到数据格式: " << static_cast<int>(*detected_format);
        
        auto serializer = factory.create_serializer(*detected_format);
        if (serializer) {
            auto result = serializer->deserialize(data);
            if (result.is_success()) {
                SHIELD_LOG_INFO << "自动反序列化成功";
                process_deserialized_data(result.data);
            }
        }
    } else {
        SHIELD_LOG_WARN << "无法检测数据格式";
    }
}

// 快速序列化示例
void quick_serialization_example() {
    nlohmann::json data = {
        {"message", "Hello, World!"},
        {"timestamp", std::time(nullptr)},
        {"values", {1, 2, 3, 4, 5}}
    };
    
    // 快速 JSON 序列化
    auto json_result = shield::serialization::quick_serialize(data, 
        shield::serialization::SerializationFormat::JSON);
    
    if (json_result.is_success()) {
        std::string json_str(json_result.data.begin(), json_result.data.end());
        SHIELD_LOG_INFO << "快速 JSON 序列化: " << json_str;
    }
    
    // 快速二进制序列化
    auto binary_result = shield::serialization::quick_serialize(data, 
        shield::serialization::SerializationFormat::BINARY);
    
    if (binary_result.is_success()) {
        SHIELD_LOG_INFO << "快速二进制序列化，大小: " << binary_result.data.size() << " 字节";
        
        // 快速反序列化
        auto restored = shield::serialization::quick_deserialize(binary_result.data,
            shield::serialization::SerializationFormat::BINARY);
            
        if (restored.is_success()) {
            SHIELD_LOG_INFO << "快速反序列化成功: " << restored.data["message"];
        }
    }
}
```

## 📚 最佳实践

### 1. 性能优化

```cpp
// ✅ 高性能序列化实践
class HighPerformanceSerializer {
public:
    void optimize_for_performance() {
        // 1. 对象池化，减少内存分配
        setup_object_pools();
        
        // 2. 预分配缓冲区
        setup_buffer_pools();
        
        // 3. 批量序列化
        setup_batch_processing();
        
        // 4. 选择合适的序列化格式
        choose_optimal_format();
    }

private:
    void setup_object_pools() {
        // JSON 对象池
        json_pool_ = std::make_unique<ObjectPool<nlohmann::json>>(100);
        
        // 序列化器池
        json_serializer_pool_ = std::make_unique<ObjectPool<JsonSerializer>>(10);
        binary_serializer_pool_ = std::make_unique<ObjectPool<BinarySerializer>>(10);
    }
    
    void setup_buffer_pools() {
        // 预分配不同大小的缓冲区
        small_buffer_pool_ = std::make_unique<BufferPool>(1024, 100);      // 1KB * 100
        medium_buffer_pool_ = std::make_unique<BufferPool>(64 * 1024, 50); // 64KB * 50
        large_buffer_pool_ = std::make_unique<BufferPool>(1024 * 1024, 10);// 1MB * 10
    }
    
    SerializationResult optimize_serialize(const nlohmann::json& data) {
        // 根据数据大小选择合适的序列化方式
        size_t estimated_size = estimate_serialized_size(data);
        
        if (estimated_size < 1024) {
            // 小数据使用 JSON
            return serialize_with_pool(data, SerializationFormat::JSON);
        } else if (estimated_size < 64 * 1024) {
            // 中等数据使用压缩二进制
            return serialize_with_compression(data);
        } else {
            // 大数据使用流式处理
            return serialize_streaming(data);
        }
    }
    
    SerializationResult serialize_with_pool(const nlohmann::json& data, 
                                           SerializationFormat format) {
        // 从池中获取序列化器
        auto serializer = get_pooled_serializer(format);
        auto buffer = get_pooled_buffer(estimate_serialized_size(data));
        
        // 序列化到预分配缓冲区
        auto result = serializer->serialize(data);
        
        // 归还到池
        return_to_pool(serializer);
        return_to_pool(buffer);
        
        return result;
    }
};
```

### 2. 错误处理和数据验证

```cpp
// ✅ 安全的序列化处理
class SafeSerializer {
public:
    SerializationResult safe_serialize(const nlohmann::json& data) {
        try {
            // 1. 数据验证
            if (!validate_input_data(data)) {
                return create_error_result(SerializationError::INVALID_INPUT, 
                    "输入数据验证失败");
            }
            
            // 2. 大小检查
            size_t estimated_size = estimate_size(data);
            if (estimated_size > max_allowed_size_) {
                return create_error_result(SerializationError::BUFFER_TOO_SMALL,
                    "数据太大: " + std::to_string(estimated_size) + " 字节");
            }
            
            // 3. 深度检查
            int depth = calculate_json_depth(data);
            if (depth > max_allowed_depth_) {
                return create_error_result(SerializationError::INVALID_INPUT,
                    "JSON 嵌套过深: " + std::to_string(depth));
            }
            
            // 4. 执行序列化
            auto serializer = create_serializer();
            return serializer->serialize(data);
            
        } catch (const std::bad_alloc& e) {
            return create_error_result(SerializationError::BUFFER_TOO_SMALL, 
                "内存不足");
        } catch (const std::exception& e) {
            return create_error_result(SerializationError::ENCODING_ERROR, 
                std::string("序列化异常: ") + e.what());
        }
    }
    
    DeserializationResult<nlohmann::json> safe_deserialize(const std::vector<uint8_t>& data) {
        try {
            // 1. 数据完整性检查
            if (data.empty()) {
                return create_deserialize_error<nlohmann::json>(
                    SerializationError::INVALID_INPUT, "空数据");
            }
            
            if (data.size() > max_allowed_size_) {
                return create_deserialize_error<nlohmann::json>(
                    SerializationError::BUFFER_TOO_SMALL, "数据过大");
            }
            
            // 2. 格式检测
            auto format = detect_format(data);
            if (!format) {
                return create_deserialize_error<nlohmann::json>(
                    SerializationError::INVALID_FORMAT, "无法识别数据格式");
            }
            
            // 3. 选择合适的反序列化器
            auto serializer = create_serializer(*format);
            if (!serializer) {
                return create_deserialize_error<nlohmann::json>(
                    SerializationError::UNSUPPORTED_TYPE, "不支持的格式");
            }
            
            // 4. 执行反序列化
            auto result = serializer->deserialize(data);
            
            // 5. 结果验证
            if (result.is_success()) {
                if (!validate_deserialized_data(result.data)) {
                    return create_deserialize_error<nlohmann::json>(
                        SerializationError::PARSING_ERROR, "反序列化结果验证失败");
                }
            }
            
            return result;
            
        } catch (const std::exception& e) {
            return create_deserialize_error<nlohmann::json>(
                SerializationError::PARSING_ERROR, 
                std::string("反序列化异常: ") + e.what());
        }
    }

private:
    bool validate_input_data(const nlohmann::json& data) {
        // 检查 JSON 结构合法性
        if (data.is_discarded()) return false;
        
        // 检查是否包含循环引用
        if (has_circular_reference(data)) return false;
        
        // 检查字符串长度
        return check_string_lengths(data);
    }
    
    bool has_circular_reference(const nlohmann::json& data, std::set<const void*>& visited = {}) {
        const void* addr = &data;
        if (visited.count(addr)) return true;
        
        visited.insert(addr);
        
        if (data.is_object()) {
            for (const auto& [key, value] : data.items()) {
                if (has_circular_reference(value, visited)) return true;
            }
        } else if (data.is_array()) {
            for (const auto& item : data) {
                if (has_circular_reference(item, visited)) return true;
            }
        }
        
        visited.erase(addr);
        return false;
    }
    
    size_t max_allowed_size_ = 100 * 1024 * 1024;  // 100MB
    int max_allowed_depth_ = 64;
};
```

### 3. 自定义序列化器

```cpp
// ✅ 自定义高效序列化器
class GameProtocolSerializer : public ISerializer {
public:
    SerializationFormat get_format() const override {
        return SerializationFormat::CUSTOM;
    }
    
    std::string get_format_name() const override {
        return "game_protocol";
    }
    
    SerializationResult serialize(const nlohmann::json& json) override {
        SerializationResult result;
        
        try {
            std::vector<uint8_t> buffer;
            buffer.reserve(1024);  // 预分配
            
            // 写入协议头
            write_header(buffer, json);
            
            // 根据消息类型使用不同的序列化策略
            std::string msg_type = json.value("type", "");
            
            if (msg_type == "player_move") {
                serialize_player_move(buffer, json);
            } else if (msg_type == "player_attack") {
                serialize_player_attack(buffer, json);
            } else if (msg_type == "chat_message") {
                serialize_chat_message(buffer, json);
            } else {
                // 默认使用 JSON
                serialize_json_fallback(buffer, json);
            }
            
            // 计算并写入校验和
            write_checksum(buffer);
            
            result.data = std::move(buffer);
            update_statistics(true, result.data.size());
            
        } catch (const std::exception& e) {
            result.error = SerializationError::ENCODING_ERROR;
            result.error_message = e.what();
            update_statistics(false, 0);
        }
        
        return result;
    }

private:
    void serialize_player_move(std::vector<uint8_t>& buffer, const nlohmann::json& json) {
        // 高效的二进制编码
        write_string(buffer, json["player_id"]);
        write_float(buffer, json["position"]["x"]);
        write_float(buffer, json["position"]["y"]);
        write_float(buffer, json["position"]["z"]);
        write_uint32(buffer, json["timestamp"]);
    }
    
    void serialize_player_attack(std::vector<uint8_t>& buffer, const nlohmann::json& json) {
        write_string(buffer, json["attacker_id"]);
        write_string(buffer, json["target_id"]);
        write_uint16(buffer, json["skill_id"]);
        write_uint32(buffer, json["damage"]);
        write_uint32(buffer, json["timestamp"]);
    }
    
    void serialize_chat_message(std::vector<uint8_t>& buffer, const nlohmann::json& json) {
        write_string(buffer, json["sender"]);
        write_string(buffer, json["channel"]);
        write_string(buffer, json["message"]);
        write_uint32(buffer, json["timestamp"]);
    }
    
    // 工具函数
    void write_string(std::vector<uint8_t>& buffer, const std::string& str) {
        uint16_t len = static_cast<uint16_t>(str.length());
        write_uint16(buffer, len);
        buffer.insert(buffer.end(), str.begin(), str.end());
    }
    
    void write_uint32(std::vector<uint8_t>& buffer, uint32_t value) {
        buffer.push_back((value >> 24) & 0xFF);
        buffer.push_back((value >> 16) & 0xFF);
        buffer.push_back((value >> 8) & 0xFF);
        buffer.push_back(value & 0xFF);
    }
    
    void write_float(std::vector<uint8_t>& buffer, float value) {
        uint32_t int_value;
        std::memcpy(&int_value, &value, sizeof(float));
        write_uint32(buffer, int_value);
    }
};
```

---

序列化模块为 Shield 框架提供了灵活高效的数据序列化能力，支持多种格式和自定义扩展。通过合理的选择和优化，可以在性能、空间效率和易用性之间找到最佳平衡。