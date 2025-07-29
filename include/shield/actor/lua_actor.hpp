#pragma once

#include "shield/actor/distributed_actor_system.hpp"
#include "shield/script/lua_vm_pool.hpp"
#include "shield/core/logger.hpp"
#include "caf/event_based_actor.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <unordered_map>

namespace shield::actor {

// Message types for LuaActor
struct LuaMessage {
    std::string type;
    std::unordered_map<std::string, std::string> data;
    std::string sender_id;
    
    LuaMessage() = default;
    LuaMessage(const std::string& msg_type, 
               const std::unordered_map<std::string, std::string>& msg_data = {},
               const std::string& sender = "")
        : type(msg_type), data(msg_data), sender_id(sender) {}
    
    // CAF serialization support
    template <class Inspector>
    friend bool inspect(Inspector& f, LuaMessage& x) {
        return f.object(x).fields(f.field("type", x.type),
                                  f.field("data", x.data),
                                  f.field("sender_id", x.sender_id));
    }
};

// Response from Lua script processing
struct LuaResponse {
    bool success = true;
    std::unordered_map<std::string, std::string> data;
    std::string error_message;
    
    LuaResponse() = default;
    LuaResponse(bool success_flag, 
                const std::unordered_map<std::string, std::string>& response_data = {},
                const std::string& error = "")
        : success(success_flag), data(response_data), error_message(error) {}
    
    // CAF serialization support
    template <class Inspector>
    friend bool inspect(Inspector& f, LuaResponse& x) {
        return f.object(x).fields(f.field("success", x.success),
                                  f.field("data", x.data),
                                  f.field("error_message", x.error_message));
    }
    
    // JSON serialization support (nlohmann::json ADL)
    friend void to_json(nlohmann::json& j, const LuaResponse& r) {
        j = nlohmann::json{
            {"success", r.success},
            {"data", r.data},
            {"error_message", r.error_message}
        };
    }
    
    friend void from_json(const nlohmann::json& j, LuaResponse& r) {
        j.at("success").get_to(r.success);
        j.at("data").get_to(r.data);
        j.at("error_message").get_to(r.error_message);
    }
};

// Base class for Lua-powered actors
class LuaActor : public caf::event_based_actor {
public:
    LuaActor(caf::actor_config& cfg, 
             script::LuaVMPool& lua_vm_pool, 
             DistributedActorSystem& actor_system,
             const std::string& script_path,
             const std::string& actor_id = "");
    virtual ~LuaActor() = default;

    // Actor initialization
    caf::behavior make_behavior() override;

protected:
    // Core functionality
    virtual bool load_script();
    virtual LuaResponse process_message(const LuaMessage& msg);
    
    // Lua integration helpers
    void register_cpp_functions();
    void setup_lua_environment();
    
    // Message handling
    virtual LuaResponse handle_lua_message(const LuaMessage& msg);
    virtual std::string handle_lua_message_json(const std::string& msg_type, const std::string& data_json);
    
    // Utility functions exposed to Lua
    void lua_log_info(const std::string& message);
    void lua_log_error(const std::string& message);
    void lua_send_message(const std::string& target_actor, const std::string& msg_type, 
                         const std::unordered_map<std::string, std::string>& data);
    
    // Actor state
    script::VMHandle m_lua_vm_handle;
    DistributedActorSystem& m_actor_system;
    script::LuaVMPool& m_lua_vm_pool;
    std::string script_path_;
    std::string actor_id_;
    bool script_loaded_;
};

// Factory function for creating LuaActor instances
caf::actor create_lua_actor(caf::actor_system& system, 
                           script::LuaVMPool& lua_vm_pool,
                           DistributedActorSystem& actor_system,
                           const std::string& script_path,
                           const std::string& actor_id = "");

} // namespace shield::actor