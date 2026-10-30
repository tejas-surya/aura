#include <Windows.h>
#include <objidl.h>
#include "ScreenCapture.h"
#include <gdiplus.h>
#include <memory>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    auto buffer = std::make_unique<uint8_t[]>(size);
    auto encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.get());
    Gdiplus::GetImageEncoders(num, size, encoders);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(encoders[i].MimeType, format) == 0) {
            *pClsid = encoders[i].Clsid;
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<uint8_t> ScreenCapture::CaptureWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return {};
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return {};
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, w, h);
    HGDIOBJ oldBmp = SelectObject(hdcMem, hBitmap);
    BitBlt(hdcMem, 0, 0, w, h, hdcScreen, rc.left, rc.top, SRCCOPY);
    SelectObject(hdcMem, oldBmp);
    auto result = HBitmapToPng(hBitmap, w, h);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return result;
}

std::vector<uint8_t> ScreenCapture::CaptureScreen() {
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    return CaptureRegion(0, 0, w, h);
}

std::vector<uint8_t> ScreenCapture::CaptureRegion(int x, int y, int width, int height) {
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    HGDIOBJ oldBmp = SelectObject(hdcMem, hBitmap);
    BitBlt(hdcMem, 0, 0, width, height, hdcScreen, x, y, SRCCOPY);
    SelectObject(hdcMem, oldBmp);
    auto result = HBitmapToPng(hBitmap, width, height);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return result;
}

std::vector<uint8_t> ScreenCapture::HBitmapToPng(HBITMAP hBitmap, int, int) {
    Gdiplus::Bitmap bitmap(hBitmap, nullptr);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) return {};
    CLSID clsid{};
    if (GetEncoderClsid(L"image/png", &clsid) < 0) return {};
    IStream* stream = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (!stream) return {};
    bitmap.Save(stream, &clsid, nullptr);
    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    ULONG size = static_cast<ULONG>(stat.cbSize.QuadPart);
    std::vector<uint8_t> result(size);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG bytesRead = 0;
    stream->Read(result.data(), size, &bytesRead);
    result.resize(bytesRead);
    stream->Release();
    return result;
}