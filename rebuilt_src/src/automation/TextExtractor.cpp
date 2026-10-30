#include "TextExtractor.h"
#include <comdef.h>

#pragma comment(lib, "oleaut32.lib")

TextExtractor::TextExtractor() {
    InitializeUIA();
}

TextExtractor::~TextExtractor() {
    if (uia_) uia_->Release();
}

bool TextExtractor::InitializeUIA() {
    HRESULT hr = CoCreateInstance(
        __uuidof(CUIAutomation), nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(IUIAutomation),
        reinterpret_cast<void**>(&uia_));

    initialized_ = SUCCEEDED(hr) && uia_;
    return initialized_;
}

std::wstring TextExtractor::ExtractFromWindow(HWND hwnd) {
    if (!initialized_ || !hwnd) return {};

    IUIAutomationElement* element = nullptr;
    HRESULT hr = uia_->ElementFromHandle(hwnd, &element);
    if (FAILED(hr) || !element) return {};

    std::wstring result = TraverseElement(element, 0);
    element->Release();
    return result;
}

std::wstring TextExtractor::ExtractFromFocusedElement() {
    if (!initialized_) return {};

    IUIAutomationElement* focused = nullptr;
    uia_->GetFocusedElement(&focused);
    if (!focused) return {};

    std::wstring result = TraverseElement(focused, 0);
    focused->Release();
    return result;
}

std::wstring TextExtractor::TraverseElement(IUIAutomationElement* element, int depth) {
    if (!element || depth > kMaxDepth) return {};

    std::wstring result;

    BSTR name = nullptr;
    element->get_CurrentName(&name);
    if (name && SysStringLen(name) > 0) {
        result += name;
        result += L" ";
    }
    SysFreeString(name);

    CONTROLTYPEID controlType = 0;
    element->get_CurrentControlType(&controlType);

    if (controlType == UIA_TextControlTypeId ||
        controlType == UIA_EditControlTypeId ||
        controlType == UIA_DocumentControlTypeId) {
        IUIAutomationTextPattern* textPattern = nullptr;
        element->GetCurrentPatternAs(UIA_TextPatternId,
            __uuidof(IUIAutomationTextPattern),
            reinterpret_cast<void**>(&textPattern));

        if (textPattern) {
            IUIAutomationTextRange* range = nullptr;
            textPattern->get_DocumentRange(&range);
            if (range) {
                BSTR text = nullptr;
                range->GetText(-1, &text);
                if (text && SysStringLen(text) > 0) {
                    result += text;
                    result += L"\n";
                }
                SysFreeString(text);
                range->Release();
            }
            textPattern->Release();
        }
    }

    VARIANT val{};
    element->GetCurrentPropertyValue(UIA_ValueValuePropertyId, &val);
    if (val.vt == VT_BSTR && val.bstrVal && SysStringLen(val.bstrVal) > 0) {
        result += val.bstrVal;
        result += L"\n";
    }
    VariantClear(&val);

    IUIAutomationCondition* trueCondition = nullptr;
    uia_->CreateTrueCondition(&trueCondition);
    if (!trueCondition) return result;

    IUIAutomationElementArray* children = nullptr;
    element->FindAll(TreeScope_Children, trueCondition, &children);
    trueCondition->Release();

    if (children) {
        int count = 0;
        children->get_Length(&count);
        for (int i = 0; i < count && i < 200; ++i) {
            IUIAutomationElement* child = nullptr;
            children->GetElement(i, &child);
            if (child) {
                result += TraverseElement(child, depth + 1);
                child->Release();
            }
        }
        children->Release();
    }

    return result;
}
