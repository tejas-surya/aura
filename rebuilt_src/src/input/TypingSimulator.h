#pragma once
#include <Windows.h>
#include <string>
#include <random>

// Human-like keystroke simulator (SendInput), reconstructed from the original
// MYOWNAPP.exe typing engine (FUN_14000bbd0):
//  - waits for the trigger modifiers to be released before typing (ESC aborts),
//  - splits the text into lines and presses a real Enter (VK_RETURN) between
//    them so multi-line code lands line-by-line,
//  - types each character as a Unicode keystroke,
//  - in human mode, varies the per-key delay and occasionally injects a
//    typo-then-backspace and longer "thinking" pauses.
class TypingSimulator {
public:
    TypingSimulator();

    void TypeText(const std::wstring& text);
    void SetDelay(int baseMs) { baseDelayMs_ = baseMs; }
    void SetHumanMode(bool enable) { humanMode_ = enable; }

    int GetDelay() const { return baseDelayMs_; }
    bool IsHumanMode() const { return humanMode_; }

private:
    void WaitForModifiersReleased();
    bool AbortRequested() const;              // ESC pressed
    void SendUnicodeChar(wchar_t ch);         // down + up, KEYEVENTF_UNICODE
    void SendVirtualKey(WORD vk);             // down + up, by virtual-key code
    void TypeLine(const std::wstring& line);
    int  DelayForChar(wchar_t ch);
    void MaybeSimulateTypo(wchar_t ch);
    int  RandRange(int lo, int hi);           // inclusive

    int baseDelayMs_ = 0;
    bool humanMode_ = true;
    std::mt19937 rng_;
};
