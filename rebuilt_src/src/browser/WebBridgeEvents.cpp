#include "WebBridgeEvents.h"
#include <nlohmann/json.hpp>

void WebBridgeEvents::Register(const std::string& eventType, Handler handler) {
    handlers_[eventType] = std::move(handler);
}

void WebBridgeEvents::Dispatch(const std::wstring& jsonMessage) {
    try {
        std::string msg(jsonMessage.size(), '\0');
        for (size_t i = 0; i < jsonMessage.size(); i++)
            msg[i] = static_cast<char>(jsonMessage[i] & 0xFF);
        auto j = nlohmann::json::parse(msg);
        std::string type = j.value("type", "");
        auto it = handlers_.find(type);
        if (it != handlers_.end()) {
            it->second(jsonMessage);
        }
    } catch (...) {}
}
