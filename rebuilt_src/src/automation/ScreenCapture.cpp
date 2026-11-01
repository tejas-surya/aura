#include <Windows.h>
#include <objidl.h>
#include "ScreenCapture.h"
#include <gdiplus.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <memory>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

template <typename T>
static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static std::vector<uint8_t> GdiplusBitmapToPng(Gdiplus::Bitmap& bitmap);

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

std::vector<uint8_t> ScreenCapture::CaptureVirtualScreen() {
    // Prefer DXGI Desktop Duplication (captures the real composited GPU output,
    // so it works for GPU/DWM-composited or protected foreground windows that
    // GDI BitBlt renders as the empty desktop background). Fall back to GDI if
    // DXGI is unsupported or returns nothing — mirrors the original MYOWNAPP.exe.
    std::vector<uint8_t> dxgi = CaptureViaDXGI();
    if (!dxgi.empty()) return dxgi;
    return CaptureVirtualScreenGDI();
}

std::vector<uint8_t> ScreenCapture::CaptureVirtualScreenGDI() {
    // Original behaviour: grab the full virtual screen spanning every monitor.
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC hdcScreen = GetDC(nullptr);
    // Fall back to the primary device resolution if virtual metrics are unavailable.
    if (w <= 0) w = GetDeviceCaps(hdcScreen, HORZRES);
    if (h <= 0) h = GetDeviceCaps(hdcScreen, VERTRES);

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, w, h);
    HGDIOBJ oldBmp = SelectObject(hdcMem, hBitmap);
    // SRCCOPY | CAPTUREBLT so layered/overlay windows are included, matching the original.
    BitBlt(hdcMem, 0, 0, w, h, hdcScreen, x, y, SRCCOPY | CAPTUREBLT);
    SelectObject(hdcMem, oldBmp);
    auto result = HBitmapToPng(hBitmap, w, h);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return result;
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

// Encodes an in-memory GDI+ bitmap to PNG bytes (used by the DXGI path, which
// builds a bitmap over mapped GPU memory rather than an HBITMAP).
static std::vector<uint8_t> GdiplusBitmapToPng(Gdiplus::Bitmap& bitmap) {
    if (bitmap.GetLastStatus() != Gdiplus::Ok) return {};
    CLSID clsid{};
    if (GetEncoderClsid(L"image/png", &clsid) < 0) return {};
    IStream* stream = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (!stream) return {};
    std::vector<uint8_t> result;
    if (bitmap.Save(stream, &clsid, nullptr) == Gdiplus::Ok) {
        STATSTG stat{};
        stream->Stat(&stat, STATFLAG_NONAME);
        ULONG size = static_cast<ULONG>(stat.cbSize.QuadPart);
        result.resize(size);
        LARGE_INTEGER zero{};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG bytesRead = 0;
        stream->Read(result.data(), size, &bytesRead);
        result.resize(bytesRead);
    }
    stream->Release();
    return result;
}

// DXGI Desktop Duplication capture of the primary output, reconstructed from the
// original MYOWNAPP.exe pipeline:
//   D3D11CreateDevice -> IDXGIDevice -> GetAdapter -> EnumOutputs(0) ->
//   IDXGIOutput1 -> DuplicateOutput -> AcquireNextFrame (500ms, 10 retries,
//   skipping empty frames) -> staging CreateTexture2D(CPU_READ) -> CopyResource
//   -> Map -> GDI+ bitmap -> PNG.
// Returns {} on any failure or an unhandled surface format so the caller falls
// back to GDI BitBlt.
std::vector<uint8_t> ScreenCapture::CaptureViaDXGI() {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(hr) || !device || !context) {
        SafeRelease(context);
        SafeRelease(device);
        return {};
    }

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    IDXGIOutput1* output1 = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;
    IDXGIResource* desktopResource = nullptr;
    ID3D11Texture2D* acquiredTex = nullptr;
    ID3D11Texture2D* stagingTex = nullptr;
    std::vector<uint8_t> result;

    auto cleanup = [&]() {
        SafeRelease(stagingTex);
        SafeRelease(acquiredTex);
        SafeRelease(desktopResource);
        SafeRelease(duplication);
        SafeRelease(output1);
        SafeRelease(output);
        SafeRelease(adapter);
        SafeRelease(dxgiDevice);
        SafeRelease(context);
        SafeRelease(device);
    };

    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)))) { cleanup(); return {}; }
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) { cleanup(); return {}; }
    if (FAILED(adapter->EnumOutputs(0, &output))) { cleanup(); return {}; }
    if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1)))) { cleanup(); return {}; }
    if (FAILED(output1->DuplicateOutput(device, &duplication))) { cleanup(); return {}; }

    // Acquire a non-empty frame, retrying past timeouts (matches original: 500ms
    // timeout, up to 10 attempts, releasing empty frames in between).
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    int tries = 10;
    bool haveFrame = false;
    while (tries-- > 0) {
        SafeRelease(desktopResource);
        hr = duplication->AcquireNextFrame(500, &frameInfo, &desktopResource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) { cleanup(); return {}; }
        if (frameInfo.LastPresentTime.QuadPart != 0 && desktopResource) {
            haveFrame = true;
            break;
        }
        // Empty frame (only mouse moved); release and retry.
        duplication->ReleaseFrame();
    }
    if (!haveFrame || !desktopResource) { cleanup(); return {}; }

    if (FAILED(desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&acquiredTex)))) {
        duplication->ReleaseFrame();
        cleanup();
        return {};
    }

    D3D11_TEXTURE2D_DESC desc{};
    acquiredTex->GetDesc(&desc);

    // Only the common desktop format is converted directly; anything else (e.g.
    // HDR R10G10B10A2) falls back to GDI by returning empty.
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        duplication->ReleaseFrame();
        cleanup();
        return {};
    }

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex))) {
        duplication->ReleaseFrame();
        cleanup();
        return {};
    }

    context->CopyResource(stagingTex, acquiredTex);
    duplication->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
        // DXGI B8G8R8A8 memory order (B,G,R,A) matches GDI+ 32bpp. Use RGB to
        // emit an opaque PNG and avoid premultiplied-alpha surprises. The bitmap
        // references the mapped memory, so encode before unmapping.
        Gdiplus::Bitmap bitmap(
            static_cast<INT>(desc.Width), static_cast<INT>(desc.Height),
            static_cast<INT>(mapped.RowPitch), PixelFormat32bppRGB,
            static_cast<BYTE*>(mapped.pData));
        result = GdiplusBitmapToPng(bitmap);
        context->Unmap(stagingTex, 0);
    }

    cleanup();
    return result;
}