#pragma once
#include <string>
#include <functional>
#include <unordered_map>

class WebBridgeEvents {
public:
    using Handler = std::function<void(const std::wstring&)>;

    void Register(const std::string& eventType, Handler handler);
    void Dispatch(const std::wstring& jsonMessage);

private:
    std::unordered_map<std::string, Handler> handlers_;
};
