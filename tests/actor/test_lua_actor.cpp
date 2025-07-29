// Test file for LuaActor
#include "shield/actor/lua_actor.hpp"
#include "shield/core/logger.hpp"
#include <iostream>
#include <cassert>
#include <filesystem>

// Mock CAF actor system for testing
struct MockActorConfig {
    // Empty for now - in real usage this would be caf::actor_config
};

class TestLuaActor : public shield::actor::LuaActor {
public:
    TestLuaActor(const std::string& script_path) 
        : LuaActor(reinterpret_cast<caf::actor_config&>(mock_config_), script_path) {}
    
    // Expose protected methods for testing
    using LuaActor::process_message;
    using LuaActor::load_script;
    
private:
    MockActorConfig mock_config_;
};

void test_lua_actor() {
    // Initialize logging
    shield::core::LogConfig log_config;
    shield::core::Logger::init(log_config);
    
    std::cout << "=== Testing LuaActor with Player Script ===" << std::endl;
    
    // Ensure script directory exists
    std::filesystem::create_directories("scripts");
    
    // Create test actor
    std::string script_path = "scripts/player_actor.lua";
    if (!std::filesystem::exists(script_path)) {
        std::cerr << "Script file not found: " << script_path << std::endl;
        std::cerr << "Please make sure player_actor.lua exists in scripts/ directory" << std::endl;
        return;
    }
    
    TestLuaActor actor(script_path);
    
    // Test 1: Script loading
    std::cout << "\nTest 1: Script loading" << std::endl;
    bool script_loaded = actor.load_script();
    assert(script_loaded && "Script loading failed");
    std::cout << "✅ Script loading: PASSED" << std::endl;
    
    // Test 2: Player login
    std::cout << "\nTest 2: Player login" << std::endl;
    shield::actor::LuaMessage login_msg("login", {
        {"player_name", "TestPlayer"},
        {"level", "5"},
        {"health", "80"}
    });
    
    auto login_response = actor.process_message(login_msg);
    assert(login_response.success && "Login failed");
    assert(login_response.data.at("player_name") == "TestPlayer");
    assert(login_response.data.at("level") == "5");
    std::cout << "✅ Player login: PASSED" << std::endl;
    
    // Test 3: Player movement
    std::cout << "\nTest 3: Player movement" << std::endl;
    shield::actor::LuaMessage move_msg("move", {
        {"x", "5"},
        {"y", "3"}
    });
    
    auto move_response = actor.process_message(move_msg);
    assert(move_response.success && "Movement failed");
    assert(move_response.data.at("x") == "5");
    assert(move_response.data.at("y") == "3");
    std::cout << "✅ Player movement: PASSED" << std::endl;
    
    // Test 4: Invalid movement (too far)
    std::cout << "\nTest 4: Invalid movement" << std::endl;
    shield::actor::LuaMessage invalid_move_msg("move", {
        {"x", "100"},  // Too far from current position
        {"y", "100"}
    });
    
    auto invalid_move_response = actor.process_message(invalid_move_msg);
    assert(!invalid_move_response.success && "Invalid movement should fail");
    std::cout << "✅ Invalid movement handling: PASSED" << std::endl;
    
    // Test 5: Attack action
    std::cout << "\nTest 5: Attack action" << std::endl;
    shield::actor::LuaMessage attack_msg("attack", {
        {"target", "monster_123"},
        {"damage", "25"}
    });
    
    auto attack_response = actor.process_message(attack_msg);
    assert(attack_response.success && "Attack failed");
    assert(attack_response.data.at("target") == "monster_123");
    assert(attack_response.data.at("damage") == "25");
    std::cout << "✅ Attack action: PASSED" << std::endl;
    
    // Test 6: Get player status
    std::cout << "\nTest 6: Get player status" << std::endl;
    shield::actor::LuaMessage status_msg("get_status");
    
    auto status_response = actor.process_message(status_msg);
    assert(status_response.success && "Get status failed");
    assert(status_response.data.at("player_name") == "TestPlayer");
    assert(status_response.data.at("level") == "5");
    assert(status_response.data.at("x") == "5");  // From previous move
    assert(status_response.data.at("y") == "3");
    std::cout << "✅ Get player status: PASSED" << std::endl;
    
    // Test 7: Unknown message type
    std::cout << "\nTest 7: Unknown message type" << std::endl;
    shield::actor::LuaMessage unknown_msg("unknown_action");
    
    auto unknown_response = actor.process_message(unknown_msg);
    assert(!unknown_response.success && "Unknown message should fail");
    std::cout << "✅ Unknown message handling: PASSED" << std::endl;
    
    std::cout << "\n🎉 All LuaActor tests passed! Game logic in Lua is working!" << std::endl;
}

int main() {
    try {
        test_lua_actor();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}