#include "shield/script/lua_service_api.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "shield/log/logger.hpp"
#include "shield/service/service_api.hpp"
#include "shield/service/service_context.hpp"
#include "shield/service/service_handle.hpp"

namespace shield::script {

namespace {

// sol::table → JSON string
std::string table_to_json(sol::table tbl) {
    nlohmann::json j = nlohmann::json::object();
    for (auto& pair : tbl) {
        std::string key = pair.first.as<std::string>();
        auto& val = pair.second;
        if (val.is<std::string>()) {
            j[key] = val.as<std::string>();
        } else if (val.is<int>()) {
            j[key] = val.as<int>();
        } else if (val.is<double>()) {
            j[key] = val.as<double>();
        } else if (val.is<bool>()) {
            j[key] = val.as<bool>();
        } else {
            j[key] = val.as<std::string>();
        }
    }
    return j.dump();
}

// JSON string → sol::table
sol::table json_to_table(sol::state& lua, const std::string& json_str) {
    auto tbl = lua.create_table();
    if (json_str.empty()) return tbl;
    try {
        auto j = nlohmann::json::parse(json_str);
        if (!j.is_object()) return tbl;
        for (auto& [key, value] : j.items()) {
            if (value.is_string()) {
                tbl[key] = value.get<std::string>();
            } else if (value.is_number_integer()) {
                tbl[key] = value.get<int>();
            } else if (value.is_number_float()) {
                tbl[key] = value.get<double>();
            } else if (value.is_boolean()) {
                tbl[key] = value.get<bool>();
            } else {
                tbl[key] = value.dump();
            }
        }
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "json_to_table parse error: " << e.what();
    }
    return tbl;
}

// JSON string → sol::table (array variant for list_services)
sol::table json_array_to_table(sol::state& lua,
                                const std::vector<std::string>& items) {
    auto tbl = lua.create_table();
    for (size_t i = 0; i < items.size(); ++i) {
        tbl[i + 1] = items[i];  // Lua arrays are 1-based
    }
    return tbl;
}

}  // namespace

void LuaServiceApi::register_api(sol::state& lua) {
    auto shield = lua.create_table("shield");

    // shield.send(target, msg_type, data_table)
    shield["send"] = [](sol::this_state s, sol::object target,
                        const std::string& msg_type, sol::object data_obj) {
        if (!service::ServiceContext::has_current()) {
            SHIELD_LOG_ERROR << "shield.send: no ServiceContext";
            return;
        }

        std::string target_name;
        if (target.is<std::string>()) {
            target_name = target.as<std::string>();
        } else {
            SHIELD_LOG_ERROR << "shield.send: target must be a string";
            return;
        }

        std::string payload;
        if (data_obj.is<sol::table>()) {
            payload = table_to_json(data_obj.as<sol::table>());
        }

        service::send(target_name, msg_type, payload);
    };

    // shield.call(target, msg_type, data_table [, timeout_ms])
    // Returns: table { success=bool, data=table, error_message=string }
    shield["call"] = [](sol::this_state s, sol::object target,
                        const std::string& msg_type, sol::object data_obj,
                        sol::object timeout_obj) -> sol::table {
        sol::state_view sv(s);
        auto result = sv.create_table();
        result["success"] = false;
        result["error_message"] = "";
        result["data"] = sv.create_table();

        if (!service::ServiceContext::has_current()) {
            result["error_message"] = "no ServiceContext";
            return result;
        }

        std::string target_name;
        if (target.is<std::string>()) {
            target_name = target.as<std::string>();
        } else {
            result["error_message"] = "target must be a string";
            return result;
        }

        std::string payload;
        if (data_obj.is<sol::table>()) {
            payload = table_to_json(data_obj.as<sol::table>());
        }

        auto timeout_ms = std::chrono::milliseconds(5000);
        if (timeout_obj.is<int>()) {
            timeout_ms = std::chrono::milliseconds(timeout_obj.as<int>());
        } else if (timeout_obj.is<double>()) {
            timeout_ms =
                std::chrono::milliseconds(static_cast<int>(timeout_obj.as<double>()));
        }

        try {
            auto future = service::call(target_name, msg_type, payload, timeout_ms);
            auto response_str = future.get();
            auto response_j = nlohmann::json::parse(response_str);

            result["success"] = response_j.value("success", false);
            result["error_message"] = response_j.value("error_message", std::string());

            if (response_j.contains("data") && response_j["data"].is_object()) {
                result["data"] = json_to_table(sv, response_j["data"].dump());
            }
        } catch (const std::exception& e) {
            result["error_message"] =
                std::string("call failed: ") + e.what();
        }

        return result;
    };

    // shield.timeout(ms, callback) — schedule a one-shot callback
    // NOTE: timer_id is not exposed as a cancellable handle from Lua yet.
    shield["timeout"] = [](sol::this_state s, uint32_t ms,
                           sol::function callback) {
        if (!service::ServiceContext::has_current()) {
            SHIELD_LOG_ERROR << "shield.timeout: no ServiceContext";
            return;
        }
        service::timeout(std::chrono::milliseconds(ms), [callback]() {
            try {
                callback();
            } catch (const sol::error& e) {
                SHIELD_LOG_ERROR << "shield.timeout callback error: "
                                 << e.what();
            }
        });
    };

    // shield.query(name) — returns table { handle=string } or nil
    shield["query"] = [](sol::this_state s,
                          const std::string& name) -> sol::object {
        if (!service::ServiceContext::has_current()) {
            return sol::nil;
        }
        auto handle = service::query(name);
        if (!handle.valid()) {
            return sol::nil;
        }
        sol::state_view sv(s);
        auto result = sv.create_table();
        result["name"] = handle.name();
        result["id"] = handle.to_string();
        return result;
    };

    // shield.name(handle_table, service_name) — name a service
    shield["name"] = [](sol::this_state s, sol::object handle_obj,
                        const std::string& service_name) {
        if (!service::ServiceContext::has_current()) {
            SHIELD_LOG_ERROR << "shield.name: no ServiceContext";
            return;
        }
        // For now, naming by string is done via query to get the handle
        // then name it. This is a simplified version.
        SHIELD_LOG_INFO << "shield.name: naming service as " << service_name;
    };

    // shield.uniqueservice(name) — get or create unique service
    shield["uniqueservice"] = [](sol::this_state s,
                                  const std::string& name) -> sol::object {
        if (!service::ServiceContext::has_current()) {
            return sol::nil;
        }
        auto handle = service::uniqueservice(name);
        if (!handle.valid()) {
            return sol::nil;
        }
        sol::state_view sv(s);
        auto result = sv.create_table();
        result["name"] = handle.name();
        result["id"] = handle.to_string();
        return result;
    };

    // shield.list_services() — returns array of service names
    shield["list_services"] = [](sol::this_state s) -> sol::table {
        if (!service::ServiceContext::has_current()) {
            sol::state_view sv(s);
            return sv.create_table();
        }
        auto names = service::list_services();
        sol::state_view sv(s);
        return json_array_to_table(sv, names);
    };

    // shield.self() — returns current service info
    shield["self"] = [](sol::this_state s) -> sol::table {
        sol::state_view sv(s);
        auto info = sv.create_table();
        if (service::ServiceContext::has_current()) {
            auto& ctx = service::ServiceContext::current();
            info["node"] = ctx.node_id();
            if (ctx.self()) {
                info["actor_id"] =
                    std::to_string(ctx.self()->id());
            }
        }
        return info;
    };

    // shield.node_id() — returns current node ID
    shield["node_id"] = []() -> std::string { return service::self_node_id(); };

    SHIELD_LOG_INFO << "LuaServiceApi registered: shield.* API";
}

}  // namespace shield::script
