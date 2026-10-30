#pragma once
#include <Windows.h>
#include <UIAutomation.h>
#include <string>

class TextExtractor {
public:
    TextExtractor();
    ~TextExtractor();

    std::wstring ExtractFromWindow(HWND hwnd);
    std::wstring ExtractFromFocusedElement();

private:
    bool InitializeUIA();
    std::wstring TraverseElement(IUIAutomationElement* element, int depth);

    IUIAutomation* uia_ = nullptr;
    bool initialized_ = false;
    static constexpr int kMaxDepth = 30;
};
