#pragma once
#include <Windows.h>
#include <vector>

class AppController;
class ConfigManager;

class HotkeyManager {
public:
    explicit HotkeyManager(AppController& controller);
    ~HotkeyManager();

    void RegisterFromConfig(const ConfigManager& config);
    void UnregisterAll();

    static UINT TranslateModifiers(int configMods);

private:
    AppController& controller_;
    HWND hwnd_ = nullptr;

    struct RegisteredHotkey {
        int id;
        UINT modifiers;
        UINT vk;
    };
    std::vector<RegisteredHotkey> registered_;
};
