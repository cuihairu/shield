// [SHIELD_LUA] Lua service implementation
#include "shield/lua/lua_service.hpp"
#include "shield/lua/lua_runtime.hpp"

#include "shield/base/error.hpp"
#include "shield/base/result.hpp"
#include "shield/config/config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace shield::lua {

CallResult CallResult::ok(nlohmann::json values) {
    return {true, std::move(values), ""};
}

CallResult CallResult::error(std::string msg) {
    return {false, nlohmann::json::array(), std::move(msg)};
}

struct LuaServiceManager::Impl {
    LuaRuntime& runtime;
    std::unordered_map<std::string, std::shared_ptr<LuaVM>> services;
    std::unordered_map<std::string, std::string> published_names;
    std::unordered_map<std::string, std::unordered_set<std::string>> owned_names;
    std::unordered_map<std::string, std::string> module_scripts;
    std::vector<std::string> service_order;

    struct DispatchFrame {
        std::string service_id;
        std::string sender_id;
        bool in_exit = false;
        bool exit_requested = false;
        std::string exit_reason = "normal";
    };

    std::vector<DispatchFrame> dispatch_stack;

    Impl(LuaRuntime& rt) : runtime(rt) {
        for (const auto& actor : shield::config::runtime_actors()) {
            module_scripts.emplace(actor.name, resolve_script_path(actor));
        }
    }

    static std::string resolve_script_path(
        const shield::config::RuntimeActorConfig& actor) {
        std::filesystem::path script(actor.script);
        if (script.is_absolute() || std::filesystem::exists(script)) {
            return script.string();
        }

        if (!actor.source_dir.empty()) {
            auto from_config = std::filesystem::path(actor.source_dir) / script;
            if (std::filesystem::exists(from_config)) {
                return from_config.string();
            }
        }

        return script.string();
    }

    std::string resolve_module(std::string_view module) const {
        auto it = module_scripts.find(std::string(module));
        if (it != module_scripts.end()) {
            return it->second;
        }
        return std::string(module);
    }

    std::string current_service_id() const {
        if (dispatch_stack.empty()) {
            return "";
        }
        return dispatch_stack.back().service_id;
    }

    std::string current_sender_id() const {
        if (dispatch_stack.empty()) {
            return "";
        }
        return dispatch_stack.back().sender_id;
    }

    bool is_exit_requested(std::string* service_id, std::string* reason) const {
        if (dispatch_stack.empty() || !dispatch_stack.back().exit_requested) {
            return false;
        }
        if (service_id) {
            *service_id = dispatch_stack.back().service_id;
        }
        if (reason) {
            *reason = dispatch_stack.back().exit_reason;
        }
        return true;
    }

    static bool valid_name(std::string_view name) {
        if (name.empty() || name.size() > 64 || name.rfind("shield.", 0) == 0) {
            return false;
        }
        for (char ch : name) {
            const bool ok = (ch >= 'a' && ch <= 'z') ||
                            (ch >= 'A' && ch <= 'Z') ||
                            (ch >= '0' && ch <= '9') ||
                            ch == '_' || ch == '.' || ch == '-';
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    class DispatchScope {
    public:
        DispatchScope(Impl& impl,
                      std::string service_id,
                      std::string sender_id,
                      bool in_exit)
            : impl_(impl) {
            impl_.dispatch_stack.push_back({
                std::move(service_id),
                std::move(sender_id),
                in_exit,
                false,
                "normal",
            });
        }

        ~DispatchScope() {
            impl_.dispatch_stack.pop_back();
        }

        DispatchScope(const DispatchScope&) = delete;
        DispatchScope& operator=(const DispatchScope&) = delete;

    private:
        Impl& impl_;
    };
};

LuaServiceManager::LuaServiceManager(LuaRuntime& runtime)
    : impl_(std::make_unique<Impl>(runtime)) {
    runtime.set_service_manager(this);
}

LuaServiceManager::~LuaServiceManager() {
    impl_->runtime.set_service_manager(nullptr);
}

SpawnResult LuaServiceManager::spawn(std::string_view module,
                                     std::string_view opts_json) {
    try {
        // Parse options
        nlohmann::json opts = nlohmann::json::parse(opts_json);

        std::string service_name = opts.value("name", "");
        nlohmann::json args = opts.value("args", nlohmann::json::object());
        nlohmann::json config = opts.value("config", nlohmann::json::object());

        // Generate service ID if no name provided
        if (service_name.empty()) {
            service_name = module;
            service_name += ":";
            service_name += std::to_string(std::hash<std::string_view>{}(module));
        }
        if (impl_->services.contains(service_name)) {
            return SpawnResult::error("service already exists: " + service_name);
        }
        if (impl_->published_names.contains(service_name)) {
            return SpawnResult::error("service name already exists: " + service_name);
        }
        if (!Impl::valid_name(service_name)) {
            return SpawnResult::error("invalid service name: " + service_name);
        }

        const std::string script_path = impl_->resolve_module(module);

        // Create VM and load module
        auto vm = impl_->runtime.create_vm();
        impl_->runtime.register_api(vm);

        std::string error;
        if (!impl_->runtime.load_service_module(vm, script_path, &error)) {
            return SpawnResult::error("Failed to load module: " +
                                      script_path + ": " + error);
        }

        nlohmann::json init_args = {
            {"name", service_name},
            {"id", service_name},
            {"args", args},
            {"config", config},
        };
        bool exit_after_init = false;
        std::string exit_reason;
        {
            Impl::DispatchScope scope(*impl_, service_name, "", false);
            if (!impl_->runtime.call_service_function(vm, "on_init", init_args,
                                                      &error)) {
                return SpawnResult::error("on_init failed for " + service_name +
                                          ": " + error);
            }
            std::string exit_service_id;
            exit_after_init =
                impl_->is_exit_requested(&exit_service_id, &exit_reason) &&
                exit_service_id == service_name;
        }

        impl_->services[service_name] = std::move(vm);
        impl_->published_names[service_name] = service_name;
        impl_->owned_names[service_name].insert(service_name);
        impl_->service_order.push_back(service_name);
        if (exit_after_init) {
            exit(service_name, exit_reason);
        }
        return SpawnResult::ok(service_name);

    } catch (const std::exception& e) {
        return SpawnResult::error(std::string("Spawn failed: ") + e.what());
    }
}

bool LuaServiceManager::send(std::string_view target,
                             std::string_view method,
                             const nlohmann::json& args,
                             std::string* error) {
    CallResult result = call(target, method, args, 0);
    if (!result.success && error) {
        *error = result.error_message;
    }
    return result.success;
}

CallResult LuaServiceManager::call(std::string_view target,
                                   std::string_view method,
                                   const nlohmann::json& args,
                                   int32_t timeout_ms) {
    (void)timeout_ms;

    const std::string service_id = query_service(target);
    auto service_it = impl_->services.find(service_id);
    if (service_it == impl_->services.end()) {
        return CallResult::error("service not found: " + std::string(target));
    }

    const std::string sender = current_service_id();
    Impl::DispatchScope scope(*impl_, service_id, sender, false);

    nlohmann::json values = nlohmann::json::array();
    std::string error;
    if (!impl_->runtime.call_service_method(service_it->second, method, args,
                                            &values, &error)) {
        return CallResult::error(error);
    }

    std::string exit_service_id;
    std::string exit_reason;
    if (impl_->is_exit_requested(&exit_service_id, &exit_reason) &&
        exit_service_id == service_id) {
        exit(exit_service_id, exit_reason);
    }

    return CallResult::ok(std::move(values));
}

void LuaServiceManager::exit(std::string_view service_id,
                            std::string_view reason) {
    auto it = impl_->services.find(std::string(service_id));
    if (it == impl_->services.end()) {
        return;
    }

    std::string error;
    nlohmann::json args = std::string(reason);
    Impl::DispatchScope scope(*impl_, std::string(service_id), "", true);
    (void)impl_->runtime.call_service_function(it->second, "on_exit", args, &error);
    if (auto names_it = impl_->owned_names.find(std::string(service_id));
        names_it != impl_->owned_names.end()) {
        for (const auto& name : names_it->second) {
            impl_->published_names.erase(name);
        }
        impl_->owned_names.erase(names_it);
    }
    impl_->services.erase(it);
    impl_->service_order.erase(
        std::remove(impl_->service_order.begin(), impl_->service_order.end(),
                    std::string(service_id)),
        impl_->service_order.end());
}

void LuaServiceManager::shutdown_all(std::string_view reason) {
    std::unordered_set<std::string> seen;
    const auto order = impl_->service_order;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (seen.insert(*it).second && impl_->services.contains(*it)) {
            exit(*it, reason);
        }
    }
    impl_->service_order.clear();
}

std::string LuaServiceManager::current_service_id() const {
    return impl_->current_service_id();
}

std::string LuaServiceManager::current_sender_id() const {
    return impl_->current_sender_id();
}

void LuaServiceManager::request_current_exit(std::string_view reason) {
    if (impl_->dispatch_stack.empty()) {
        return;
    }
    auto& frame = impl_->dispatch_stack.back();
    if (frame.in_exit) {
        return;
    }
    frame.exit_requested = true;
    frame.exit_reason = reason.empty() ? "normal" : std::string(reason);
}

std::string LuaServiceManager::query_service(std::string_view name) const {
    auto it = impl_->published_names.find(std::string(name));
    if (it == impl_->published_names.end()) {
        return "";
    }
    return it->second;
}

bool LuaServiceManager::register_name(std::string_view name, std::string* error) {
    const std::string owner = current_service_id();
    if (owner.empty()) {
        if (error) {
            *error = "register requires current service context";
        }
        return false;
    }
    if (!impl_->services.contains(owner)) {
        if (error) {
            *error = "current service is not running: " + owner;
        }
        return false;
    }
    if (!Impl::valid_name(name)) {
        if (error) {
            *error = "invalid service name: " + std::string(name);
        }
        return false;
    }
    if (auto existing = impl_->published_names.find(std::string(name));
        existing != impl_->published_names.end() && existing->second != owner) {
        if (error) {
            *error = "service name already exists: " + std::string(name);
        }
        return false;
    }

    impl_->published_names[std::string(name)] = owner;
    impl_->owned_names[owner].insert(std::string(name));
    return true;
}

bool LuaServiceManager::unregister_name(std::string_view name, std::string* error) {
    const std::string owner = current_service_id();
    if (owner.empty()) {
        if (error) {
            *error = "unregister requires current service context";
        }
        return false;
    }

    auto existing = impl_->published_names.find(std::string(name));
    if (existing == impl_->published_names.end()) {
        if (error) {
            *error = "service name not found: " + std::string(name);
        }
        return false;
    }
    if (existing->second != owner) {
        if (error) {
            *error = "service name is owned by another service: " +
                     std::string(name);
        }
        return false;
    }

    impl_->published_names.erase(existing);
    if (auto names_it = impl_->owned_names.find(owner);
        names_it != impl_->owned_names.end()) {
        names_it->second.erase(std::string(name));
    }
    return true;
}

std::vector<std::string> LuaServiceManager::list_services() const {
    std::vector<std::string> services;
    services.reserve(impl_->published_names.size());
    for (const auto& [name, _] : impl_->published_names) {
        services.push_back(name);
    }
    std::sort(services.begin(), services.end());
    return services;
}

}  // namespace shield::lua
