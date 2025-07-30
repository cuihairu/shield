# Shield Universal Serialization System

The Shield framework provides a powerful and flexible universal serialization system that supports multiple serialization formats, including JSON, Protobuf, MessagePack, and more.

## Features

- **Multi-format support**: JSON, Protobuf, MessagePack (sproto coming soon)
- **Automatic format detection**: Automatically selects optimal serialization format based on type traits
- **Type safety**: Compile-time type checking based on C++20 concepts
- **User-friendly**: Simple API and automatic ADL detection
- **High performance**: Zero-copy design and optimized serialization paths
- **Extensible**: Easy to add new serialization formats

## Quick Start

### 1. Initialize Serialization System

```cpp
#include "shield/serialization/universal_serialization_system.hpp"

using namespace shield::serialization;

// Configure serialization options
SerializationConfig config;
config.enable_json = true;
config.enable_protobuf = true;
config.enable_messagepack = true;
config.default_format = SerializationFormat::JSON;
config.enable_auto_format_detection = true;

// Initialize system
initialize_universal_serialization_system(config);
```

### 2. Define Serializable Data Structures

#### JSON Support (using nlohmann::json)

```cpp
struct Player {
    uint64_t id;
    std::string name;
    int level;
    
    // ADL method (recommended)
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

#### Protobuf Support

```cpp
class GameMessage : public google::protobuf::MessageLite {
    // Inherit from protobuf MessageLite
    // Or implement SerializeToString/ParseFromString methods
};
```

#### MessagePack Support

```cpp
struct GameState {
    int64_t timestamp;
    std::string state;
    std::vector<int> player_ids;
};

// Use msgpack macro
MSGPACK_DEFINE_MAP(GameState, timestamp, state, player_ids);
```

### 3. Serialization and Deserialization

#### Automatic Format Selection

```cpp
Player player{123, "Alice", 42};

// Automatically select optimal format
auto data = serialize_universal(player);

// Deserialize with specified format
auto restored = deserialize_universal<Player>(data, SerializationFormat::JSON);
```

#### Specify Format

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

#### Convenience Functions

```cpp
// JSON convenience functions
std::string json = to_json_string(player);
Player p = from_json_string<Player>(json);

// Protobuf convenience functions
std::vector<uint8_t> bytes = to_protobuf_bytes(message);
GameMessage msg = from_protobuf_bytes<GameMessage>(bytes);

// MessagePack convenience functions
std::vector<uint8_t> data = to_messagepack_bytes(state);
GameState gs = from_messagepack_bytes<GameState>(data);
```

## Advanced Usage

### Format Detection and Recommendation

```cpp
// Check formats supported by type
constexpr bool json_ok = is_json_serializable_v<Player>;
constexpr bool proto_ok = is_protobuf_serializable_v<GameMessage>;
constexpr bool msgpack_ok = is_messagepack_serializable_v<GameState>;

// Get recommended format
constexpr auto best_format = detect_best_format<Player>();

// Runtime format recommendation
auto& system = UniversalSerializationSystem::instance();
auto format = system.get_recommended_format<Player>();
```

### System Information

```cpp
auto& system = UniversalSerializationSystem::instance();

// Get system status
std::cout << system.get_system_info() << std::endl;

// Get supported formats
auto formats = system.get_available_formats();
for (const auto& format : formats) {
    std::cout << "Supported: " << format << std::endl;
}
```

### Error Handling

```cpp
try {
    auto data = serialize_as<SerializationFormat::JSON>(player);
    auto restored = deserialize_as<SerializationFormat::JSON, Player>(data);
} catch (const SerializationException& e) {
    std::cerr << "Serialization failed: " << e.what() << std::endl;
}
```

## Extension Guide

### Adding New Serialization Formats

1. Add new format to `SerializationFormat` enum
2. Implement corresponding concept detection (like `CustomSerializable`)
3. Create serializer inheriting from `UniversalSerializer<Format>`
4. Register new serializer in initialization system

### Custom Type Adaptation

```cpp
// Add JSON support for custom types
namespace nlohmann {
    template<>
    struct adl_serializer<MyCustomType> {
        static void to_json(json& j, const MyCustomType& obj) {
            // Implement serialization logic
        }
        
        static void from_json(const json& j, MyCustomType& obj) {
            // Implement deserialization logic
        }
    };
}
```

## Performance Optimization Tips

1. **Choose appropriate format**: 
   - JSON: Human-readable, debug-friendly
   - Protobuf: Efficient, backward compatible
   - MessagePack: Compact, excellent performance

2. **Avoid frequent format conversions**: Maintain format consistency within the same module

3. **Use move semantics**: Use `std::move` when serializing large objects

4. **Batch operations**: For many small objects, consider batch serialization

## Dependencies

- **nlohmann::json**: JSON支持
- **protobuf**: Protobuf支持  
- **msgpack-cxx**: MessagePack支持
- **C++20**: 概念和模板特性

## Compilation Options

```cmake
# Enable in CMakeLists.txt
find_package(nlohmann_json REQUIRED)
find_package(protobuf REQUIRED)
find_package(msgpack-cxx REQUIRED)

target_link_libraries(your_target 
    nlohmann_json::nlohmann_json
    protobuf::libprotobuf
    msgpack-cxx
)
```

## Example Project

See `examples/serialization_demo.cpp` for complete usage examples.

---

**Note**: This serialization system is designed as a framework-level universal tool and does not contain specific business logic. Users can freely define any data structures and serialization methods.