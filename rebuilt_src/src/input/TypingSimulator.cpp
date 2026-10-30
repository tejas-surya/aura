#include "TypingSimulator.h"

TypingSimulator::TypingSimulator()
    : rng_(std::random_device{}()) {}

void TypingSimulator::TypeText(const std::wstring& text) {
    for (wchar_t ch : text) {
        TypeChar(ch);
        Sleep(static_cast<DWORD>(CalculateDelay()));
    }
}

void TypingSimulator::TypeChar(wchar_t ch) {
    INPUT inputs[2]{};

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = 0;
    inputs[0].ki.wScan = ch;
    inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 0;
    inputs[1].ki.wScan = ch;
    inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));
}

int TypingSimulator::CalculateDelay() const {
    if (!humanMode_) return baseDelayMs_;

    std::normal_distribution<double> dist(
        static_cast<double>(baseDelayMs_),
        static_cast<double>(baseDelayMs_) * 0.3);

    int delay = static_cast<int>(dist(rng_));
    return std::clamp(delay, baseDelayMs_ / 4, baseDelayMs_ * 3);
}

void TypingSimulator::SendKeyInput(WORD vk, WORD scan, bool keyUp) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = scan;
    input.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &input, sizeof(INPUT));
}
