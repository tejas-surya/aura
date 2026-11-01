#include "ConfigManager.h"
#include <fstream>
#include <filesystem>
#include <Windows.h>

namespace fs = std::filesystem;

std::wstring ConfigManager::ResolvePath(const std::wstring& path) const {
    if (!path.empty()) return path;

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return (fs::path(exePath).parent_path() / L"settings.json").wstring();
}

bool ConfigManager::Load(const std::wstring& path) {
    filePath_ = ResolvePath(path);

    std::ifstream ifs(filePath_);
    if (!ifs.is_open()) return false;

    try {
        json_ = nlohmann::json::parse(ifs);
        ParseHotkeys();
        return true;
    } catch (...) {
        return false;
    }
}

bool ConfigManager::Save(const std::wstring& path) {
    std::wstring target = path.empty() ? filePath_ : path;
    if (target.empty()) return false;

    std::ofstream ofs(target);
    if (!ofs.is_open()) return false;

    ofs << json_.dump(2);
    return ofs.good();
}

void ConfigManager::CreateDefaults() {
    json_ = {
        {"theme", "dark"},
        {"width", 350},
        {"height", 500},
        {"panicHidden", false},
        {"opacity", 100},
        {"textSize", 80},
        {"volume", 0},
        {"humanTyping", true},
        {"typeDelayMs", 200},
        {"imagePreprocessing", false},
        {"defaultWebsite", "chatgpt"},
        {"customWebsiteUrl", "google.com"},
        {"hotkeys", nlohmann::json::array({
            {{"id", 4},  {"modifiers", 6}, {"vk", 191}, {"action", "Show / hide app"}},
            {{"id", 1},  {"modifiers", 6}, {"vk", 65},  {"action", "Screenshot"}},
            {{"id", 15}, {"modifiers", 6}, {"vk", 69},  {"action", "Extract UI text"}},
            {{"id", 16}, {"modifiers", 6}, {"vk", 13},  {"action", "Extract & send AI"}},
            {{"id", 14}, {"modifiers", 6}, {"vk", 86},  {"action", "Voice copilot"}},
            {{"id", 5},  {"modifiers", 6}, {"vk", 70},  {"action", "Auto-type highlighted"}},
            {{"id", 6},  {"modifiers", 6}, {"vk", 188}, {"action", "Opacity down"}},
            {{"id", 7},  {"modifiers", 6}, {"vk", 190}, {"action", "Opacity up"}},
            {{"id", 9},  {"modifiers", 6}, {"vk", 68},  {"action", "Click-through"}},
            {{"id", 10}, {"modifiers", 6}, {"vk", 39},  {"action", "Next AI tab"}},
            {{"id", 11}, {"modifiers", 6}, {"vk", 37},  {"action", "Previous AI tab"}},
            {{"id", 23}, {"modifiers", 6}, {"vk", 38},  {"action", "Move window up"}},
            {{"id", 24}, {"modifiers", 6}, {"vk", 40},  {"action", "Move window down"}},
            {{"id", 17}, {"modifiers", 2}, {"vk", 38},  {"action", "Scroll up"}},
            {{"id", 18}, {"modifiers", 2}, {"vk", 40},  {"action", "Scroll down"}},
            {{"id", 8},  {"modifiers", 6}, {"vk", 85},  {"action", "Rotate URL"}},
            {{"id", 12}, {"modifiers", 6}, {"vk", 219}, {"action", "Typing speed -"}},
            {{"id", 13}, {"modifiers", 6}, {"vk", 221}, {"action", "Typing speed +"}},
            {{"id", 19}, {"modifiers", 6}, {"vk", 84},  {"action", "Text-only mode"}},
            {{"id", 20}, {"modifiers", 6}, {"vk", 8},   {"action", "Panic hide"}},
            {{"id", 25}, {"modifiers", 6}, {"vk", 77},  {"action", "Mouse-only mode"}},
            {{"id", 26}, {"modifiers", 6}, {"vk", 187}, {"action", "Zoom in"}},
            {{"id", 27}, {"modifiers", 6}, {"vk", 189}, {"action", "Zoom out"}}
        })}
    };
    ParseHotkeys();
}

void ConfigManager::ParseHotkeys() {
    hotkeys_.clear();
    if (!json_.contains("hotkeys") || !json_["hotkeys"].is_array()) return;

    for (auto& hk : json_["hotkeys"]) {
        HotkeyBinding binding{};
        binding.id = hk.value("id", 0);
        binding.modifiers = hk.value("modifiers", 0);
        binding.vk = hk.value("vk", 0);
        binding.action = hk.value("action", "");
        hotkeys_.push_back(binding);
    }
}

int ConfigManager::GetInt(const std::string& key, int def) const {
    return json_.value(key, def);
}

bool ConfigManager::GetBool(const std::string& key, bool def) const {
    return json_.value(key, def);
}

std::string ConfigManager::GetString(const std::string& key, const std::string& def) const {
    return json_.value(key, def);
}

std::wstring ConfigManager::GetWString(const std::string& key, const std::wstring& def) const {
    if (!json_.contains(key)) return def;
    std::string s = json_[key].get<std::string>();
    return std::wstring(s.begin(), s.end());
}

void ConfigManager::SetInt(const std::string& key, int val) {
    json_[key] = val;
}

void ConfigManager::SetBool(const std::string& key, bool val) {
    json_[key] = val;
}

void ConfigManager::SetString(const std::string& key, const std::string& val) {
    json_[key] = val;
}
