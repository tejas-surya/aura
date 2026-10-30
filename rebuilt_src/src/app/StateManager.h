#pragma once
#include <string>
#include <vector>

class StateManager {
public:
    void AppendDebugLog(const std::string& entry);

private:
    std::wstring GetDebugLogPath() const;
};
