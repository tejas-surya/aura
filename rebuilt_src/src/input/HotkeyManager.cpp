#include "HotkeyManager.h"
#include "../app/AppController.h"
#include "../app/ConfigManager.h"
#include "../ui/OverlayWindow.h"

HotkeyManager::HotkeyManager(AppController& controller)
    : controller_(controller)
    , hwnd_(controller.GetOverlay().GetHwnd()) {}

HotkeyManager::~HotkeyManager() {
    UnregisterAll();
}

UINT HotkeyManager::TranslateModifiers(int configMods) {
    UINT mods = 0;
    if (configMods & 1) mods |= MOD_ALT;
    if (configMods & 2) mods |= MOD_CONTROL;
    if (configMods & 4) mods |= MOD_SHIFT;
    if (configMods & 8) mods |= MOD_WIN;
    return mods | MOD_NOREPEAT;
}

void HotkeyManager::RegisterFromConfig(const ConfigManager& config) {
    UnregisterAll();

    for (const auto& hk : config.GetHotkeys()) {
        UINT mods = TranslateModifiers(hk.modifiers);
        UINT vk = static_cast<UINT>(hk.vk);

        if (RegisterHotKey(hwnd_, hk.id, mods, vk)) {
            registered_.push_back({hk.id, mods, vk});
        }
    }
}

void HotkeyManager::UnregisterAll() {
    for (auto& hk : registered_) {
        UnregisterHotKey(hwnd_, hk.id);
    }
    registered_.clear();
}
