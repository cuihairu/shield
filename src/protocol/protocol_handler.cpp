#include "shield/protocol/protocol_handler.hpp"

namespace shield::protocol {

void ProtocolHandlerFactory::register_handler(ProtocolType type,
                                              HandlerCreator creator) {
    creators_[type] = std::move(creator);
}

std::unique_ptr<IProtocolHandler> ProtocolHandlerFactory::create_handler(
    ProtocolType type) {
    auto it = creators_.find(type);
    if (it != creators_.end()) {
        return it->second();
    }
    return nullptr;
}

}  // namespace shield::protocol