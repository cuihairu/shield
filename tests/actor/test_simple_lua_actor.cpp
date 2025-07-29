// Simplified test for LuaActor without CAF integration
#include "shield/script/lua_engine.hpp"
#include "shield/core/logger.hpp"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <unordered_map>

// Simplified LuaActor for testing (without CAF dependency)
class SimpleLuaActor {
public:
    struct LuaMessage {
        std::string type;
        std::unordered_map<std::string, std::string> data;
        std::string sender_id;
        
        LuaMessage(const std::string& msg_type, 
                   const std::unordered_map<std::string, std::string>& msg_data = {},
                   const std::string& sender = "")
            : type(msg_type), data(msg_data), sender_id(sender) {}
    };

    struct LuaResponse {
        bool success = true;
        std::unordered_map<std::string, std::string> data;
        std::string error_message;
        
        LuaResponse(bool success_flag, 
                    const std::unordered_map<std::string, std::string>& response_data = {},
                    const std::string& error = "")
            : success(success_flag), data(response_data), error_message(error) {}
    };

    SimpleLuaActor(const std::string& script_path) 
        : script_path_(script_path), script_loaded_(false) {
        
        // Generate unique actor ID
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        actor_id_ = "lua_actor_" + std::to_string(timestamp);
        
        // Create and initialize Lua engine
        lua_engine_ = std::make_unique<shield::script::LuaEngine>("lua_actor_engine");
        lua_engine_->init();
        lua_engine_->start();
        
        // Setup Lua environment
        setup_lua_environment();
        register_cpp_functions();
        
        SHIELD_LOG_INFO << "SimpleLuaActor created with ID: " << actor_id_ << ", script: " << script_path_;
    }

    bool load_script() {
        if (!std::filesystem::exists(script_path_)) {
            SHIELD_LOG_ERROR << "Script file does not exist: " << script_path_;
            return false;
        }
        
        try {
            bool success = lua_engine_->load_script(script_path_);
            if (success) {
                script_loaded_ = true;
                SHIELD_LOG_INFO << "Successfully loaded Lua script: " << script_path_;
                
                // Call initialization function if it exists
                bool init_result = lua_engine_->call_function<void>("on_init");
                if (!init_result) {
                    SHIELD_LOG_INFO << "No on_init function found in script (this is optional)";
                }
            }
            return success;
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Exception loading script " << script_path_ << ": " << e.what();
            return false;
        }
    }

    LuaResponse process_message(const LuaMessage& msg) {
        if (!script_loaded_) {
            return LuaResponse(false, {}, "Script not loaded");
        }
        
        try {
            // Set message data in Lua global space
            std::string lua_code = "current_message = {" +
                std::string("type = \"") + msg.type + "\", " +
                std::string("sender_id = \"") + msg.sender_id + "\", " +
                std::string("data = {}}");
            
            lua_engine_->execute_string(lua_code);
            
            // Set message data fields
            for (const auto& [key, value] : msg.data) {
                lua_engine_->execute_string("current_message.data[\"" + key + "\"] = \"" + value + "\"");
            }
            
            // Call Lua message handler using execute_string
            bool call_success = lua_engine_->execute_string("response = on_message(current_message)");
            
            if (call_success) {
                // Extract response data using Lua code
                lua_engine_->execute_string(R"(
                    if response and response.success ~= nil then
                        response_success = response.success
                    else
                        response_success = false
                    end
                    
                    if response and response.error_message then
                        response_error = response.error_message  
                    else
                        response_error = ""
                    end
                )");
                
                // Get simple values
                auto success = lua_engine_->get_global<bool>("response_success");
                auto error_msg = lua_engine_->get_global<std::string>("response_error");
                
                // Extract data fields manually 
                std::unordered_map<std::string, std::string> response_data;
                if (success && *success) {
                    // For login response, extract known fields
                    if (msg.type == "login") {
                        auto player_id = lua_engine_->execute_string("return response.data.player_id") ? 
                                        lua_engine_->get_global<std::string>("player_id") : std::nullopt;
                        auto player_name = lua_engine_->execute_string("return response.data.player_name") ?
                                         lua_engine_->get_global<std::string>("player_name") : std::nullopt;
                        auto level = lua_engine_->execute_string("return response.data.level") ?
                                   lua_engine_->get_global<std::string>("level") : std::nullopt;
                        auto health = lua_engine_->execute_string("return response.data.health") ?
                                    lua_engine_->get_global<std::string>("health") : std::nullopt;
                        
                        // Extract data by executing Lua code to set globals
                        lua_engine_->execute_string(R"(
                            if response and response.data then
                                temp_player_id = response.data.player_id or ""
                                temp_player_name = response.data.player_name or ""
                                temp_level = response.data.level or ""
                                temp_health = response.data.health or ""
                            end
                        )");
                        
                        auto temp_player_id = lua_engine_->get_global<std::string>("temp_player_id");
                        auto temp_player_name = lua_engine_->get_global<std::string>("temp_player_name");
                        auto temp_level = lua_engine_->get_global<std::string>("temp_level");
                        auto temp_health = lua_engine_->get_global<std::string>("temp_health");
                        
                        if (temp_player_id) response_data["player_id"] = *temp_player_id;
                        if (temp_player_name) response_data["player_name"] = *temp_player_name;
                        if (temp_level) response_data["level"] = *temp_level;
                        if (temp_health) response_data["health"] = *temp_health;
                    }
                    // Handle other message types similarly
                    else if (msg.type == "move") {
                        lua_engine_->execute_string(R"(
                            if response and response.data then
                                temp_x = response.data.x or ""
                                temp_y = response.data.y or ""
                            end
                        )");
                        auto temp_x = lua_engine_->get_global<std::string>("temp_x");
                        auto temp_y = lua_engine_->get_global<std::string>("temp_y");
                        if (temp_x) response_data["x"] = *temp_x;
                        if (temp_y) response_data["y"] = *temp_y;
                    }
                    else if (msg.type == "get_status") {
                        lua_engine_->execute_string(R"(
                            if response and response.data then
                                temp_player_id = response.data.player_id or ""
                                temp_player_name = response.data.player_name or ""
                                temp_level = response.data.level or ""
                                temp_health = response.data.health or ""
                                temp_max_health = response.data.max_health or ""
                                temp_x = response.data.x or ""
                                temp_y = response.data.y or ""
                            end
                        )");
                        auto temp_player_id = lua_engine_->get_global<std::string>("temp_player_id");
                        auto temp_player_name = lua_engine_->get_global<std::string>("temp_player_name");
                        auto temp_level = lua_engine_->get_global<std::string>("temp_level");
                        auto temp_health = lua_engine_->get_global<std::string>("temp_health");
                        auto temp_max_health = lua_engine_->get_global<std::string>("temp_max_health");
                        auto temp_x = lua_engine_->get_global<std::string>("temp_x");
                        auto temp_y = lua_engine_->get_global<std::string>("temp_y");
                        
                        if (temp_player_id) response_data["player_id"] = *temp_player_id;
                        if (temp_player_name) response_data["player_name"] = *temp_player_name;
                        if (temp_level) response_data["level"] = *temp_level;
                        if (temp_health) response_data["health"] = *temp_health;
                        if (temp_max_health) response_data["max_health"] = *temp_max_health;
                        if (temp_x) response_data["x"] = *temp_x;
                        if (temp_y) response_data["y"] = *temp_y;
                    }
                }
                
                return LuaResponse(
                    success ? *success : false,
                    response_data,
                    error_msg ? *error_msg : ""
                );
            } else {
                return LuaResponse(false, {}, "Failed to call Lua on_message function");
            }
            
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Exception in process_message: " << e.what();
            return LuaResponse(false, {}, std::string("C++ exception: ") + e.what());
        }
    }

private:
    void setup_lua_environment() {
        // Set global variables accessible to Lua scripts
        lua_engine_->set_global("actor_id", actor_id_);
        lua_engine_->set_global("script_path", script_path_);
        
        // Create helper functions
        lua_engine_->execute_string(R"(
            function create_message(msg_type, data, sender)
                return {
                    type = msg_type or "",
                    data = data or {},
                    sender_id = sender or ""
                }
            end
            
            function create_response(success, data, error_msg)
                return {
                    success = success ~= false,
                    data = data or {},
                    error_message = error_msg or ""
                }
            end
            
            function on_message(msg)
                log_info("Received message: " .. msg.type)
                return create_response(true, {reply = "message received"})
            end
        )");
    }

    void register_cpp_functions() {
        // Register logging functions
        lua_engine_->register_function("log_info", [this](const std::string& msg) {
            SHIELD_LOG_INFO << "[" << actor_id_ << "] " << msg;
        });
        
        lua_engine_->register_function("log_error", [this](const std::string& msg) {
            SHIELD_LOG_ERROR << "[" << actor_id_ << "] " << msg;
        });
        
        // Register utility functions
        lua_engine_->register_function("get_current_time", []() -> int64_t {
            auto now = std::chrono::system_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        });
        
        lua_engine_->register_function("get_actor_id", [this]() -> std::string {
            return actor_id_;
        });
    }

    std::unique_ptr<shield::script::LuaEngine> lua_engine_;
    std::string script_path_;
    std::string actor_id_;
    bool script_loaded_;
};

void test_simple_lua_actor() {
    // Initialize logging
    shield::core::LogConfig log_config;
    shield::core::Logger::init(log_config);
    
    std::cout << "=== Testing SimpleLuaActor with Player Script ===" << std::endl;
    
    // Create test actor
    std::string script_path = "scripts/player_actor.lua";
    if (!std::filesystem::exists(script_path)) {
        std::cerr << "Script file not found: " << script_path << std::endl;
        return;
    }
    
    SimpleLuaActor actor(script_path);
    
    // Test 1: Script loading
    std::cout << "\nTest 1: Script loading" << std::endl;
    bool script_loaded = actor.load_script();
    assert(script_loaded && "Script loading failed");
    std::cout << "✅ Script loading: PASSED" << std::endl;
    
    // Test 2: Player login
    std::cout << "\nTest 2: Player login" << std::endl;
    SimpleLuaActor::LuaMessage login_msg("login", {
        {"player_name", "TestPlayer"},
        {"level", "5"},
        {"health", "80"}
    });
    
    auto login_response = actor.process_message(login_msg);
    std::cout << "Login response success: " << login_response.success << std::endl;
    std::cout << "Login response error: " << login_response.error_message << std::endl;
    std::cout << "Login response data size: " << login_response.data.size() << std::endl;
    for (const auto& [key, value] : login_response.data) {
        std::cout << "  " << key << " = " << value << std::endl;
    }
    assert(login_response.success && "Login failed");
    assert(login_response.data.at("player_name") == "TestPlayer");
    assert(login_response.data.at("level") == "5");
    std::cout << "✅ Player login: PASSED" << std::endl;
    
    // Test 3: Player movement
    std::cout << "\nTest 3: Player movement" << std::endl;
    SimpleLuaActor::LuaMessage move_msg("move", {
        {"x", "5"},
        {"y", "3"}
    });
    
    auto move_response = actor.process_message(move_msg);
    assert(move_response.success && "Movement failed");
    assert(move_response.data.at("x") == "5");
    assert(move_response.data.at("y") == "3");
    std::cout << "✅ Player movement: PASSED" << std::endl;
    
    // Test 4: Get player status
    std::cout << "\nTest 4: Get player status" << std::endl;
    SimpleLuaActor::LuaMessage status_msg("get_status");
    
    auto status_response = actor.process_message(status_msg);
    assert(status_response.success && "Get status failed");
    assert(status_response.data.at("player_name") == "TestPlayer");
    assert(status_response.data.at("level") == "5");
    assert(status_response.data.at("x") == "5");  // From previous move
    assert(status_response.data.at("y") == "3");
    std::cout << "✅ Get player status: PASSED" << std::endl;
    
    std::cout << "\n🎉 All SimpleLuaActor tests passed! Game logic in Lua is working!" << std::endl;
}

int main() {
    try {
        test_simple_lua_actor();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}