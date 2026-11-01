#pragma once
#include <Windows.h>
#include <vector>
#include <cstdint>

class ScreenCapture {
public:
    std::vector<uint8_t> CaptureWindow(HWND hwnd);
    std::vector<uint8_t> CaptureScreen();
    // Primary screenshot path (Ctrl+Shift+A). Tries DXGI Desktop Duplication
    // first (reads the real composited GPU output, so it captures GPU/DWM-
    // composited and otherwise-protected foreground windows that GDI BitBlt
    // misses), and falls back to the legacy GDI BitBlt of the full virtual
    // desktop. This mirrors the original MYOWNAPP.exe capture pipeline.
    std::vector<uint8_t> CaptureVirtualScreen();
    std::vector<uint8_t> CaptureRegion(int x, int y, int width, int height);
    // Encodes a GDI bitmap as PNG (also used by Ctrl+V to paste a clipboard image).
    std::vector<uint8_t> HBitmapToPng(HBITMAP hBitmap, int width, int height);

private:
    // DXGI Desktop Duplication capture of the primary output. Returns empty on
    // any failure/unsupported format so the caller can fall back to GDI.
    std::vector<uint8_t> CaptureViaDXGI();
    // GDI BitBlt of the full virtual desktop (legacy fallback).
    std::vector<uint8_t> CaptureVirtualScreenGDI();
};
