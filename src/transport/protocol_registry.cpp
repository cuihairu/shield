// [SHIELD_TRANSPORT] Body codec registry: id/name keyed codec lookup.
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

bool BodyCodecRegistry::add(std::uint16_t codec_id,
                            std::unique_ptr<BodyCodec> codec) {
    if (!codec || codecs_.contains(codec_id)) {
        return false;
    }

    const auto codec_name = std::string(codec->name());
    if (!codec_name.empty() && codec_names_.contains(codec_name)) {
        return false;
    }

    codecs_.emplace(codec_id, std::move(codec));
    if (!codec_name.empty()) {
        codec_names_.emplace(codec_name, codec_id);
    }
    return true;
}

void BodyCodecRegistry::upsert(std::uint16_t codec_id,
                               std::unique_ptr<BodyCodec> codec) {
    if (!codec) {
        return;
    }

    const auto existing = codecs_.find(codec_id);
    if (existing != codecs_.end()) {
        const auto old_name = std::string(existing->second->name());
        const auto old_name_it = codec_names_.find(old_name);
        if (old_name_it != codec_names_.end() &&
            old_name_it->second == codec_id) {
            codec_names_.erase(old_name_it);
        }
    }

    const auto codec_name = std::string(codec->name());
    codecs_.insert_or_assign(codec_id, std::move(codec));
    if (!codec_name.empty()) {
        codec_names_.insert_or_assign(codec_name, codec_id);
    }
}

BodyCodec* BodyCodecRegistry::find(std::uint16_t codec_id) {
    const auto it = codecs_.find(codec_id);
    if (it == codecs_.end()) {
        return nullptr;
    }
    return it->second.get();
}

const BodyCodec* BodyCodecRegistry::find(std::uint16_t codec_id) const {
    const auto it = codecs_.find(codec_id);
    if (it == codecs_.end()) {
        return nullptr;
    }
    return it->second.get();
}

BodyCodec* BodyCodecRegistry::find_by_name(std::string_view name) {
    const auto it = codec_names_.find(std::string(name));
    if (it == codec_names_.end()) {
        return nullptr;
    }
    return find(it->second);
}

const BodyCodec* BodyCodecRegistry::find_by_name(std::string_view name) const {
    const auto it = codec_names_.find(std::string(name));
    if (it == codec_names_.end()) {
        return nullptr;
    }
    return find(it->second);
}

void BodyCodecRegistry::clear() {
    codecs_.clear();
    codec_names_.clear();
}

std::size_t BodyCodecRegistry::size() const { return codecs_.size(); }

}  // namespace shield::transport
