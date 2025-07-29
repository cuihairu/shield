# Shield Universal Serialization System

Shield框架提供了一个强大且灵活的通用序列化系统，支持多种序列化格式，包括JSON、Protobuf、MessagePack等。

## 特性

- **多格式支持**: JSON、Protobuf、MessagePack (sproto 即将支持)
- **自动格式检测**: 根据类型特性自动选择最佳序列化格式
- **类型安全**: 基于C++20概念的编译时类型检查
- **用户友好**: 简洁的API和自动ADL检测
- **高性能**: 零拷贝设计和优化的序列化路径
- **可扩展**: 易于添加新的序列化格式

## 快速开始

### 1. 初始化序列化系统

```cpp
#include "shield/serialization/universal_serialization_system.hpp"

using namespace shield::serialization;

// 配置序列化选项
SerializationConfig config;
config.enable_json = true;
config.enable_protobuf = true;
config.enable_messagepack = true;
config.default_format = SerializationFormat::JSON;
config.enable_auto_format_detection = true;

// 初始化系统
initialize_universal_serialization_system(config);
```

### 2. 定义可序列化的数据结构

#### JSON支持 (使用nlohmann::json)

```cpp
struct Player {
    uint64_t id;
    std::string name;
    int level;
    
    // ADL方式 (推荐)
    friend void to_json(nlohmann::json& j, const Player& p) {
        j = nlohmann::json{{"id", p.id}, {"name", p.name}, {"level", p.level}};
    }
    
    friend void from_json(const nlohmann::json& j, Player& p) {
        j.at("id").get_to(p.id);
        j.at("name").get_to(p.name);
        j.at("level").get_to(p.level);
    }
};
```

#### Protobuf支持

```cpp
class GameMessage : public google::protobuf::MessageLite {
    // 继承自protobuf MessageLite即可
    // 或者实现 SerializeToString/ParseFromString 方法
};
```

#### MessagePack支持

```cpp
struct GameState {
    int64_t timestamp;
    std::string state;
    std::vector<int> player_ids;
};

// 使用msgpack宏
MSGPACK_DEFINE_MAP(GameState, timestamp, state, player_ids);
```

### 3. 序列化和反序列化

#### 自动格式选择

```cpp
Player player{123, "Alice", 42};

// 自动选择最佳格式
auto data = serialize_universal(player);

// 指定格式反序列化
auto restored = deserialize_universal<Player>(data, SerializationFormat::JSON);
```

#### 指定格式

```cpp
// JSON
auto json_str = serialize_as<SerializationFormat::JSON>(player);
auto player1 = deserialize_as<SerializationFormat::JSON, Player>(json_str);

// Protobuf
auto proto_bytes = serialize_as<SerializationFormat::PROTOBUF>(message);
auto message1 = deserialize_as<SerializationFormat::PROTOBUF, GameMessage>(proto_bytes);

// MessagePack
auto msgpack_bytes = serialize_as<SerializationFormat::MESSAGEPACK>(game_state);
auto state1 = deserialize_as<SerializationFormat::MESSAGEPACK, GameState>(msgpack_bytes);
```

#### 便利函数

```cpp
// JSON便利函数
std::string json = to_json_string(player);
Player p = from_json_string<Player>(json);

// Protobuf便利函数
std::vector<uint8_t> bytes = to_protobuf_bytes(message);
GameMessage msg = from_protobuf_bytes<GameMessage>(bytes);

// MessagePack便利函数
std::vector<uint8_t> data = to_messagepack_bytes(state);
GameState gs = from_messagepack_bytes<GameState>(data);
```

## 高级用法

### 格式检测和推荐

```cpp
// 检查类型支持的格式
constexpr bool json_ok = is_json_serializable_v<Player>;
constexpr bool proto_ok = is_protobuf_serializable_v<GameMessage>;
constexpr bool msgpack_ok = is_messagepack_serializable_v<GameState>;

// 获取推荐格式
constexpr auto best_format = detect_best_format<Player>();

// 运行时格式推荐
auto& system = UniversalSerializationSystem::instance();
auto format = system.get_recommended_format<Player>();
```

### 系统信息

```cpp
auto& system = UniversalSerializationSystem::instance();

// 获取系统状态
std::cout << system.get_system_info() << std::endl;

// 获取支持的格式
auto formats = system.get_available_formats();
for (const auto& format : formats) {
    std::cout << "Supported: " << format << std::endl;
}
```

### 错误处理

```cpp
try {
    auto data = serialize_as<SerializationFormat::JSON>(player);
    auto restored = deserialize_as<SerializationFormat::JSON, Player>(data);
} catch (const SerializationException& e) {
    std::cerr << "Serialization failed: " << e.what() << std::endl;
}
```

## 扩展指南

### 添加新的序列化格式

1. 在`SerializationFormat`枚举中添加新格式
2. 实现对应的概念检测 (如`CustomSerializable`)
3. 创建继承自`UniversalSerializer<Format>`的序列化器
4. 在初始化系统中注册新的序列化器

### 自定义类型适配

```cpp
// 为自定义类型添加JSON支持
namespace nlohmann {
    template<>
    struct adl_serializer<MyCustomType> {
        static void to_json(json& j, const MyCustomType& obj) {
            // 实现序列化逻辑
        }
        
        static void from_json(const json& j, MyCustomType& obj) {
            // 实现反序列化逻辑
        }
    };
}
```

## 性能优化建议

1. **选择合适的格式**: 
   - JSON: 人类可读，调试友好
   - Protobuf: 高效，向后兼容
   - MessagePack: 紧凑，性能优秀

2. **避免频繁的格式转换**: 在同一模块内保持格式一致性

3. **使用移动语义**: 大对象序列化时使用`std::move`

4. **批量操作**: 对于大量小对象，考虑批量序列化

## 依赖项

- **nlohmann::json**: JSON支持
- **protobuf**: Protobuf支持  
- **msgpack-cxx**: MessagePack支持
- **C++20**: 概念和模板特性

## 编译选项

```cmake
# 在CMakeLists.txt中启用
find_package(nlohmann_json REQUIRED)
find_package(protobuf REQUIRED)
find_package(msgpack-cxx REQUIRED)

target_link_libraries(your_target 
    nlohmann_json::nlohmann_json
    protobuf::libprotobuf
    msgpack-cxx
)
```

## 示例项目

查看 `examples/serialization_demo.cpp` 了解完整的使用示例。

---

**注意**: 该序列化系统设计为框架级别的通用工具，不包含特定的业务逻辑。用户可以自由定义任何数据结构和序列化方式。