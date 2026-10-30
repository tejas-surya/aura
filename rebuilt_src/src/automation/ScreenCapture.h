#pragma once
#include <Windows.h>
#include <vector>
#include <cstdint>

class ScreenCapture {
public:
    std::vector<uint8_t> CaptureWindow(HWND hwnd);
    std::vector<uint8_t> CaptureScreen();
    std::vector<uint8_t> CaptureRegion(int x, int y, int width, int height);

private:
    std::vector<uint8_t> HBitmapToPng(HBITMAP hBitmap, int width, int height);
};
