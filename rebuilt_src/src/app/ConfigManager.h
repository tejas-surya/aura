#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct HotkeyBinding {
    int id;
    int modifiers;
    int vk;
    std::string action;
};

class ConfigManager {
public:
    bool Load(const std::wstring& path = L"");
    bool Save(const std::wstring& path = L"");
    void CreateDefaults();

    int GetInt(const std::string& key, int def) const;
    bool GetBool(const std::string& key, bool def) const;
    std::string GetString(const std::string& key, const std::string& def) const;
    std::wstring GetWString(const std::string& key, const std::wstring& def) const;

    void SetInt(const std::string& key, int val);
    void SetBool(const std::string& key, bool val);
    void SetString(const std::string& key, const std::string& val);

    const std::vector<HotkeyBinding>& GetHotkeys() const { return hotkeys_; }
    const nlohmann::json& GetJson() const { return json_; }

private:
    std::wstring ResolvePath(const std::wstring& path) const;
    void ParseHotkeys();

    nlohmann::json json_;
    std::vector<HotkeyBinding> hotkeys_;
    std::wstring filePath_;
};
