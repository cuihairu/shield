// [SHIELD_TRANSPORT] RPC descriptor table and JSON parsing
#include "shield/transport/rpc_descriptor.hpp"

#include <nlohmann/json.hpp>

namespace shield::transport {

bool RpcDescriptorTable::add(const RpcDescriptor& descriptor) {
    if (!names_.empty() || !descriptors_.empty()) {
        if (descriptors_.contains(descriptor.route_id)) {
            return false;
        }
        if (!descriptor.name.empty() && names_.contains(descriptor.name)) {
            return false;
        }
    }
    descriptors_.emplace(descriptor.route_id, descriptor);
    if (!descriptor.name.empty()) {
        names_.emplace(descriptor.name, descriptor.route_id);
    }
    return true;
}

const RpcDescriptor* RpcDescriptorTable::find(std::uint32_t route_id) const {
    const auto it = descriptors_.find(route_id);
    return it == descriptors_.end() ? nullptr : &it->second;
}

const RpcDescriptor* RpcDescriptorTable::find_by_name(
    std::string_view name) const {
    const auto it = names_.find(std::string(name));
    return it == names_.end() ? nullptr
                              : &descriptors_.find(it->second)->second;
}

bool RpcDescriptorTable::merge(const RpcDescriptorTable& other,
                               std::string* error) {
    for (const auto& [id, descriptor] : other.descriptors_) {
        if (!add(descriptor)) {
            if (error) {
                *error = "rpc route conflict: id " + std::to_string(id) +
                         (descriptor.name.empty()
                              ? ""
                              : " (name '" + descriptor.name + "')");
            }
            return false;
        }
    }
    return true;
}

void RpcDescriptorTable::clear() {
    descriptors_.clear();
    names_.clear();
}

RouteEntry route_entry_from_descriptor(const RpcDescriptor& descriptor) {
    RouteEntry entry;
    entry.route_id = descriptor.route_id;
    entry.direction = descriptor.direction;
    entry.requires_auth = descriptor.requires_auth;
    entry.kind = descriptor.kind;
    entry.debug_name = descriptor.name;
    entry.policy = descriptor.policy;
    return entry;
}  // GCOVR_EXCL_LINE (gcov clone artifact)

namespace {

std::optional<RouteDirection> parse_direction(std::string_view value) {
    if (value == "c2s" || value == "client_to_server") {
        return RouteDirection::ClientToServer;
    }
    if (value == "s2c" || value == "server_to_client") {
        return RouteDirection::ServerToClient;
    }
    if (value == "bidi" || value == "bidirectional") {
        return RouteDirection::Bidirectional;
    }
    return std::nullopt;
}

std::optional<RouteAction> parse_action(std::string_view value) {
    if (value == "decode_local") {
        return RouteAction::DecodeLocal;
    }
    if (value == "forward_raw") {
        return RouteAction::ForwardRaw;
    }
    if (value == "drop") {
        return RouteAction::Drop;
    }
    return std::nullopt;
}

}  // namespace

bool parse_rpc_routes_json(std::string_view routes_json,
                           RpcDescriptorTable& table, std::string* error) {
    nlohmann::json parsed = nlohmann::json::parse(routes_json, nullptr, false);
    if (parsed.is_discarded()) {
        if (error) *error = "rpc.routes is not valid JSON";
        return false;
    }
    if (!parsed.is_array()) {
        if (error) *error = "rpc.routes must be a JSON array";
        return false;
    }

    for (const auto& route : parsed) {
        if (!route.is_object()) {
            if (error) *error = "rpc.routes[] entries must be objects";
            return false;
        }

        RpcDescriptor descriptor;
        descriptor.route_id = route.value("id", std::uint32_t{0});
        if (descriptor.route_id == 0) {
            if (error) *error = "rpc.routes[].id is required and must be >= 1";
            return false;
        }

        descriptor.name = route.value("name", std::string{});
        if (route.contains("direction")) {
            if (!route["direction"].is_string()) {
                if (error) *error = "rpc.routes[].direction must be a string";
                return false;
            }
            const auto direction =
                parse_direction(route["direction"].get<std::string>());
            if (!direction) {
                if (error) *error = "rpc.routes[].direction is invalid";
                return false;
            }
            descriptor.direction = *direction;
        }
        descriptor.requires_auth = route.value("requires_auth", true);
        descriptor.binding = route.value("binding", std::string{});
        if (descriptor.binding.empty()) {
            if (error) {
                *error = "rpc.routes[].binding is required (route id " +
                         std::to_string(descriptor.route_id) + ")";
            }
            return false;
        }
        descriptor.owner_service = route.value("owner_service", std::string{});
        descriptor.request_codec = route.value("request_codec", std::string{});
        descriptor.request_schema =
            route.value("request_schema", std::string{});
        descriptor.response_schema =
            route.value("response_schema", std::string{});
        descriptor.policy.lazy_decode = route.value("lazy_decode", true);
        if (route.contains("action")) {
            if (!route["action"].is_string()) {
                if (error) *error = "rpc.routes[].action must be a string";
                return false;
            }
            const auto action =
                parse_action(route["action"].get<std::string>());
            if (!action) {
                if (error) *error = "rpc.routes[].action is invalid";
                return false;
            }
            descriptor.policy.action = *action;
        }

        if (!table.add(descriptor)) {
            if (error) {
                *error = "rpc.routes contains duplicate id or name (route id " +
                         std::to_string(descriptor.route_id) + ")";
            }
            return false;
        }
    }
    return true;
}

}  // namespace shield::transport
