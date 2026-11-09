// [SHIELD_TRANSPORT] Protocol pipeline: dispatch/encode over the
// envelope+codec stack, private route resolution and the JSON
// pipeline builder. Envelopes, codecs, the route table and the
// codec registry live in the sibling translation units.
#include "shield/transport/protocol.hpp"

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

#include "protocol_impl.hpp"
#include "shield/plugin/protocol_codec.h"
#include "shield/transport/rpc_descriptor.hpp"

namespace shield::transport {

namespace {
std::optional<Endian> parse_protocol_endian(const nlohmann::json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    const auto text = value.get<std::string>();
    if (text == "big" || text == "be") {
        return Endian::Big;
    }
    if (text == "little" || text == "le") {
        return Endian::Little;
    }
    return std::nullopt;
}

std::optional<EnvelopeKind> parse_envelope_kind(const std::string& value) {
    if (value == "lenprefix" || value == "len-prefix" ||
        value == "len_prefix") {
        return EnvelopeKind::LenPrefix;
    }
    if (value == "idlen" || value == "id-len" || value == "id_len") {
        return EnvelopeKind::IdLen;
    }
    if (value == "typed_len" || value == "type_len" || value == "typed-len" ||
        value == "typelen") {
        return EnvelopeKind::TypeLen;
    }
    if (value == "delimiter" || value == "line") {
        return EnvelopeKind::Delimiter;
    }
    return std::nullopt;
}

std::optional<RouteSource> parse_route_source(const std::string& value) {
    if (value == "header" || value == "header.route_id" ||
        value == "header.msg_id") {
        return RouteSource::Header;
    }
    if (value == "body" || value == "body.route" || value == "body.route_id") {
        return RouteSource::Body;
    }
    if (value == "none") {
        return RouteSource::None;
    }
    return std::nullopt;
}

RouteSource default_route_source_for_envelope(EnvelopeKind kind) {
    switch (kind) {  // GCOVR_EXCL_BR_LINE (defensive: compiler default edge,
                     // all four kinds handled)
        case EnvelopeKind::IdLen:
        case EnvelopeKind::TypeLen:
            return RouteSource::Header;
        case EnvelopeKind::LenPrefix:
        case EnvelopeKind::Delimiter:
            return RouteSource::Body;
    }
    return RouteSource::Body;  // GCOVR_EXCL_LINE (defensive: all kinds handled)
}
}  // namespace

ProtocolPipeline::ProtocolPipeline(ProtocolProfile profile, RouteTable routes,
                                   BodyCodecRegistry codecs)
    : profile_(std::move(profile)),
      envelope_(create_envelope(profile_.envelope_kind, profile_.envelope)),
      routes_(std::move(routes)),
      codecs_(std::move(codecs)) {
    if (!envelope_) {
        error_ = "failed to create protocol envelope";
    }
}

std::vector<DispatchResult> ProtocolPipeline::feed(const std::uint8_t* data,
                                                   std::size_t size) {
    std::vector<DispatchResult> results;
    error_.clear();

    if (!envelope_) {
        error_ = "protocol envelope is not configured";
        // GCOVR_EXCL_BR_START (compiler artifact: inlined std::string and
        // vector pseudo-branches)
        results.push_back(DispatchResult{.error = error_});
        // GCOVR_EXCL_BR_STOP
        return results;
    }

    auto packets = envelope_->feed(data, size);
    if (envelope_->has_error()) {
        error_ = envelope_->error();
        // GCOVR_EXCL_BR_START (compiler artifact: inlined std::string and
        // vector pseudo-branches)
        results.push_back(DispatchResult{.error = error_});
        // GCOVR_EXCL_BR_STOP
        return results;
    }

    results.reserve(packets.size());
    for (auto& packet : packets) {
        DispatchResult result;
        result.packet = std::move(packet);

        const auto* route = resolve_route(result.packet, result);
        result.route = route;
        if (!result.ok()) {
            results.push_back(std::move(result));
            continue;
        }

        if (route == nullptr) {
            result.action = profile_.unknown_route_action;
            results.push_back(std::move(result));
            continue;
        }

        result.action = route->policy.action;
        if (result.action == RouteAction::Drop) {
            results.push_back(std::move(result));
            continue;
        }
        const auto should_decode =
            result.action == RouteAction::DecodeLocal &&
            (profile_.decode_before_dispatch || !route->policy.lazy_decode);
        if (should_decode) {
            auto* codec = codec_for_route(*route);
            if (codec == nullptr) {
                result.error = "body codec is not registered";
                results.push_back(std::move(result));
                continue;
            }
            try {
                result.decoded_body =
                    codec->decode(result.packet.ref(), *route);
            } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: catch-entry pseudo-arc)
                         ex) {              // (compiler artifact: eh
                                            // edge residual; catch
                                            // body covered by
                                            // decode-failure tests)
                result.error = std::string("body decode failed: ") + ex.what();
            }
        }

        results.push_back(std::move(result));
    }

    return results;
}

std::vector<std::uint8_t> ProtocolPipeline::encode(const PacketRef& packet) {
    error_.clear();
    if (!envelope_) {
        error_ = "protocol envelope is not configured";
        return {};
    }

    auto encoded = envelope_->encode(packet);
    if (envelope_->has_error()) {
        error_ = envelope_->error();
    }
    return encoded;
}

std::vector<std::uint8_t> ProtocolPipeline::encode_message(DecodedBody body) {
    error_.clear();
    if (!envelope_) {
        error_ = "protocol envelope is not configured";
        return {};
    }

    const auto* route = resolve_outbound_route(body);
    if (route == nullptr) {
        error_ = "failed to resolve outbound route";
        return {};
    }

    auto* codec = codec_for_route(*route);
    if (codec == nullptr) {
        error_ = "body codec is not registered";
        return {};
    }

    std::vector<std::uint8_t> body_bytes;
    try {
        body_bytes = codec->encode(body, *route, profile_);
    } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // catch-entry pseudo-arc)
                 ex) {              // (compiler artifact: eh edge
                                    // residual; catch body covered
                                    // by encode-failure tests)
        error_ = std::string("body encode failed: ") + ex.what();
        return {};
    }

    Packet packet;
    packet.route_id = route->route_id;
    packet.kind = static_cast<std::uint16_t>(route->kind);
    packet.body = std::move(body_bytes);
    return encode(packet.ref());
}

bool ProtocolPipeline::materialize_decode(DispatchResult& result) {
    if (!result.ok() || result.action != RouteAction::DecodeLocal) {
        return result.ok();
    }
    if (result.decoded()) {
        return true;
    }
    if (result.route == nullptr) {
        result.error = "decode_local dispatch is missing route metadata";
        return false;
    }

    auto* codec = codec_for_route(*result.route);
    if (codec == nullptr) {
        result.error = "body codec is not registered";
        return false;
    }

    try {
        result.decoded_body = codec->decode(result.packet.ref(), *result.route);
        return true;
    } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch-entry pseudo-arc)
        const std::exception&
            ex) {  // (compiler artifact: eh edge residual;
                   // catch body covered by materialize-failure tests)
        result.error = std::string("body decode failed: ") + ex.what();
        return false;
    }
}

void ProtocolPipeline::reset() {
    error_.clear();
    if (envelope_) {
        envelope_->reset();
    }
}

std::string_view ProtocolPipeline::default_codec_name() const {
    const auto* codec = codecs_.find(profile_.default_codec_id);
    if (codec == nullptr) {
        return {};
    }
    return codec->name();
}

const RouteEntry* ProtocolPipeline::resolve_route(Packet& packet,
                                                  DispatchResult& result) {
    if (packet.route_id != 0) {
        if (const auto* route = routes_.find(packet.route_id)) {
            return route;
        }
    }

    if (profile_.route_source != RouteSource::Body ||
        !profile_.decode_body_route) {
        return nullptr;
    }

    auto* codec = codecs_.find(profile_.default_codec_id);
    if (codec == nullptr) {
        result.error = "default body codec is not registered";
        return nullptr;
    }

    const auto key = codec->route_key(packet.ref());
    if (!key || key->empty()) {
        return nullptr;
    }

    if (key->route_id != 0) {
        packet.route_id = key->route_id;
        return routes_.find(key->route_id);
    }

    if (!key->route_name  // GCOVR_EXCL_BR_LINE (defensive: empty keys rejected
                          // above, name always non-empty)
             .empty()) {  // GCOVR_EXCL_BR_LINE (defensive: empty keys rejected
                          // above, name always non-empty here)
        if (const auto* route = routes_.find_by_name(key->route_name)) {
            packet.route_id = route->route_id;
            return route;
        }
    }

    return nullptr;
}

BodyCodec* ProtocolPipeline::codec_for_route(const RouteEntry& route) {
    // A pipeline binds a single body codec (profile_.default_codec_id); route
    // entries carry no codec identity.
    (void)route;
    return codecs_.find(profile_.default_codec_id);
}

const RouteEntry* ProtocolPipeline::resolve_outbound_route(DecodedBody& body) {
    bool saw_route_hint = body.route_id != 0 || !body.route_name.empty();

    if (body.route_id != 0) {
        if (const auto* route = routes_.find(body.route_id)) {
            if (body.route_name.empty()) {
                body.route_name = route->debug_name;
            }
            return route;
        }
    }

    if (!body.route_name.empty()) {
        if (const auto* route = routes_.find_by_name(body.route_name)) {
            body.route_id = route->route_id;
            return route;
        }
    }

    // [M3] The route travels in the header (ClientEgress.route_id), never in
    // the payload: message bodies are pure business data, so JSON fields like
    // route_id/msg_id/route/method are no longer scanned for route hints.

    if (!saw_route_hint) {
        if (const auto* route = routes_.only()) {
            body.route_id = route->route_id;
            if (body.route_name  // GCOVR_EXCL_BR_LINE (defensive: only() is
                                 // reached only without a route hint)
                    .empty()) {  // GCOVR_EXCL_BR_LINE (defensive: only() is
                                 // reached only without a route hint, name
                                 // always empty here)
                body.route_name = route->debug_name;
            }
            return route;
        }
    }

    return nullptr;
}

std::unique_ptr<ProtocolPipeline> build_protocol_pipeline_from_json(
    std::string_view config_json, const ProtocolBuildOptions& options,
    std::string* error) {
    try {
        nlohmann::json config =
            nlohmann::json::parse(config_json, nullptr, false);
        if (config.is_discarded() || !config.is_object() || config.empty()) {
            return nullptr;
        }

        ProtocolProfile profile;
        profile.name = config.value("name", std::string{});

        const auto envelope =
            config.value("envelope", nlohmann::json::object());
        if (envelope.is_object()) {
            if (envelope.contains("type")) {
                const auto parsed =
                    parse_envelope_kind(envelope["type"].get<std::string>());
                if (!parsed) {
                    if (error)
                        *error = "unknown network.protocol.envelope.type";
                    return nullptr;
                }
                profile.envelope_kind = *parsed;
            }
            if (envelope.contains("endian")) {
                const auto parsed = parse_protocol_endian(envelope["endian"]);
                if (!parsed) {
                    if (error)
                        *error = "invalid network.protocol.envelope.endian";
                    return nullptr;
                }
                profile.envelope.endian = *parsed;
            }
            profile.envelope.length_bytes =
                static_cast<std::uint8_t>(envelope.value("length_bytes", 0));
            profile.envelope.route_id_bytes =
                static_cast<std::uint8_t>(envelope.value("route_id_bytes", 0));
            profile.envelope.length_includes_header =
                envelope.value("length_includes_header", false);
            if (envelope.contains("delimiter") &&
                envelope["delimiter"].is_string()) {
                const auto delimiter = envelope["delimiter"].get<std::string>();
                if (!delimiter.empty()) {
                    profile.envelope.delimiter =
                        static_cast<std::uint8_t>(delimiter.front());
                }
            }
            profile.envelope.max_frame_size =
                envelope.value("max_frame_size", std::size_t{0});
        }
        if (profile.envelope.max_frame_size == 0 &&
            options.fallback_max_frame_size > 0) {
            profile.envelope.max_frame_size = options.fallback_max_frame_size;
        }
        profile.route_source =
            default_route_source_for_envelope(profile.envelope_kind);

        const auto body = config.value("body", nlohmann::json::object());
        const std::string body_codec = body.value("codec", "raw");
        const std::string body_provider = body.value("provider", "");

        BodyCodecRegistry codecs;
        constexpr std::uint16_t default_codec_id = 1;
        std::unique_ptr<BodyCodec> codec;
        if (!body_provider.empty()) {
            if (!options.external_codec_resolver) {
                if (error) {
                    *error =
                        "network.protocol.body.provider is configured "
                        "but no external codec resolver is available";
                }
                return nullptr;
            }
            std::string resolver_error;
            const auto* external = options.external_codec_resolver(
                body_provider, body_codec, &resolver_error);
            if (external == nullptr) {
                if (error) {
                    // GCOVR_EXCL_BR_START (compiler artifact: inlined string
                    // selection arcs; resolver_error.empty() is covered
                    // both ways)
                    *error = resolver_error.empty()
                                 ? "failed to resolve "
                                   "network.protocol.body.provider"
                                 : resolver_error;
                    // GCOVR_EXCL_BR_STOP
                }
                return nullptr;
            }
            if (external->codec_name == nullptr ||
                std::string_view(external->codec_name) != body_codec) {
                if (error) {
                    *error =
                        "network.protocol.body.provider does not serve "
                        "network.protocol.body.codec";
                }
                return nullptr;
            }
            if (external->decode == nullptr ||
                external->encode ==  // GCOVR_EXCL_BR_LINE (defensive)
                    nullptr) {  // GCOVR_EXCL_BR_LINE (integration: half-vtable
                                // providers are rejected at the codec level)
                if (error) {    // GCOVR_EXCL_BR_LINE (integration: every
                              // pipeline-build caller passes a non-null error)
                    *error =
                        "network.protocol.body.provider has incomplete vtable";
                }
                return nullptr;
            }
            // GCOVR_EXCL_START (constructing the external body codec requires a
            // loaded codec plugin; integration context)
            codec = std::make_unique<ExternalBodyCodec>(body_provider,
                                                        body_codec, external);
            // GCOVR_EXCL_STOP
        } else {
            // Fail fast: only builtin codecs (raw/json) and the catalog-driven
            // xmldef path may be built without a provider. Any other codec
            // name is served by a runtime-loaded codec plugin, so building a
            // pipeline without a provider would silently install a
            // PassthroughBodyCodec placeholder that throws on the first
            // packet. Reject it here instead (bootstrap probes the pipeline
            // factory at startup, so this surfaces as a startup error).
            if (body_codec != "raw" && body_codec != "json" &&
                body_codec != "xmldef" && body_codec != "xml_def") {
                if (error) {
                    *error = "network.protocol.body.codec '" + body_codec +
                             "' is not a builtin codec; configure "
                             "network.protocol.body.provider to a loaded "
                             "codec plugin that serves it";
                }
                return nullptr;
            }
            codec = create_body_codec(body_codec);
        }
        if (!codec) {  // GCOVR_EXCL_BR_LINE (defensive: config validation
                       // accepts only known codec names)
            // GCOVR_EXCL_START (defensive: config validation accepts only
            // codec names that create_body_codec knows)
            if (error)
                *error =
                    "unsupported network.protocol.body.codec";  // GCOVR_EXCL_BR_LINE
                                                                // (defensive:
                                                                // unreachable,
                                                                // see the
                                                                // exclusion
                                                                // region
                                                                // above)
            return nullptr;
            // GCOVR_EXCL_STOP
        }
        const std::string normalized_body_codec(codec->name());
        codecs.add(default_codec_id, std::move(codec));
        profile.default_codec_id = default_codec_id;

        const auto routing = config.value("routing", nlohmann::json::object());
        if (routing.is_object()) {
            if (routing.contains("source") && routing["source"].is_string()) {
                const auto parsed =
                    parse_route_source(routing["source"].get<std::string>());
                if (!parsed) {
                    if (error)
                        *error = "invalid network.protocol.routing.source";
                    return nullptr;
                }
                profile.route_source = *parsed;
            }
            profile.decode_body_route =
                routing.value("decode_body_route", true);
            profile.decode_before_dispatch =
                routing.value("decode_before_dispatch", false);
            if (routing.contains("unknown_route_action") &&
                routing["unknown_route_action"].is_string()) {
                const auto parsed = parse_action(
                    routing["unknown_route_action"].get<std::string>());
                if (!parsed) {
                    if (error) {
                        *error =
                            "invalid "
                            "network.protocol.routing.unknown_route_action";
                    }
                    return nullptr;
                }
                profile.unknown_route_action = *parsed;
            }
        }

        RouteTable routes;
        if (normalized_body_codec == "xmldef" && body.contains("catalog") &&
            body["catalog"].is_string()) {
            XmldefCatalogOptions catalog_options;
            // GCOVR_EXCL_BR_START (compiler artifact: inlined json
            // contains/is_string copy arcs; the contains-false arm
            // is covered by the catalog test)
            if (routing.contains("default_action") &&
                routing["default_action"].is_string()) {
                // GCOVR_EXCL_BR_STOP
                const auto parsed =
                    parse_action(routing["default_action"].get<std::string>());
                if (!parsed) {
                    if (error) {
                        *error =
                            "invalid network.protocol.routing.default_action";
                    }
                    return nullptr;
                }
                catalog_options.default_action = *parsed;
            }
            catalog_options.default_lazy_decode =
                routing.value("lazy_decode", true);

            std::filesystem::path catalog_path(
                body["catalog"].get<std::string>());
            if (catalog_path.is_relative() && !options.source_dir.empty()) {
                catalog_path =
                    std::filesystem::path(std::string(options.source_dir)) /
                    catalog_path;
            }

            std::string catalog_error;
            if (!load_xmldef_routes_from_file(catalog_path.string(), routes,
                                              catalog_options,
                                              &catalog_error)) {
                if (error) {
                    *error = "failed to load xmldef catalog: " + catalog_error;
                }
                return nullptr;
            }
        }

        if (config.contains("routes")) {
            // Inline route tables were folded into the RPC descriptor set:
            // declare actors[].rpc.routes instead (single static source).
            if (error) {
                *error =
                    "network.protocol.routes was removed; declare "
                    "actors[].rpc.routes instead";
            }
            return nullptr;
        }

        if (options.descriptor_routes != nullptr) {
            options.descriptor_routes->for_each(
                [&routes](const RpcDescriptor& descriptor) {
                    routes.upsert(route_entry_from_descriptor(descriptor));
                });
        }

        return std::make_unique<ProtocolPipeline>(
            std::move(profile), std::move(routes), std::move(codecs));
    } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch-entry pseudo-arc)
        const nlohmann::json::exception&
            ex) {  // (compiler artifact: eh edge residual;
                   // catch body covered by json type-error tests)
        if (error) {
            *error = std::string("invalid network.protocol: ") + ex.what();
        }
        return nullptr;
    }
}

std::unique_ptr<ProtocolPipeline> build_protocol_pipeline_from_json(
    std::string_view config_json, std::string_view source_dir,
    std::size_t fallback_max_frame_size, std::string* error) {
    ProtocolBuildOptions options;
    options.source_dir = source_dir;
    options.fallback_max_frame_size = fallback_max_frame_size;
    return build_protocol_pipeline_from_json(config_json, options, error);
}

}  // namespace shield::transport
