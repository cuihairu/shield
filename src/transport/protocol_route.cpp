// [SHIELD_TRANSPORT] Packet view, the route table and the XML route
// catalog (load_xmldef_routes_*).
// Split out of protocol.cpp.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "shield/plugin/protocol_codec.h"
#include "shield/transport/protocol.hpp"
#include "shield/transport/rpc_descriptor.hpp"

namespace shield::transport {

namespace {
std::string trim_copy(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(begin, end - begin + 1));
}

std::optional<std::uint32_t> parse_u32(std::string_view value) {
    try {
        std::size_t parsed = 0;
        const auto text = trim_copy(value);
        const auto number = std::stoull(text, &parsed, 0);
        if (parsed != text.size() ||
            number > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(number);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> parse_bool(std::string_view value) {
    const auto text = trim_copy(value);
    if (text == "true" || text == "1" || text == "yes") {
        return true;
    }
    if (text == "false" || text == "0" || text == "no") {
        return false;
    }
    return std::nullopt;
}
}  // namespace

std::optional<RouteAction> parse_action(std::string_view value) {
    const auto text = trim_copy(value);
    if (text == "decode" || text == "decode_local") {
        return RouteAction::DecodeLocal;
    }
    if (text == "forward" || text == "forward_raw") {
        return RouteAction::ForwardRaw;
    }
    if (text == "drop") {
        return RouteAction::Drop;
    }
    return std::nullopt;
}

namespace {
std::unordered_map<std::string, std::string> parse_xml_attributes(
    std::string_view tag) {
    std::unordered_map<std::string, std::string> attrs;
    std::size_t pos = 0;

    while (pos < tag.size()) {
        while (pos < tag.size() &&
               (tag[pos] == ' ' || tag[pos] == '\t' || tag[pos] == '\r' ||
                tag[pos] == '\n' || tag[pos] == '/')) {
            ++pos;
        }
        const auto key_begin = pos;
        while (pos < tag.size() &&
               (std::isalnum(static_cast<unsigned char>(tag[pos])) ||
                tag[pos] == '_' || tag[pos] == '-' || tag[pos] == '.')) {
            ++pos;
        }
        if (key_begin == pos) {
            ++pos;
            continue;
        }

        const auto key = std::string(tag.substr(key_begin, pos - key_begin));
        // GCOVR_EXCL_BR_START (defensive: the xmldef catalog reader never
        // produces a tag that ends right after a key name)
        while (pos < tag.size() && (tag[pos] == ' ' || tag[pos] == '\t' ||
                                    tag[pos] == '\r' || tag[pos] == '\n')) {
            ++pos;
        }
        // GCOVR_EXCL_BR_STOP
        if (pos >= tag.size() || tag[pos] != '=') {
            continue;
        }
        ++pos;
        while (pos < tag.size() && (tag[pos] == ' ' || tag[pos] == '\t' ||
                                    tag[pos] == '\r' || tag[pos] == '\n')) {
            ++pos;
        }
        if (pos >= tag.size() || (tag[pos] != '"' && tag[pos] != '\'')) {
            continue;
        }

        const auto quote = tag[pos++];
        const auto value_begin = pos;
        while (pos < tag.size() && tag[pos] != quote) {
            ++pos;
        }
        if (pos >= tag.size()) {
            break;
        }
        attrs.insert_or_assign(
            key, std::string(tag.substr(value_begin, pos - value_begin)));
        ++pos;
    }

    return attrs;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

const std::string* find_attr(
    const std::unordered_map<std::string, std::string>& attrs,
    std::string_view key) {
    const auto it = attrs.find(std::string(key));
    if (it == attrs.end()) {
        return nullptr;
    }
    return &it->second;
}
}  // namespace

PacketRef Packet::ref() const {
    return PacketRef{
        .route_id = route_id,
        .kind = kind,
        .flags = flags,
        .seq = seq,
        .body = body,
        .raw_frame = raw_frame,
    };
}

bool RouteTable::add(RouteEntry entry) {
    if (!entry.debug_name.empty() && route_names_.contains(entry.debug_name)) {
        return false;
    }

    const auto route_id = entry.route_id;
    const auto route_name = entry.debug_name;
    const auto [_, inserted] = routes_.emplace(route_id, std::move(entry));
    if (inserted && !route_name.empty()) {
        route_names_.emplace(route_name, route_id);
    }
    return inserted;
}

void RouteTable::upsert(RouteEntry entry) {
    const auto route_id = entry.route_id;
    const auto route_name = entry.debug_name;

    const auto existing = routes_.find(route_id);
    if (existing != routes_.end() && !existing->second.debug_name.empty()) {
        const auto name = existing->second.debug_name;
        const auto name_it = route_names_.find(name);
        if (name_it != route_names_.end() && name_it->second == route_id) {
            route_names_.erase(name_it);
        }
    }

    routes_.insert_or_assign(route_id, std::move(entry));
    if (!route_name.empty()) {
        route_names_.insert_or_assign(route_name, route_id);
    }
}

const RouteEntry* RouteTable::find(std::uint32_t route_id) const {
    const auto it = routes_.find(route_id);
    if (it == routes_.end()) {
        return nullptr;
    }
    return &it->second;
}

const RouteEntry* RouteTable::find_by_name(std::string_view route_name) const {
    const auto it = route_names_.find(std::string(route_name));
    if (it == route_names_.end()) {
        return nullptr;
    }
    return find(it->second);
}

const RouteEntry* RouteTable::only() const {
    if (routes_.size() != 1) {
        return nullptr;
    }
    return &routes_.begin()->second;
}

bool RouteTable::contains(std::uint32_t route_id) const {
    return routes_.contains(route_id);
}

void RouteTable::clear() {
    routes_.clear();
    route_names_.clear();
}

std::size_t RouteTable::size() const { return routes_.size(); }

bool load_xmldef_routes_from_string(std::string_view xml, RouteTable& routes,
                                    const XmldefCatalogOptions& options,
                                    std::string* error) {
    std::size_t pos = 0;

    while (true) {
        const auto open = xml.find('<', pos);
        if (open == std::string_view::npos) {
            break;
        }
        const auto close = xml.find('>', open + 1);
        if (close == std::string_view::npos) {
            if (error) *error = "xmldef contains an unterminated tag";
            return false;
        }

        const auto raw_tag = xml.substr(open + 1, close - open - 1);
        pos = close + 1;
        const auto tag_text = trim_copy(raw_tag);
        std::string_view tag(tag_text);
        if (tag.empty() || tag.front() == '/' || tag.front() == '?' ||
            tag.front() == '!') {
            continue;
        }

        const auto name_end = tag.find_first_of(" \t\r\n/");
        const auto tag_name = tag.substr(0, name_end);
        if (tag_name != "message" && tag_name != "route") {
            continue;
        }

        const auto attr_text = name_end == std::string_view::npos
                                   ? std::string_view{}
                                   : tag.substr(name_end + 1);
        const auto attrs = parse_xml_attributes(attr_text);
        const auto* id_attr = find_attr(attrs, "id");
        if (id_attr == nullptr) {
            if (error) *error = "xmldef message/route is missing id";
            return false;
        }

        const auto route_id = parse_u32(*id_attr);
        if (!route_id || *route_id == 0) {
            if (error) *error = "xmldef message/route id must be uint32";
            return false;
        }

        RouteEntry entry;
        entry.route_id = *route_id;
        entry.policy.action = options.default_action;
        entry.policy.lazy_decode = options.default_lazy_decode;

        if (const auto* value = find_attr(attrs, "name")) {
            entry.debug_name = *value;
            // Same-name convention, matching route_entry_from_descriptor:
            // the catalog route name is also the schema type name.
            entry.schema_name = *value;
        }
        if (const auto* value = find_attr(attrs, "direction")) {
            std::string dir_str(*value);
            if (dir_str == "c2s" || dir_str == "client_to_server") {
                entry.direction = RouteDirection::ClientToServer;
            } else if (dir_str == "s2c" || dir_str == "server_to_client") {
                entry.direction = RouteDirection::ServerToClient;
            } else if (dir_str == "bidi" || dir_str == "bidirectional") {
                entry.direction = RouteDirection::Bidirectional;
            } else {
                if (error) *error = "xmldef direction must be c2s/s2c/bidi";
                return false;
            }
        }
        if (const auto* value = find_attr(attrs, "requires_auth")) {
            std::string auth_str(*value);
            entry.requires_auth = (auth_str == "true" || auth_str == "1");
        }
        if (const auto* value = find_attr(attrs, "action")) {
            const auto parsed = parse_action(*value);
            if (!parsed) {
                if (error) *error = "xmldef action is invalid";
                return false;
            }
            entry.policy.action = *parsed;
        }
        if (const auto* value = find_attr(attrs, "lazy_decode")) {
            const auto parsed = parse_bool(*value);
            if (!parsed) {
                if (error) *error = "xmldef lazy_decode must be bool";
                return false;
            }
            entry.policy.lazy_decode = *parsed;
        }

        if (!routes.add(std::move(entry))) {
            if (error) *error = "xmldef contains duplicate route id or name";
            return false;
        }
    }

    return true;
}

bool load_xmldef_routes_from_file(std::string_view path, RouteTable& routes,
                                  const XmldefCatalogOptions& options,
                                  std::string* error) {
    std::ifstream file{std::filesystem::path(std::string(path))};
    if (!file.is_open()) {
        if (error) *error = "failed to open xmldef catalog";
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return load_xmldef_routes_from_string(buffer.str(), routes, options, error);
}

}  // namespace shield::transport
