#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "shield/serialization/universal_serialization_system.hpp"

// Example data structure
struct Player {
    uint64_t id;
    std::string name;
    int level;
    double experience;
    std::vector<std::string> items;

    // JSON serialization support (nlohmann::json ADL)
    friend void to_json(nlohmann::json& j, const Player& p) {
        j = nlohmann::json{{"id", p.id},
                           {"name", p.name},
                           {"level", p.level},
                           {"experience", p.experience},
                           {"items", p.items}};
    }

    friend void from_json(const nlohmann::json& j, Player& p) {
        j.at("id").get_to(p.id);
        j.at("name").get_to(p.name);
        j.at("level").get_to(p.level);
        j.at("experience").get_to(p.experience);
        j.at("items").get_to(p.items);
    }
};

// Demonstrate simple type MessagePack support
MSGPACK_DEFINE_MAP(Player, id, name, level, experience, items);

int main() {
    using namespace shield::serialization;

    std::cout << "=== Universal Serialization System Demo ===" << std::endl;

    try {
        // 1. Initialize serialization system
        SerializationConfig config;
        config.enable_json = true;
        config.enable_protobuf = true;
        config.enable_messagepack = true;
        config.default_format = SerializationFormat::JSON;
        config.enable_auto_format_detection = true;

        initialize_universal_serialization_system(config);

        // Show system information
        auto& system = UniversalSerializationSystem::instance();
        std::cout << system.get_system_info() << std::endl;

        // 2. Create test data
        Player player{.id = 12345,
                      .name = "TestPlayer",
                      .level = 42,
                      .experience = 12345.67,
                      .items = {"sword", "shield", "potion"}};

        std::cout << "\n=== Original Player Data ===" << std::endl;
        std::cout << "ID: " << player.id << std::endl;
        std::cout << "Name: " << player.name << std::endl;
        std::cout << "Level: " << player.level << std::endl;
        std::cout << "Experience: " << player.experience << std::endl;
        std::cout << "Items: ";
        for (const auto& item : player.items) {
            std::cout << item << " ";
        }
        std::cout << std::endl;

        // 3. JSON serialization test
        std::cout << "\n=== JSON Serialization Test ===" << std::endl;

        // Auto-detect format and serialize
        auto json_data = serialize_universal(player);
        std::cout << "Serialized JSON: " << json_data << std::endl;

        // Specify format serialization
        auto json_explicit = serialize_as<SerializationFormat::JSON>(player);
        std::cout << "Explicit JSON: " << json_explicit << std::endl;

        // Deserialize
        auto restored_player =
            deserialize_as<SerializationFormat::JSON, Player>(json_explicit);
        std::cout << "Restored player name: " << restored_player.name
                  << std::endl;

        // 4. MessagePack serialization test (if enabled)
#ifdef SHIELD_ENABLE_MESSAGEPACK
        std::cout << "\n=== MessagePack Serialization Test ===" << std::endl;

        try {
            auto msgpack_data =
                serialize_as<SerializationFormat::MESSAGEPACK>(player);
            std::cout << "MessagePack serialized, size: " << msgpack_data.size()
                      << " bytes" << std::endl;

            auto msgpack_restored =
                deserialize_as<SerializationFormat::MESSAGEPACK, Player>(
                    msgpack_data);
            std::cout << "MessagePack restored player name: "
                      << msgpack_restored.name << std::endl;
        } catch (const SerializationException& e) {
            std::cout << "MessagePack test failed: " << e.what() << std::endl;
        }
#else
        std::cout << "\nMessagePack support not enabled" << std::endl;
#endif

        // 5. Format auto-detection test
        std::cout << "\n=== Format Auto-Detection Test ===" << std::endl;
        auto recommended_format = system.get_recommended_format<Player>();

        const char* format_name = "Unknown";
        switch (recommended_format) {
            case SerializationFormat::JSON:
                format_name = "JSON";
                break;
            case SerializationFormat::PROTOBUF:
                format_name = "Protobuf";
                break;
            case SerializationFormat::MESSAGEPACK:
                format_name = "MessagePack";
                break;
            case SerializationFormat::SPROTO:
                format_name = "sproto";
                break;
            default:
                break;
        }

        std::cout << "Recommended format for Player: " << format_name
                  << std::endl;

        // 6. Convenience function test
        std::cout << "\n=== Convenience Functions Test ===" << std::endl;

        // JSON convenience functions
        auto json_str = to_json_string(player);
        std::cout << "JSON convenience: " << json_str.substr(0, 50) << "..."
                  << std::endl;

        auto convenience_player = from_json_string<Player>(json_str);
        std::cout << "Convenience restored: " << convenience_player.name
                  << std::endl;

        std::cout << "\n=== Demo completed successfully ===" << std::endl;

    } catch (const SerializationException& e) {
        std::cerr << "Serialization error: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}