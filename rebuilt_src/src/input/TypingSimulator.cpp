#include "TypingSimulator.h"
#include <vector>

TypingSimulator::TypingSimulator()
    : rng_(std::random_device{}()) {}

int TypingSimulator::RandRange(int lo, int hi) {
    if (hi <= lo) return lo;
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(rng_);
}

bool TypingSimulator::AbortRequested() const {
    return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
}

void TypingSimulator::SendUnicodeChar(wchar_t ch) {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wScan = ch;
    in[0].ki.dwFlags = KEYEVENTF_UNICODE;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

void TypingSimulator::SendVirtualKey(WORD vk) {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// The original waits until the hotkey chord (Ctrl/Shift/Alt + trigger) is fully
// released, otherwise the held modifiers corrupt the simulated keystrokes.
void TypingSimulator::WaitForModifiersReleased() {
    for (int waited = 0; waited < 1000; waited += 25) {
        bool held =
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
            (GetAsyncKeyState(VK_SHIFT)   & 0x8000) ||
            (GetAsyncKeyState(VK_MENU)    & 0x8000) ||
            (GetAsyncKeyState(VK_LWIN)    & 0x8000) ||
            (GetAsyncKeyState(VK_RWIN)    & 0x8000) ||
            (GetAsyncKeyState(VK_BACK)    & 0x8000);
        if (!held) return;
        if (AbortRequested()) return;
        Sleep(25);
    }
}

int TypingSimulator::DelayForChar(wchar_t ch) {
    int d = baseDelayMs_;
    if (!humanMode_) return d;

    d += RandRange(15, 60);                       // base per-key jitter
    if (ch == L' ' || ch == L'\t') d += RandRange(20, 100);

    static const wchar_t* kPunct = L",;:{}[]()=+-*/<>|&^%~!@#";
    if (wcschr(kPunct, ch) && RandRange(0, 99) < 45) d += RandRange(30, 130);

    if (RandRange(0, 99) < 5)  d += RandRange(150, 600);   // occasional pause
    if (RandRange(0, 99) < 2)  d += RandRange(800, 2500);  // rare "thinking" pause
    return d;
}

// 3% chance on an alphanumeric char: fat-finger a couple of wrong letters, then
// backspace them away before typing the real character.
void TypingSimulator::MaybeSimulateTypo(wchar_t ch) {
    if (!humanMode_) return;
    if (!iswalnum(ch)) return;
    if (RandRange(0, 99) >= 3) return;

    static const wchar_t* kHome = L"asdfghjkl";
    int n = RandRange(1, 2);
    for (int i = 0; i < n; ++i) {
        SendUnicodeChar(kHome[RandRange(0, 8)]);
        Sleep(RandRange(20, 60));
    }
    Sleep(RandRange(150, 400));
    for (int i = 0; i < n; ++i) {
        SendVirtualKey(VK_BACK);
        Sleep(RandRange(40, 100));
    }
    Sleep(RandRange(80, 220));
}

void TypingSimulator::TypeLine(const std::wstring& line) {
    for (wchar_t ch : line) {
        if (AbortRequested()) return;
        if (ch == L'\r') continue;
        MaybeSimulateTypo(ch);
        SendUnicodeChar(ch);
        int d = DelayForChar(ch);
        if (d > 0) Sleep(static_cast<DWORD>(d));
    }
}

void TypingSimulator::TypeText(const std::wstring& text) {
    if (text.empty()) return;

    WaitForModifiersReleased();
    if (AbortRequested()) return;

    // Split into lines; a real Enter key is pressed between them so code is
    // entered line-by-line instead of collapsing onto one line.
    std::vector<std::wstring> lines;
    std::wstring cur;
    for (wchar_t ch : text) {
        if (ch == L'\n') { lines.push_back(cur); cur.clear(); }
        else if (ch != L'\r') cur.push_back(ch);
    }
    lines.push_back(cur);

    for (size_t i = 0; i < lines.size(); ++i) {
        if (AbortRequested()) return;
        if (i > 0) {
            SendVirtualKey(VK_RETURN);
            int d = baseDelayMs_ > 0 ? baseDelayMs_ : 0;
            if (humanMode_) d += RandRange(40, 140);
            if (d > 0) Sleep(static_cast<DWORD>(d));
        }
        TypeLine(lines[i]);
    }
}
