// [SHIELD_TRANSPORT] Internal cross-TU declaration for the protocol split.
//
// Not a public contract: the public pipeline API stays in
// shield/transport/protocol.hpp. parse_action is the one file-local helper
// shared across the split (route catalog + JSON pipeline builder), so it is
// defined flat in protocol_route.cpp and declared here at shield::transport
// scope (external linkage; no anonymous namespace in a header).
#pragma once

#include <optional>
#include <string_view>

#include "shield/transport/protocol.hpp"

namespace shield::transport {

// Parse a route action name ("decode_local" / "forward" / "forward_raw" /
// "drop"). Defined in protocol_route.cpp.
std::optional<RouteAction> parse_action(std::string_view value);

}  // namespace shield::transport
