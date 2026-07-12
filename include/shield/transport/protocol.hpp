// [SHIELD_TRANSPORT] Header-first protocol routing primitives
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct shield_protocol_codec_v1;

namespace shield::transport {

using ByteSpan = std::span<const std::uint8_t>;

enum class Endian : std::uint8_t {
    Big,
    Little,
};

enum class PacketKind : std::uint16_t {
    Message = 0,
    Request = 1,
    Response = 2,
    Push = 3,
    Control = 4,
};

enum class RouteSource : std::uint8_t {
    None,
    Header,
    Body,
};

enum class RouteAction : std::uint8_t {
    DecodeLocal,
    ForwardRaw,
    Drop,
};

enum class EnvelopeKind : std::uint8_t {
    LenPrefix,
    IdLen,
    TypeLen,
    Delimiter,
};

struct PacketRef {
    std::uint32_t route_id = 0;
    std::uint16_t kind = static_cast<std::uint16_t>(PacketKind::Message);
    std::uint16_t flags = 0;
    std::uint32_t seq = 0;
    ByteSpan body;
    ByteSpan raw_frame;

    bool has_header_route() const { return route_id != 0; }
};

struct Packet {
    std::uint32_t route_id = 0;
    std::uint16_t kind = static_cast<std::uint16_t>(PacketKind::Message);
    std::uint16_t flags = 0;
    std::uint32_t seq = 0;
    std::vector<std::uint8_t> body;
    std::vector<std::uint8_t> raw_frame;

    PacketRef ref() const;
};

enum class RouteDirection : std::uint8_t {
    ClientToServer = 1,
    ServerToClient = 2,
    Bidirectional  = 3,
};

struct RoutePolicy {
    RouteAction action = RouteAction::DecodeLocal;
    bool lazy_decode = true;
};

struct RouteEntry {
    std::uint32_t route_id = 0;
    RouteDirection direction = RouteDirection::ClientToServer;
    bool requires_auth = true;
    std::uint16_t codec_id = 0;
    std::uint16_t schema_id = 0;
    PacketKind kind = PacketKind::Message;
    std::string debug_name;
    RoutePolicy policy;
};

class RouteTable {
public:
    bool add(RouteEntry entry);
    void upsert(RouteEntry entry);

    const RouteEntry* find(std::uint32_t route_id) const;
    const RouteEntry* find_by_name(std::string_view route_name) const;
    const RouteEntry* only() const;
    bool contains(std::uint32_t route_id) const;

    void clear();
    std::size_t size() const;

private:
    std::unordered_map<std::uint32_t, RouteEntry> routes_;
    std::unordered_map<std::string, std::uint32_t> route_names_;
};

struct EnvelopeConfig {
    Endian endian = Endian::Big;
    std::uint8_t length_bytes = 0;
    std::uint8_t route_id_bytes = 0;
    bool length_includes_header = false;
    std::uint8_t delimiter = '\n';
    std::size_t max_frame_size = 0;
    RouteSource route_source = RouteSource::Body;
};

class Envelope {
public:
    explicit Envelope(EnvelopeConfig config = {});
    virtual ~Envelope() = default;

    virtual std::string_view name() const = 0;
    virtual std::vector<Packet> feed(const std::uint8_t* data,
                                     std::size_t size) = 0;
    virtual std::vector<std::uint8_t> encode(const PacketRef& packet) = 0;

    const EnvelopeConfig& config() const { return config_; }
    const std::string& error() const { return error_; }
    bool has_error() const { return !error_.empty(); }
    virtual void reset();

protected:
    EnvelopeConfig config_;
    std::vector<std::uint8_t> buffer_;
    std::string error_;
};

class LenPrefixEnvelope final : public Envelope {
public:
    explicit LenPrefixEnvelope(EnvelopeConfig config = {});

    std::string_view name() const override { return "lenprefix"; }
    std::vector<Packet> feed(const std::uint8_t* data,
                             std::size_t size) override;
    std::vector<std::uint8_t> encode(const PacketRef& packet) override;
};

class IdLenEnvelope final : public Envelope {
public:
    explicit IdLenEnvelope(EnvelopeConfig config = {});

    std::string_view name() const override { return "idlen"; }
    std::vector<Packet> feed(const std::uint8_t* data,
                             std::size_t size) override;
    std::vector<std::uint8_t> encode(const PacketRef& packet) override;
};

class TypeLenEnvelope final : public Envelope {
public:
    explicit TypeLenEnvelope(EnvelopeConfig config = {});

    std::string_view name() const override { return "typed_len"; }
    std::vector<Packet> feed(const std::uint8_t* data,
                             std::size_t size) override;
    std::vector<std::uint8_t> encode(const PacketRef& packet) override;
};

class DelimiterEnvelope final : public Envelope {
public:
    explicit DelimiterEnvelope(EnvelopeConfig config = {});

    std::string_view name() const override { return "delimiter"; }
    std::vector<Packet> feed(const std::uint8_t* data,
                             std::size_t size) override;
    std::vector<std::uint8_t> encode(const PacketRef& packet) override;
};

std::unique_ptr<Envelope> create_envelope(EnvelopeKind kind,
                                          EnvelopeConfig config = {});
std::unique_ptr<Envelope> create_envelope(std::string_view kind,
                                          EnvelopeConfig config = {});

struct BodyRouteKey {
    std::uint32_t route_id = 0;
    std::string route_name;

    bool empty() const { return route_id == 0 && route_name.empty(); }
};

struct ProtocolProfile;

struct DecodedBody {
    std::uint32_t route_id = 0;
    std::uint16_t codec_id = 0;
    std::uint16_t schema_id = 0;
    std::string route_name;
    std::vector<std::uint8_t> bytes;
    std::optional<nlohmann::json> message;

    bool has_message() const { return message.has_value(); }
};

class BodyCodec {
public:
    virtual ~BodyCodec() = default;

    virtual std::string_view name() const = 0;
    virtual std::optional<BodyRouteKey> route_key(PacketRef packet);
    virtual DecodedBody decode(PacketRef packet, const RouteEntry& route) = 0;
    virtual std::vector<std::uint8_t> encode(
        const DecodedBody& body, const RouteEntry& route,
        const ProtocolProfile& profile) = 0;
};

class RawBodyCodec final : public BodyCodec {
public:
    std::string_view name() const override { return "raw"; }
    DecodedBody decode(PacketRef packet, const RouteEntry& route) override;
    std::vector<std::uint8_t> encode(const DecodedBody& body,
                                     const RouteEntry& route,
                                     const ProtocolProfile& profile) override;
};

class PassthroughBodyCodec final : public BodyCodec {
public:
    explicit PassthroughBodyCodec(std::string name);

    std::string_view name() const override { return name_; }
    DecodedBody decode(PacketRef packet, const RouteEntry& route) override;
    std::vector<std::uint8_t> encode(const DecodedBody& body,
                                     const RouteEntry& route,
                                     const ProtocolProfile& profile) override;

private:
    std::string name_;
};

class JsonBodyCodec final : public BodyCodec {
public:
    std::string_view name() const override { return "json"; }
    std::optional<BodyRouteKey> route_key(PacketRef packet) override;
    DecodedBody decode(PacketRef packet, const RouteEntry& route) override;
    std::vector<std::uint8_t> encode(const DecodedBody& body,
                                     const RouteEntry& route,
                                     const ProtocolProfile& profile) override;
};

std::unique_ptr<BodyCodec> create_body_codec(std::string_view name);

class ExternalBodyCodec final : public BodyCodec {
public:
    ExternalBodyCodec(std::string provider, std::string name,
                      const shield_protocol_codec_v1* codec);

    std::string_view name() const override { return name_; }
    DecodedBody decode(PacketRef packet, const RouteEntry& route) override;
    std::vector<std::uint8_t> encode(const DecodedBody& body,
                                     const RouteEntry& route,
                                     const ProtocolProfile& profile) override;

private:
    std::string provider_;
    std::string name_;
    const shield_protocol_codec_v1* codec_ = nullptr;
};

struct XmldefCatalogOptions {
    std::uint16_t default_codec_id = 0;
    RouteAction default_action = RouteAction::DecodeLocal;
    bool default_lazy_decode = true;
};

bool load_xmldef_routes_from_string(std::string_view xml, RouteTable& routes,
                                    const XmldefCatalogOptions& options = {},
                                    std::string* error = nullptr);
bool load_xmldef_routes_from_file(std::string_view path, RouteTable& routes,
                                  const XmldefCatalogOptions& options = {},
                                  std::string* error = nullptr);

class BodyCodecRegistry {
public:
    bool add(std::uint16_t codec_id, std::unique_ptr<BodyCodec> codec);
    void upsert(std::uint16_t codec_id, std::unique_ptr<BodyCodec> codec);

    BodyCodec* find(std::uint16_t codec_id);
    const BodyCodec* find(std::uint16_t codec_id) const;
    BodyCodec* find_by_name(std::string_view name);
    const BodyCodec* find_by_name(std::string_view name) const;

    void clear();
    std::size_t size() const;

private:
    std::unordered_map<std::uint16_t, std::unique_ptr<BodyCodec>> codecs_;
    std::unordered_map<std::string, std::uint16_t> codec_names_;
};

struct ProtocolProfile {
    std::string name;
    EnvelopeKind envelope_kind = EnvelopeKind::LenPrefix;
    EnvelopeConfig envelope;
    std::uint16_t default_codec_id = 0;
    RouteSource route_source = RouteSource::Body;
    RouteAction unknown_route_action = RouteAction::Drop;
    bool decode_body_route = true;
    bool decode_before_dispatch = false;
};

struct DispatchResult {
    Packet packet;
    const RouteEntry* route = nullptr;
    RouteAction action = RouteAction::Drop;
    std::optional<DecodedBody> decoded_body;
    std::string error;

    bool ok() const { return error.empty(); }
    bool should_forward_raw() const {
        return action == RouteAction::ForwardRaw;
    }
    bool should_drop() const {
        return action == RouteAction::Drop && error.empty();
    }
    bool decoded() const { return decoded_body.has_value(); }
};

class ProtocolPipeline {
public:
    ProtocolPipeline(ProtocolProfile profile, RouteTable routes,
                     BodyCodecRegistry codecs);

    std::vector<DispatchResult> feed(const std::uint8_t* data,
                                     std::size_t size);
    std::vector<std::uint8_t> encode(const PacketRef& packet);
    std::vector<std::uint8_t> encode_message(DecodedBody body);
    bool materialize_decode(DispatchResult& result);

    const ProtocolProfile& profile() const { return profile_; }
    const Envelope& envelope() const { return *envelope_; }
    RouteTable& routes() { return routes_; }
    const RouteTable& routes() const { return routes_; }
    BodyCodecRegistry& codecs() { return codecs_; }
    const BodyCodecRegistry& codecs() const { return codecs_; }
    std::string_view default_codec_name() const;
    const std::string& error() const { return error_; }
    void reset();

private:
    const RouteEntry* resolve_route(Packet& packet, DispatchResult& result);
    const RouteEntry* resolve_outbound_route(DecodedBody& body);
    BodyCodec* codec_for_route(const RouteEntry& route);

    ProtocolProfile profile_;
    std::unique_ptr<Envelope> envelope_;
    RouteTable routes_;
    BodyCodecRegistry codecs_;
    std::string error_;
};

using ExternalBodyCodecResolver = std::function<const shield_protocol_codec_v1*(
    std::string_view provider, std::string_view codec_name,
    std::string* error)>;

struct ProtocolBuildOptions {
    std::string_view source_dir;
    std::size_t fallback_max_frame_size = 0;
    ExternalBodyCodecResolver external_codec_resolver;
};

std::unique_ptr<ProtocolPipeline> build_protocol_pipeline_from_json(
    std::string_view config_json, const ProtocolBuildOptions& options,
    std::string* error = nullptr);

std::unique_ptr<ProtocolPipeline> build_protocol_pipeline_from_json(
    std::string_view config_json, std::string_view source_dir = {},
    std::size_t fallback_max_frame_size = 0, std::string* error = nullptr);

}  // namespace shield::transport
