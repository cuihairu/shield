#include "shield/serialization/serializer.hpp"

namespace shield::serialization {

void SerializerRegistry::register_serializer(SerializationFormat format, std::unique_ptr<ISerializer> serializer) {
    serializers_[format] = std::move(serializer);
}

ISerializer* SerializerRegistry::get_serializer(SerializationFormat format) {
    auto it = serializers_.find(format);
    if (it != serializers_.end()) {
        return it->second.get();
    }
    return nullptr;
}

} // namespace shield::serialization