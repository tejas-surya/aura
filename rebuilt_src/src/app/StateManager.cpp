#include "StateManager.h"
#include "../crypto/CryptoUtils.h"
#include <Windows.h>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

std::wstring StateManager::GetDebugLogPath() const {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return (fs::path(exePath).parent_path() / L"debug_log.enc").wstring();
}

void StateManager::AppendDebugLog(const std::string& entry) {
    auto logPath = GetDebugLogPath();

    std::vector<uint8_t> existing;
    if (fs::exists(logPath)) {
        std::ifstream ifs(logPath, std::ios::binary);
        existing = std::vector<uint8_t>(
            std::istreambuf_iterator<char>(ifs), {});
    }

    auto decrypted = existing.empty()
        ? std::vector<uint8_t>{}
        : CryptoUtils::DecryptAES(existing);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char timestamp[64]{};
    wsprintfA(timestamp, "[%02d:%02d:%02d.%03d] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::string fullEntry = std::string(timestamp) + entry + "\n";
    decrypted.insert(decrypted.end(), fullEntry.begin(), fullEntry.end());

    auto encrypted = CryptoUtils::EncryptAES(decrypted);
    std::ofstream ofs(logPath, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(encrypted.data()), encrypted.size());
}
