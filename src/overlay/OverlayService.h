#pragma once

#include "overlay/DirectionalOverlayTypes.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <mutex>
#include <string>
#include <vector>

namespace audio_assist::overlay {

struct OverlayTheme {
    D2D1_COLOR_F background_key = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
    D2D1_COLOR_F ring = D2D1::ColorF(0.88f, 0.90f, 0.92f, 0.30f);
    D2D1_COLOR_F text = D2D1::ColorF(0.94f, 0.96f, 0.98f, 0.95f);
    D2D1_COLOR_F gunshot = D2D1::ColorF(0.92f, 0.18f, 0.20f, 0.92f);
    D2D1_COLOR_F footsteps = D2D1::ColorF(0.94f, 0.76f, 0.20f, 0.90f);
    D2D1_COLOR_F voice = D2D1::ColorF(0.22f, 0.62f, 0.95f, 0.86f);
    D2D1_COLOR_F other = D2D1::ColorF(0.72f, 0.76f, 0.82f, 0.72f);
};

class OverlayService {
public:
    OverlayService();
    ~OverlayService();

    OverlayService(const OverlayService&) = delete;
    OverlayService& operator=(const OverlayService&) = delete;

    bool initialize(const OverlayConfig& config, HINSTANCE instance);
    void pushEvent(const VisualEvent& event);
    void render();
    void setTheme(const OverlayTheme& theme);
    void setOpacity(float value);
    void setClassEnabled(const std::wstring& label, bool enabled);
    void setCriticalSoundsOnly(bool enabled);
    int runMessageLoop();
    HWND hwnd() const { return hwnd_; }
    const std::wstring& lastError() const { return last_error_; }

private:
    struct ActiveEvent {
        VisualEvent event;
        std::uint64_t expires_at_ms = 0;
    };

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    bool createDeviceIndependentResources();
    bool createDeviceResources();
    void discardDeviceResources();
    void updateWindowPlacement();
    void applyWindowStyles();
    void pruneExpired(std::uint64_t now_ms);
    void drawRing(const D2D1_SIZE_F& size);
    void drawEvent(const ActiveEvent& active, const D2D1_SIZE_F& size, std::uint64_t now_ms);
    D2D1_COLOR_F colorForLabel(const std::wstring& label) const;
    bool isClassEnabled(const std::wstring& label) const;
    bool isCriticalLabel(const std::wstring& label) const;

    OverlayConfig config_;
    OverlayTheme theme_;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    std::wstring last_error_;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> render_target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format_;
    std::vector<ActiveEvent> active_events_;
    std::vector<std::wstring> disabled_classes_;
    mutable std::mutex events_mutex_;
};

std::uint64_t monotonicNowMs();
const wchar_t* directionName(CoarseDirection direction);

}  // namespace audio_assist::overlay
