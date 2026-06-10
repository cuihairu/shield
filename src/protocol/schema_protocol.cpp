#include "shield/protocol/schema_protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <boost/property_tree/xml_parser.hpp>

namespace shield::protocol {

namespace {

constexpr uint8_t kSchemaMagic[] = {'S', 'H', 'P', '1'};

void append_u8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
    }
}

void append_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
    }
}

void append_bytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& data) {
    out.insert(out.end(), data.begin(), data.end());
}

uint8_t read_u8(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset >= data.size()) {
        throw std::runtime_error("Unexpected end of buffer");
    }
    return data[offset++];
}

uint32_t read_u32(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("Unexpected end of buffer");
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(data[offset++]) << (i * 8);
    }
    return value;
}

uint64_t read_u64(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset + 8 > data.size()) {
        throw std::runtime_error("Unexpected end of buffer");
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[offset++]) << (i * 8);
    }
    return value;
}

std::vector<uint8_t> read_blob(const std::vector<uint8_t>& data, size_t& offset,
                               size_t size) {
    if (offset + size > data.size()) {
        throw std::runtime_error("Unexpected end of buffer");
    }
    std::vector<uint8_t> blob(data.begin() + static_cast<long>(offset),
                              data.begin() + static_cast<long>(offset + size));
    offset += size;
    return blob;
}

FieldType parse_field_type(const std::string& value) {
    if (value == "bool") return FieldType::BOOL;
    if (value == "int32") return FieldType::INT32;
    if (value == "int64") return FieldType::INT64;
    if (value == "uint32") return FieldType::UINT32;
    if (value == "uint64") return FieldType::UINT64;
    if (value == "float") return FieldType::FLOAT;
    if (value == "double") return FieldType::DOUBLE;
    if (value == "bytes") return FieldType::BYTES;
    return FieldType::STRING;
}

MessageKind parse_message_kind(const std::string& value) {
    if (value == "event") return MessageKind::EVENT;
    if (value == "command") return MessageKind::COMMAND;
    if (value == "stream") return MessageKind::STREAM;
    return MessageKind::RPC;
}

MessageDirection parse_message_direction(const std::string& value) {
    if (value == "c2s") return MessageDirection::C2S;
    if (value == "s2c") return MessageDirection::S2C;
    return MessageDirection::BIDIRECTIONAL;
}

}  // namespace

ProtocolValue::ProtocolValue(bool value) : storage_(value) {}
ProtocolValue::ProtocolValue(int8_t value)
    : storage_(static_cast<int64_t>(value)) {}
ProtocolValue::ProtocolValue(int16_t value)
    : storage_(static_cast<int64_t>(value)) {}
ProtocolValue::ProtocolValue(int32_t value)
    : storage_(static_cast<int64_t>(value)) {}
ProtocolValue::ProtocolValue(int64_t value) : storage_(value) {}
ProtocolValue::ProtocolValue(uint8_t value)
    : storage_(static_cast<uint64_t>(value)) {}
ProtocolValue::ProtocolValue(uint16_t value)
    : storage_(static_cast<uint64_t>(value)) {}
ProtocolValue::ProtocolValue(uint32_t value)
    : storage_(static_cast<uint64_t>(value)) {}
ProtocolValue::ProtocolValue(uint64_t value) : storage_(value) {}
ProtocolValue::ProtocolValue(float value)
    : storage_(static_cast<double>(value)) {}
ProtocolValue::ProtocolValue(double value) : storage_(value) {}
ProtocolValue::ProtocolValue(const char* value)
    : storage_(std::string(value ? value : "")) {}
ProtocolValue::ProtocolValue(std::string value) : storage_(std::move(value)) {}
ProtocolValue::ProtocolValue(std::string_view value)
    : storage_(std::string(value)) {}
ProtocolValue::ProtocolValue(const std::vector<uint8_t>& value)
    : storage_(value) {}
ProtocolValue::ProtocolValue(std::vector<uint8_t>&& value)
    : storage_(std::move(value)) {}

FieldType ProtocolValue::field_type() const {
    if (!storage_) return FieldType::BYTES;
    if (std::holds_alternative<bool>(*storage_)) return FieldType::BOOL;
    if (std::holds_alternative<int64_t>(*storage_)) return FieldType::INT64;
    if (std::holds_alternative<uint64_t>(*storage_)) return FieldType::UINT64;
    if (std::holds_alternative<double>(*storage_)) return FieldType::DOUBLE;
    if (std::holds_alternative<std::string>(*storage_)) return FieldType::STRING;
    return FieldType::BYTES;
}

const ProtocolValue::Storage& ProtocolValue::storage() const {
    if (!storage_) {
        throw std::runtime_error("ProtocolValue is null");
    }
    return *storage_;
}

std::string ProtocolValue::to_string() const {
    if (!storage_) return "";
    if (std::holds_alternative<bool>(*storage_)) {
        return std::get<bool>(*storage_) ? "true" : "false";
    }
    if (std::holds_alternative<int64_t>(*storage_)) {
        return std::to_string(std::get<int64_t>(*storage_));
    }
    if (std::holds_alternative<uint64_t>(*storage_)) {
        return std::to_string(std::get<uint64_t>(*storage_));
    }
    if (std::holds_alternative<double>(*storage_)) {
        std::ostringstream oss;
        oss << std::get<double>(*storage_);
        return oss.str();
    }
    if (std::holds_alternative<std::string>(*storage_)) {
        return std::get<std::string>(*storage_);
    }
    const auto& bytes = std::get<std::vector<uint8_t>>(*storage_);
    return std::string(bytes.begin(), bytes.end());
}

bool SchemaRegistry::load_from_xml_file(const std::string& path) {
    try {
        boost::property_tree::ptree tree;
        boost::property_tree::read_xml(path, tree);
        clear();

        auto root = tree.get_child_optional("schema");
        if (!root) {
            last_error_ = "Missing <schema> root";
            return false;
        }

        for (const auto& [key, node] : *root) {
            if (key != "message") {
                continue;
            }

            MessageDefinition definition;
            definition.name = node.get<std::string>("<xmlattr>.name", "");
            definition.id = node.get<uint32_t>("<xmlattr>.id", 0);
            definition.kind =
                parse_message_kind(node.get<std::string>("<xmlattr>.kind", "rpc"));
            definition.direction = parse_message_direction(
                node.get<std::string>("<xmlattr>.direction", "bidi"));
            definition.timeout_ms =
                node.get<uint32_t>("<xmlattr>.timeout_ms", 0);
            definition.compressed =
                node.get<bool>("<xmlattr>.compressed", false);

            for (const auto& [field_key, field_node] : node) {
                if (field_key != "field") {
                    continue;
                }

                MessageFieldDefinition field;
                field.id = field_node.get<uint32_t>("<xmlattr>.id", 0);
                field.name = field_node.get<std::string>("<xmlattr>.name", "");
                field.type = parse_field_type(
                    field_node.get<std::string>("<xmlattr>.type", "string"));
                field.repeated =
                    field_node.get<bool>("<xmlattr>.repeated", false);
                field.required =
                    field_node.get<bool>("<xmlattr>.required", false);
                field.default_value =
                    field_node.get<std::string>("<xmlattr>.default", "");
                definition.fields.push_back(std::move(field));
            }

            messages_.push_back(std::move(definition));
        }

        std::sort(messages_.begin(), messages_.end(),
                  [](const MessageDefinition& lhs, const MessageDefinition& rhs) {
                      return lhs.id < rhs.id;
                  });

        message_by_id_.clear();
        message_by_name_.clear();
        for (size_t i = 0; i < messages_.size(); ++i) {
            message_by_id_[messages_[i].id] = i;
            message_by_name_[messages_[i].name] = i;
        }

        uint64_t hash = 1469598103934665603ULL;
        for (const auto& message : messages_) {
            auto mix = [&hash](auto value) {
                hash ^= static_cast<uint64_t>(value);
                hash *= 1099511628211ULL;
            };
            for (char ch : message.name) {
                mix(static_cast<uint8_t>(ch));
            }
            mix(message.id);
            mix(static_cast<uint32_t>(message.kind));
            mix(static_cast<uint32_t>(message.direction));
            mix(message.timeout_ms);
            mix(message.compressed ? 1U : 0U);
            for (const auto& field : message.fields) {
                for (char ch : field.name) {
                    mix(static_cast<uint8_t>(ch));
                }
                mix(field.id);
                mix(static_cast<uint32_t>(field.type));
                mix(field.repeated ? 1U : 0U);
                mix(field.required ? 1U : 0U);
            }
        }
        schema_hash_ = hash;
        last_error_.clear();
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        clear();
        return false;
    }
}

bool SchemaRegistry::load_from_xml_string(const std::string& xml) {
    std::istringstream input(xml);
    try {
        boost::property_tree::ptree tree;
        boost::property_tree::read_xml(input, tree);
        clear();

        auto root = tree.get_child_optional("schema");
        if (!root) {
            last_error_ = "Missing <schema> root";
            return false;
        }

        for (const auto& [key, node] : *root) {
            if (key != "message") {
                continue;
            }

            MessageDefinition definition;
            definition.name = node.get<std::string>("<xmlattr>.name", "");
            definition.id = node.get<uint32_t>("<xmlattr>.id", 0);
            definition.kind =
                parse_message_kind(node.get<std::string>("<xmlattr>.kind", "rpc"));
            definition.direction = parse_message_direction(
                node.get<std::string>("<xmlattr>.direction", "bidi"));
            definition.timeout_ms =
                node.get<uint32_t>("<xmlattr>.timeout_ms", 0);
            definition.compressed =
                node.get<bool>("<xmlattr>.compressed", false);

            for (const auto& [field_key, field_node] : node) {
                if (field_key != "field") {
                    continue;
                }

                MessageFieldDefinition field;
                field.id = field_node.get<uint32_t>("<xmlattr>.id", 0);
                field.name = field_node.get<std::string>("<xmlattr>.name", "");
                field.type = parse_field_type(
                    field_node.get<std::string>("<xmlattr>.type", "string"));
                field.repeated =
                    field_node.get<bool>("<xmlattr>.repeated", false);
                field.required =
                    field_node.get<bool>("<xmlattr>.required", false);
                field.default_value =
                    field_node.get<std::string>("<xmlattr>.default", "");
                definition.fields.push_back(std::move(field));
            }

            messages_.push_back(std::move(definition));
        }

        std::sort(messages_.begin(), messages_.end(),
                  [](const MessageDefinition& lhs, const MessageDefinition& rhs) {
                      return lhs.id < rhs.id;
                  });

        message_by_id_.clear();
        message_by_name_.clear();
        for (size_t i = 0; i < messages_.size(); ++i) {
            message_by_id_[messages_[i].id] = i;
            message_by_name_[messages_[i].name] = i;
        }

        uint64_t hash = 1469598103934665603ULL;
        for (const auto& message : messages_) {
            auto mix = [&hash](auto value) {
                hash ^= static_cast<uint64_t>(value);
                hash *= 1099511628211ULL;
            };
            for (char ch : message.name) {
                mix(static_cast<uint8_t>(ch));
            }
            mix(message.id);
            mix(static_cast<uint32_t>(message.kind));
            mix(static_cast<uint32_t>(message.direction));
            mix(message.timeout_ms);
            mix(message.compressed ? 1U : 0U);
            for (const auto& field : message.fields) {
                for (char ch : field.name) {
                    mix(static_cast<uint8_t>(ch));
                }
                mix(field.id);
                mix(static_cast<uint32_t>(field.type));
                mix(field.repeated ? 1U : 0U);
                mix(field.required ? 1U : 0U);
            }
        }
        schema_hash_ = hash;
        last_error_.clear();
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        clear();
        return false;
    }
}

void SchemaRegistry::clear() {
    messages_.clear();
    message_by_id_.clear();
    message_by_name_.clear();
    schema_hash_ = 0;
    last_error_.clear();
}

const MessageDefinition* SchemaRegistry::find_message_by_id(
    uint32_t message_id) const {
    auto it = message_by_id_.find(message_id);
    if (it == message_by_id_.end()) {
        return nullptr;
    }
    return &messages_[it->second];
}

const MessageDefinition* SchemaRegistry::find_message_by_name(
    const std::string& name) const {
    auto it = message_by_name_.find(name);
    if (it == message_by_name_.end()) {
        return nullptr;
    }
    return &messages_[it->second];
}

std::vector<uint8_t> encode_message(const MessageDefinition& definition,
                                    const MessageEnvelope& envelope) {
    std::vector<uint8_t> out;
    out.reserve(32);
    out.insert(out.end(), std::begin(kSchemaMagic), std::end(kSchemaMagic));
    append_u64(out, 0);
    append_u32(out, definition.id);
    append_u64(out, envelope.correlation_id);
    append_u64(out, envelope.stream_id);
    append_u32(out, envelope.sequence);
    append_u8(out, envelope.compressed ? 1 : 0);
    append_u32(out, static_cast<uint32_t>(envelope.fields.size()));

    for (const auto& field : envelope.fields) {
        append_u32(out, field.field_id);
        append_u32(out, static_cast<uint32_t>(field.values.size()));
        for (const auto& value : field.values) {
            const auto type = value.field_type();
            append_u8(out, static_cast<uint8_t>(type));
            if (type == FieldType::BOOL) {
                append_u8(out, std::get<bool>(value.storage()) ? 1 : 0);
            } else if (type == FieldType::INT64) {
                append_u64(out, static_cast<uint64_t>(std::get<int64_t>(value.storage())));
            } else if (type == FieldType::UINT64) {
                append_u64(out, std::get<uint64_t>(value.storage()));
            } else if (type == FieldType::DOUBLE) {
                double data = std::get<double>(value.storage());
                static_assert(sizeof(double) == sizeof(uint64_t));
                uint64_t raw = 0;
                std::memcpy(&raw, &data, sizeof(double));
                append_u64(out, raw);
            } else if (type == FieldType::STRING) {
                const auto& text = std::get<std::string>(value.storage());
                append_u32(out, static_cast<uint32_t>(text.size()));
                out.insert(out.end(), text.begin(), text.end());
            } else {
                const auto& bytes = std::get<std::vector<uint8_t>>(value.storage());
                append_u32(out, static_cast<uint32_t>(bytes.size()));
                append_bytes(out, bytes);
            }
        }
    }

    return out;
}

MessageEnvelope decode_message(const SchemaRegistry& registry,
                               const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(kSchemaMagic) + 8) {
        throw std::runtime_error("Message too short");
    }
    if (!std::equal(std::begin(kSchemaMagic), std::end(kSchemaMagic),
                    data.begin())) {
        throw std::runtime_error("Invalid schema protocol magic");
    }

    size_t offset = sizeof(kSchemaMagic);
    (void)read_u64(data, offset);

    MessageEnvelope envelope;
    envelope.message_id = read_u32(data, offset);
    envelope.correlation_id = read_u64(data, offset);
    envelope.stream_id = read_u64(data, offset);
    envelope.sequence = read_u32(data, offset);
    envelope.compressed = read_u8(data, offset) != 0;

    const auto field_count = read_u32(data, offset);
    const auto* definition = registry.find_message_by_id(envelope.message_id);
    if (!definition) {
        throw std::runtime_error("Unknown message id");
    }

    envelope.fields.reserve(field_count);
    for (uint32_t i = 0; i < field_count; ++i) {
        MessageField field;
        field.field_id = read_u32(data, offset);
        const auto value_count = read_u32(data, offset);
        field.values.reserve(value_count);

        for (uint32_t j = 0; j < value_count; ++j) {
            const auto type = static_cast<FieldType>(read_u8(data, offset));
            switch (type) {
                case FieldType::BOOL:
                    field.values.emplace_back(read_u8(data, offset) != 0);
                    break;
                case FieldType::INT32:
                case FieldType::INT64:
                    field.values.emplace_back(static_cast<int64_t>(read_u64(data, offset)));
                    break;
                case FieldType::UINT32:
                case FieldType::UINT64:
                    field.values.emplace_back(read_u64(data, offset));
                    break;
                case FieldType::FLOAT: {
                    uint32_t raw = read_u32(data, offset);
                    float value = 0;
                    std::memcpy(&value, &raw, sizeof(float));
                    field.values.emplace_back(static_cast<double>(value));
                    break;
                }
                case FieldType::DOUBLE: {
                    uint64_t raw = read_u64(data, offset);
                    double value = 0;
                    std::memcpy(&value, &raw, sizeof(double));
                    field.values.emplace_back(value);
                    break;
                }
                case FieldType::STRING: {
                    auto text_size = read_u32(data, offset);
                    auto blob = read_blob(data, offset, text_size);
                    field.values.emplace_back(
                        std::string(blob.begin(), blob.end()));
                    break;
                }
                case FieldType::BYTES: {
                    auto blob_size = read_u32(data, offset);
                    auto blob = read_blob(data, offset, blob_size);
                    field.values.emplace_back(std::move(blob));
                    break;
                }
            }
        }
        envelope.fields.push_back(std::move(field));
    }

    return envelope;
}

bool ProtocolValue::is_null() const { return !storage_.has_value(); }

void PendingRpcRegistry::erase(uint64_t request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(request_id);
}

}  // namespace shield::protocol
