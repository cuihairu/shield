#include "shield/actor/lua_actor.hpp"

#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>

#include "shield/caf_type_ids.hpp"

namespace shield::actor {

LuaActor::LuaActor(caf::actor_config& cfg, script::LuaVMPool& lua_vm_pool,
                   DistributedActorSystem& actor_system,
                   const std::string& script_path, const std::string& actor_id)
    : event_based_actor(cfg),
      m_lua_vm_pool(lua_vm_pool),
      m_actor_system(actor_system),
      script_path_(script_path),
      actor_id_(
          actor_id.empty()
              ? "lua_actor_" +
                    std::to_string(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count())
              : actor_id),
      script_loaded_(false) {
    // Acquire Lua VM from pool
    m_lua_vm_handle = m_lua_vm_pool.acquire_vm();
    if (!m_lua_vm_handle) {
        SHIELD_LOG_ERROR << "Failed to acquire Lua VM for LuaActor: "
                         << actor_id_;
        throw std::runtime_error("Failed to acquire Lua VM");
    }

    // Setup Lua environment
    setup_lua_environment();
    register_cpp_functions();

    SHIELD_LOG_INFO << "LuaActor created with ID: " << actor_id_
                    << ", script: " << script_path_;
}

caf::behavior LuaActor::make_behavior() {
    // Load the Lua script
    if (!load_script()) {
        SHIELD_LOG_ERROR << "Failed to load script for LuaActor: "
                         << script_path_;
        return {[this](const std::string& msg_type,
                       const std::string& data_json) -> std::string {
            return R"({"success": false, "error_message": "Script not loaded"})";
        }};
    }

    return {[this](const std::string& msg_type,
                   const std::string& data_json) -> std::string {
        return handle_lua_message_json(msg_type, data_json);
    }};
}

bool LuaActor::load_script() {
    if (!std::filesystem::exists(script_path_)) {
        SHIELD_LOG_ERROR << "Script file does not exist: " << script_path_;
        return false;
    }

    try {
        bool success = m_lua_vm_handle->load_script(script_path_);
        if (success) {
            script_loaded_ = true;
            SHIELD_LOG_INFO << "Successfully loaded Lua script: "
                            << script_path_;

            // Call initialization function if it exists
            auto init_result = m_lua_vm_handle->call_function<void>("on_init");
            if (!init_result) {
                SHIELD_LOG_INFO
                    << "No on_init function found in script (this is optional)";
            }
        }
        return success;
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Exception loading script " << script_path_ << ": "
                         << e.what();
        return false;
    }
}

void LuaActor::setup_lua_environment() {
    // Set global variables accessible to Lua scripts
    m_lua_vm_handle->set_global("actor_id", actor_id_);
    m_lua_vm_handle->set_global("script_path", script_path_);

    // Create a message table type for Lua
    m_lua_vm_handle->execute_string(R"(
        -- Helper function to create message objects
        function create_message(msg_type, data, sender)
            return {
                type = msg_type or "",
                data = data or {},
                sender_id = sender or ""
            }
        end
        
        -- Helper function to create response objects  
        function create_response(success, data, error_msg)
            return {
                success = success ~= false,  -- default to true
                data = data or {},
                error_message = error_msg or ""
            }
        end
        
        -- Default message handler (can be overridden)
        function on_message(msg)
            log_info("Received message: " .. msg.type)
            return create_response(true, {reply = "message received"})
        end
    )");
}

void LuaActor::register_cpp_functions() {
    // Register logging functions
    m_lua_vm_handle->register_function(
        "log_info", [this](const std::string& msg) { lua_log_info(msg); });

    m_lua_vm_handle->register_function(
        "log_error", [this](const std::string& msg) { lua_log_error(msg); });

    // Register message sending function
    m_lua_vm_handle->register_function(
        "send_message",
        [this](const std::string& target, const std::string& msg_type,
               const std::unordered_map<std::string, std::string>& data) {
            lua_send_message(target, msg_type, data);
        });

    // Register utility functions
    m_lua_vm_handle->register_function("get_current_time", []() -> int64_t {
        auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch())
            .count();
    });

    m_lua_vm_handle->register_function(
        "get_actor_id", [this]() -> std::string { return actor_id_; });
}

LuaResponse LuaActor::handle_lua_message(const LuaMessage& msg) {
    if (!script_loaded_) {
        return LuaResponse(false, {}, "Script not loaded");
    }

    try {
        // Create a sol::table from LuaMessage data
        sol::table lua_msg_table = m_lua_vm_handle->lua().create_table();
        lua_msg_table["type"] = msg.type;
        lua_msg_table["sender_id"] = msg.sender_id;
        sol::table lua_data_table = m_lua_vm_handle->lua().create_table();
        for (const auto& [key, value] : msg.data) {
            lua_data_table[key] = value;
        }
        lua_msg_table["data"] = lua_data_table;

        // Call Lua message handler with the table as argument
        sol::protected_function on_message_func =
            m_lua_vm_handle->lua()["on_message"];
        if (!on_message_func.valid()) {
            SHIELD_LOG_ERROR
                << "Lua script does not have an 'on_message' function.";
            return LuaResponse(
                false, {}, "Lua script does not have 'on_message' function");
        }

        sol::protected_function_result result = on_message_func(lua_msg_table);

        if (result.valid()) {
            sol::table response_table = result;
            sol::object success_obj = response_table["success"];
            bool success = success_obj.valid() ? success_obj.as<bool>() : true;
            sol::object error_message_obj = response_table["error_message"];
            std::string error_message =
                error_message_obj.valid() ? error_message_obj.as<std::string>()
                                          : "";
            std::unordered_map<std::string, std::string> response_data;
            sol::object data_table_obj = response_table["data"];
            sol::table data_table = data_table_obj.valid()
                                        ? data_table_obj.as<sol::table>()
                                        : m_lua_vm_handle->lua().create_table();
            for (auto pair : data_table) {
                response_data[pair.first.as<std::string>()] =
                    pair.second.as<std::string>();
            }
            return LuaResponse(success, response_data, error_message);
        } else {
            sol::error err = result;
            SHIELD_LOG_ERROR << "Error calling Lua on_message function: "
                             << err.what();
            return LuaResponse(false, {},
                               std::string("Lua error: ") + err.what());
        }

    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Exception in handle_lua_message: " << e.what();
        return LuaResponse(false, {},
                           std::string("C++ exception: ") + e.what());
    }
}

std::string LuaActor::handle_lua_message_json(const std::string& msg_type,
                                              const std::string& data_json) {
    if (!script_loaded_) {
        return R"({"success": false, "error_message": "Script not loaded"})";
    }

    try {
        // Parse JSON data
        nlohmann::json data_obj;
        if (!data_json.empty()) {
            data_obj = nlohmann::json::parse(data_json);
        }

        // Create Lua message table
        sol::table lua_msg_table = m_lua_vm_handle->lua().create_table();
        lua_msg_table["type"] = msg_type;
        lua_msg_table["sender_id"] = "gateway";  // From gateway

        sol::table lua_data_table = m_lua_vm_handle->lua().create_table();
        for (auto& [key, value] : data_obj.items()) {
            if (value.is_string()) {
                lua_data_table[key] = value.get<std::string>();
            } else {
                lua_data_table[key] = value.dump();
            }
        }
        lua_msg_table["data"] = lua_data_table;

        // Call Lua message handler
        sol::protected_function on_message_func =
            m_lua_vm_handle->lua()["on_message"];
        if (!on_message_func.valid()) {
            return R"({"success": false, "error_message": "Lua script does not have 'on_message' function"})";
        }

        sol::protected_function_result result = on_message_func(lua_msg_table);
        if (result.valid()) {
            sol::table response_table = result;

            // Convert response to JSON
            nlohmann::json response_json;
            response_json["success"] = response_table["success"].get_or(true);
            response_json["error_message"] =
                response_table["error_message"].get_or(std::string());

            nlohmann::json data_response;
            sol::object data_obj = response_table["data"];
            if (data_obj.valid() && data_obj.is<sol::table>()) {
                sol::table data_table = data_obj.as<sol::table>();
                for (auto pair : data_table) {
                    std::string key = pair.first.as<std::string>();
                    if (pair.second.is<std::string>()) {
                        data_response[key] = pair.second.as<std::string>();
                    } else {
                        data_response[key] = "non-string-value";
                    }
                }
            }
            response_json["data"] = data_response;

            return response_json.dump();
        } else {
            sol::error err = result;
            nlohmann::json error_response;
            error_response["success"] = false;
            error_response["error_message"] =
                std::string("Lua error: ") + err.what();
            error_response["data"] = nlohmann::json::object();
            return error_response.dump();
        }

    } catch (const std::exception& e) {
        nlohmann::json error_response;
        error_response["success"] = false;
        error_response["error_message"] =
            std::string("C++ exception: ") + e.what();
        error_response["data"] = nlohmann::json::object();
        return error_response.dump();
    }
}

LuaResponse LuaActor::process_message(const LuaMessage& msg) {
    return handle_lua_message(msg);
}

void LuaActor::lua_log_info(const std::string& message) {
    SHIELD_LOG_INFO << "[" << actor_id_ << "] " << message;
}

void LuaActor::lua_log_error(const std::string& message) {
    SHIELD_LOG_ERROR << "[" << actor_id_ << "] " << message;
}

void LuaActor::lua_send_message(
    const std::string& target_actor, const std::string& msg_type,
    const std::unordered_map<std::string, std::string>& data) {
    // For now, just log the message sending attempt
    // In a full implementation, this would send to the distributed actor system
    SHIELD_LOG_INFO << "Lua actor " << actor_id_
                    << " attempting to send message '" << msg_type
                    << "' to actor: " << target_actor;

    // Convert data to JSON for logging
    nlohmann::json data_json(data);
    SHIELD_LOG_DEBUG << "Message data: " << data_json.dump();
}

caf::actor create_lua_actor(caf::actor_system& system,
                            script::LuaVMPool& lua_vm_pool,
                            DistributedActorSystem& actor_system,
                            const std::string& script_path,
                            const std::string& actor_id) {
    SHIELD_LOG_INFO << "Spawning LuaActor with script: " << script_path;
    return system.spawn<LuaActor>(lua_vm_pool, actor_system, script_path,
                                  actor_id);
}

}  // namespace shield::actor