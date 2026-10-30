#pragma once
#include <Windows.h>
#include <string>
#include <random>

class TypingSimulator {
public:
    TypingSimulator();

    void TypeText(const std::wstring& text);
    void TypeChar(wchar_t ch);
    void SetDelay(int baseMs) { baseDelayMs_ = baseMs; }
    void SetHumanMode(bool enable) { humanMode_ = enable; }

    int GetDelay() const { return baseDelayMs_; }
    bool IsHumanMode() const { return humanMode_; }

private:
    int CalculateDelay() const;
    void SendKeyInput(WORD vk, WORD scan, bool keyUp);

    int baseDelayMs_ = 200;
    bool humanMode_ = true;
    mutable std::mt19937 rng_;
};
