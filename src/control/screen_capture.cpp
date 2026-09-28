// src/control/screen_capture.cpp
#include "screen_capture.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <cstring>

namespace runner::control {

namespace {

template <typename T>
class ComRef {
public:
    ComRef() = default;
    ~ComRef() { reset(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    T** put() { reset(); return &ptr_; }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    void reset() { if (ptr_) { ptr_->Release(); ptr_ = nullptr; } }
private:
    T* ptr_ = nullptr;
};

// 호출 스레드의 COM 초기화 상태를 존중하는 RAII.
class ComScope {
public:
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns_ = SUCCEEDED(hr);                 // S_OK 또는 S_FALSE → 짝 맞춰 해제
        usable_ = owns_ || hr == RPC_E_CHANGED_MODE;  // 이미 STA로 초기화된 스레드도 사용 가능
    }
    ~ComScope() { if (owns_) CoUninitialize(); }
    bool usable() const { return usable_; }
private:
    bool owns_ = false;
    bool usable_ = false;
};

std::string hr_text(const char* step, HRESULT hr) {
    char buf[96];
    wsprintfA(buf, "%s failed (hr=0x%08lX)", step, static_cast<unsigned long>(hr));
    return buf;
}

}  // namespace

bool enable_per_monitor_dpi_awareness() {
    // ERROR_ACCESS_DENIED only means awareness was already set; inspect actual level.
    using SetFn = BOOL(WINAPI*)(HANDLE);
    using GetFn = HANDLE(WINAPI*)();
    using AwarenessFn = int(WINAPI*)(HANDLE);
    const auto module = GetModuleHandleW(L"user32.dll");
    if (!module) return false;
    const auto set = reinterpret_cast<SetFn>(GetProcAddress(module, "SetProcessDpiAwarenessContext"));
    const auto get = reinterpret_cast<GetFn>(GetProcAddress(module, "GetThreadDpiAwarenessContext"));
    const auto awareness = reinterpret_cast<AwarenessFn>(GetProcAddress(module, "GetAwarenessFromDpiAwarenessContext"));
    if (!set || !get || !awareness) return false;
    set(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4)));
    return awareness(get()) == 2; // DPI_AWARENESS_PER_MONITOR_AWARE
}

void primary_screen_size(int& width, int& height) {
    width = GetSystemMetrics(SM_CXSCREEN);
    height = GetSystemMetrics(SM_CYSCREEN);
}

CaptureResult capture_primary_display_png(const CaptureLimits& limits) {
    CaptureResult r;
    int w = 0, h = 0;
    primary_screen_size(w, h);
    if (w <= 0 || h <= 0) { r.error = "screen size unavailable"; return r; }
    if (static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) > limits.max_pixels) {
        r.error = "screen exceeds pixel limit";
        return r;
    }

    // 1) 화면 → 32bpp top-down DIB
    HDC screen = GetDC(nullptr);
    if (!screen) { r.error = "GetDC failed"; return r; }
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = mem ? CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    bool blit_ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        blit_ok = BitBlt(mem, 0, 0, w, h, screen, 0, 0, SRCCOPY | CAPTUREBLT) != FALSE;
        SelectObject(mem, old);
        GdiFlush();
    }
    ReleaseDC(nullptr, screen);
    if (!blit_ok) {
        if (dib) DeleteObject(dib);
        if (mem) DeleteDC(mem);
        r.error = "BitBlt failed (desktop may be locked or not interactive)";
        return r;
    }

    const UINT stride = static_cast<UINT>(w) * 4u;
    const UINT buffer_size = stride * static_cast<UINT>(h);
    auto* px = static_cast<std::uint8_t*>(bits);
    for (UINT i = 3; i < buffer_size; i += 4) px[i] = 0xFF;  // GDI alpha는 정의되지 않으므로 불투명 처리

    // 2) DIB → PNG (WIC, 메모리 스트림)
    std::string err;
    {
        ComScope com;
        ComRef<IWICImagingFactory> factory;
        ComRef<IWICBitmapEncoder> encoder;
        ComRef<IWICBitmapFrameEncode> frame;
        ComRef<IStream> stream;
        HRESULT hr = com.usable() ? S_OK : E_FAIL;
        if (FAILED(hr)) err = "COM init failed";

        if (SUCCEEDED(hr)) {
            hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWICImagingFactory, reinterpret_cast<void**>(factory.put()));
            if (FAILED(hr)) err = hr_text("CoCreateInstance(WIC)", hr);
        }
        if (SUCCEEDED(hr)) {
            hr = CreateStreamOnHGlobal(nullptr, TRUE, stream.put());
            if (FAILED(hr)) err = hr_text("CreateStreamOnHGlobal", hr);
        }
        if (SUCCEEDED(hr)) {
            hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
            if (FAILED(hr)) err = hr_text("CreateEncoder", hr);
        }
        if (SUCCEEDED(hr)) {
            hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
            if (FAILED(hr)) err = hr_text("Encoder.Initialize", hr);
        }
        if (SUCCEEDED(hr)) {
            hr = encoder->CreateNewFrame(frame.put(), nullptr);
            if (FAILED(hr)) err = hr_text("CreateNewFrame", hr);
        }
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h));
        if (SUCCEEDED(hr)) {
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            hr = frame->SetPixelFormat(&fmt);
            if (SUCCEEDED(hr) && !IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) {
                hr = E_FAIL;
                err = "PNG encoder rejected 32bppBGRA";
            }
        }
        if (SUCCEEDED(hr)) hr = frame->WritePixels(static_cast<UINT>(h), stride, buffer_size, px);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        if (FAILED(hr) && err.empty()) err = hr_text("PNG encode", hr);

        if (SUCCEEDED(hr)) {
            STATSTG stat{};
            hr = stream->Stat(&stat, STATFLAG_NONAME);
            HGLOBAL hg = nullptr;
            if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(stream.get(), &hg);
            if (SUCCEEDED(hr)) {
                const std::size_t n = static_cast<std::size_t>(stat.cbSize.QuadPart);
                if (n == 0 || n > limits.max_png_bytes) {
                    err = n == 0 ? "empty PNG" : "PNG exceeds size limit";
                } else if (void* p = GlobalLock(hg)) {
                    r.png.assign(static_cast<std::uint8_t*>(p), static_cast<std::uint8_t*>(p) + n);
                    GlobalUnlock(hg);
                } else {
                    err = "GlobalLock failed";
                }
            } else {
                err = hr_text("stream read", hr);
            }
        }
    }  // COM 객체를 COM 해제 전에 모두 Release

    DeleteObject(dib);
    DeleteDC(mem);

    if (!err.empty() || r.png.empty()) {
        r.png.clear();
        r.error = err.empty() ? "PNG encode failed" : err;
        return r;
    }
    r.ok = true;
    r.width = w;
    r.height = h;
    return r;
}

}  // namespace runner::control
