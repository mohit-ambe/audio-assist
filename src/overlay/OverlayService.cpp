#include "overlay/OverlayService.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <sstream>

namespace audio_assist::overlay {

namespace {

constexpr UINT_PTR kRenderTimerId = 1;
constexpr UINT kRenderIntervalMs = 16;
constexpr float kPi = 3.14159265358979323846f;

std::wstring toLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

float directionAngleDegrees(CoarseDirection direction) {
    switch (direction) {
        case CoarseDirection::Left:
            return 270.0f;
        case CoarseDirection::FrontLeft:
            return 315.0f;
        case CoarseDirection::CenterAmbiguous:
            return 0.0f;
        case CoarseDirection::FrontRight:
            return 45.0f;
        case CoarseDirection::Right:
            return 90.0f;
        case CoarseDirection::RearUnknown:
            return 180.0f;
    }
    return 0.0f;
}

D2D1_POINT_2F pointOnCircle(D2D1_POINT_2F center, float radius, float degrees) {
    const float radians = (degrees - 90.0f) * (kPi / 180.0f);
    return D2D1::Point2F(center.x + radius * std::cos(radians), center.y + radius * std::sin(radians));
}

std::wstring formatLastWin32Error(const wchar_t* operation) {
    const DWORD code = GetLastError();
    std::wostringstream message;
    message << operation << L" failed";
    if (code != ERROR_SUCCESS) {
        message << L" (Win32 error " << code << L")";
    }
    return message.str();
}

}  // namespace

std::uint64_t monotonicNowMs() {
    using clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

const wchar_t* directionName(CoarseDirection direction) {
    switch (direction) {
        case CoarseDirection::Left:
            return L"left";
        case CoarseDirection::FrontLeft:
            return L"front-left";
        case CoarseDirection::CenterAmbiguous:
            return L"front/center";
        case CoarseDirection::FrontRight:
            return L"front-right";
        case CoarseDirection::Right:
            return L"right";
        case CoarseDirection::RearUnknown:
            return L"rear/unknown";
    }
    return L"unknown";
}

OverlayService::OverlayService() = default;

OverlayService::~OverlayService() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

bool OverlayService::initialize(const OverlayConfig& config, HINSTANCE instance) {
    config_ = config;
    instance_ = instance;
    last_error_.clear();

    if (!createDeviceIndependentResources()) {
        return false;
    }

    const wchar_t* class_name = L"AudioAssistDirectionalOverlay";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &OverlayService::windowProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = class_name;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        last_error_ = formatLastWin32Error(L"RegisterClassExW");
        return false;
    }

    const DWORD ex_style = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                           (config_.click_through ? WS_EX_TRANSPARENT : 0);
    hwnd_ = CreateWindowExW(ex_style,
                            class_name,
                            L"Audio Assist Directional Overlay",
                            WS_POPUP,
                            CW_USEDEFAULT,
                            CW_USEDEFAULT,
                            config_.size_px,
                            config_.size_px,
                            nullptr,
                            nullptr,
                            instance_,
                            this);
    if (!hwnd_) {
        last_error_ = formatLastWin32Error(L"CreateWindowExW");
        return false;
    }

    applyWindowStyles();
    updateWindowPlacement();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
    SetTimer(hwnd_, kRenderTimerId, kRenderIntervalMs, nullptr);
    return true;
}

void OverlayService::pushEvent(const VisualEvent& event) {
    if (config_.critical_sounds_only && !isCriticalLabel(event.label)) {
        return;
    }
    if (!isClassEnabled(event.label)) {
        return;
    }

    ActiveEvent active;
    active.event = event;
    active.event.created_at_ms = monotonicNowMs();
    active.expires_at_ms = active.event.created_at_ms + active.event.ttl_ms;

    std::lock_guard<std::mutex> lock(events_mutex_);
    active_events_.push_back(active);
    constexpr std::size_t kMaxActiveEvents = 24;
    if (active_events_.size() > kMaxActiveEvents) {
        active_events_.erase(active_events_.begin(), active_events_.begin() + (active_events_.size() - kMaxActiveEvents));
    }
}

void OverlayService::render() {
    if (!createDeviceResources()) {
        return;
    }

    const std::uint64_t now = monotonicNowMs();
    std::vector<ActiveEvent> snapshot;
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        pruneExpired(now);
        snapshot = active_events_;
    }

    render_target_->BeginDraw();
    render_target_->Clear(theme_.background_key);
    const D2D1_SIZE_F size = render_target_->GetSize();
    drawRing(size);
    for (const ActiveEvent& active : snapshot) {
        drawEvent(active, size, now);
    }

    const HRESULT hr = render_target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discardDeviceResources();
    }
}

void OverlayService::setTheme(const OverlayTheme& theme) {
    theme_ = theme;
}

void OverlayService::setOpacity(float value) {
    config_.opacity = clamp01(value);
    applyWindowStyles();
}

void OverlayService::setClassEnabled(const std::wstring& label, bool enabled) {
    const std::wstring normalized = toLower(label);
    auto it = std::find(disabled_classes_.begin(), disabled_classes_.end(), normalized);
    if (enabled && it != disabled_classes_.end()) {
        disabled_classes_.erase(it);
    } else if (!enabled && it == disabled_classes_.end()) {
        disabled_classes_.push_back(normalized);
    }
}

void OverlayService::setCriticalSoundsOnly(bool enabled) {
    config_.critical_sounds_only = enabled;
}

int OverlayService::runMessageLoop() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK OverlayService::windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* service = reinterpret_cast<OverlayService*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        service = reinterpret_cast<OverlayService*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(service));
        if (service) {
            service->hwnd_ = hwnd;
        }
    }

    if (service) {
        return service->handleMessage(message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT OverlayService::handleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_TIMER:
            if (wparam == kRenderTimerId) {
                render();
                return 0;
            }
            break;
        case WM_DISPLAYCHANGE:
            updateWindowPlacement();
            return 0;
        case WM_SIZE:
            if (render_target_) {
                const UINT width = LOWORD(lparam);
                const UINT height = HIWORD(lparam);
                render_target_->Resize(D2D1::SizeU(width, height));
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kRenderTimerId);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

bool OverlayService::createDeviceIndependentResources() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.GetAddressOf()))) {
        last_error_ = L"D2D1CreateFactory failed";
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf())))) {
        last_error_ = L"DWriteCreateFactory failed";
        return false;
    }
    if (FAILED(dwrite_factory_->CreateTextFormat(L"Segoe UI",
                                                 nullptr,
                                                 DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                 DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL,
                                                 15.0f,
                                                 L"en-us",
                                                 text_format_.GetAddressOf()))) {
        last_error_ = L"CreateTextFormat failed";
        return false;
    }
    text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return true;
}

bool OverlayService::createDeviceResources() {
    if (render_target_) {
        return true;
    }

    RECT rect{};
    GetClientRect(hwnd_, &rect);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(rect.right - rect.left),
                                        static_cast<UINT32>(rect.bottom - rect.top));
    const D2D1_RENDER_TARGET_PROPERTIES target_props =
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED));
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props = D2D1::HwndRenderTargetProperties(hwnd_, size);
    if (FAILED(d2d_factory_->CreateHwndRenderTarget(target_props, hwnd_props, render_target_.GetAddressOf()))) {
        return false;
    }
    if (FAILED(render_target_->CreateSolidColorBrush(theme_.ring, brush_.GetAddressOf()))) {
        discardDeviceResources();
        return false;
    }
    return true;
}

void OverlayService::discardDeviceResources() {
    brush_.Reset();
    render_target_.Reset();
}

void OverlayService::updateWindowPlacement() {
    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    if (config_.monitor_id > 0) {
        struct EnumState {
            int target = 0;
            int index = 0;
            HMONITOR monitor = nullptr;
        } state{config_.monitor_id, 0, monitor};
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR candidate, HDC, LPRECT, LPARAM data) -> BOOL {
            auto* state = reinterpret_cast<EnumState*>(data);
            if (state->index == state->target) {
                state->monitor = candidate;
                return FALSE;
            }
            ++state->index;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&state));
        monitor = state.monitor;
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const RECT work = info.rcWork;
    const int size = std::max(160, config_.size_px);
    const int margin = 32;
    int x = work.left + ((work.right - work.left) - size) / 2;
    int y = work.top + ((work.bottom - work.top) - size) / 2;

    switch (config_.anchor) {
        case OverlayAnchor::TopLeft:
            x = work.left + margin;
            y = work.top + margin;
            break;
        case OverlayAnchor::TopRight:
            x = work.right - size - margin;
            y = work.top + margin;
            break;
        case OverlayAnchor::BottomLeft:
            x = work.left + margin;
            y = work.bottom - size - margin;
            break;
        case OverlayAnchor::BottomRight:
            x = work.right - size - margin;
            y = work.bottom - size - margin;
            break;
        case OverlayAnchor::Center:
            break;
    }

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, size, size, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void OverlayService::applyWindowStyles() {
    if (!hwnd_) {
        return;
    }
    LONG_PTR ex_style = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    ex_style |= WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    if (config_.click_through) {
        ex_style |= WS_EX_TRANSPARENT;
    } else {
        ex_style &= ~WS_EX_TRANSPARENT;
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex_style);
    const BYTE alpha = static_cast<BYTE>(std::round(clamp01(config_.opacity) * 255.0f));
    SetLayeredWindowAttributes(hwnd_, RGB(0, 0, 0), alpha, LWA_ALPHA | LWA_COLORKEY);
}

void OverlayService::pruneExpired(std::uint64_t now_ms) {
    active_events_.erase(std::remove_if(active_events_.begin(), active_events_.end(), [now_ms](const ActiveEvent& active) {
                             return active.expires_at_ms <= now_ms;
                         }),
                         active_events_.end());
}

void OverlayService::drawRing(const D2D1_SIZE_F& size) {
    const D2D1_POINT_2F center = D2D1::Point2F(size.width * 0.5f, size.height * 0.5f);
    const float radius = std::min(size.width, size.height) * 0.34f;
    brush_->SetColor(theme_.ring);
    render_target_->DrawEllipse(D2D1::Ellipse(center, radius, radius), brush_.Get(), 2.0f);
    render_target_->DrawLine(D2D1::Point2F(center.x - 6.0f, center.y),
                             D2D1::Point2F(center.x + 6.0f, center.y),
                             brush_.Get(),
                             1.5f);
    render_target_->DrawLine(D2D1::Point2F(center.x, center.y - 6.0f),
                             D2D1::Point2F(center.x, center.y + 6.0f),
                             brush_.Get(),
                             1.5f);
}

void OverlayService::drawEvent(const ActiveEvent& active, const D2D1_SIZE_F& size, std::uint64_t now_ms) {
    const float lifetime = static_cast<float>(active.event.ttl_ms);
    const float age = static_cast<float>(now_ms - active.event.created_at_ms);
    const float fade = clamp01(1.0f - (age / std::max(lifetime, 1.0f)));
    const float intensity = clamp01(active.event.intensity);
    D2D1_COLOR_F color = colorForLabel(active.event.label);
    color.a *= std::max(0.12f, fade);
    brush_->SetColor(color);

    const D2D1_POINT_2F center = D2D1::Point2F(size.width * 0.5f, size.height * 0.5f);
    const float radius = std::min(size.width, size.height) * (0.30f + 0.055f * intensity);
    const float angle = directionAngleDegrees(active.event.coarse_direction);
    const float half_width = active.event.coarse_direction == CoarseDirection::CenterAmbiguous ? 13.0f : 18.0f;
    const D2D1_POINT_2F p1 = pointOnCircle(center, radius - 16.0f, angle - half_width);
    const D2D1_POINT_2F p2 = pointOnCircle(center, radius + 24.0f + 16.0f * intensity, angle);
    const D2D1_POINT_2F p3 = pointOnCircle(center, radius - 16.0f, angle + half_width);

    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(d2d_factory_->CreatePathGeometry(geometry.GetAddressOf())) &&
        SUCCEEDED(geometry->Open(sink.GetAddressOf()))) {
        sink->BeginFigure(p1, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(p2);
        sink->AddLine(p3);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        render_target_->FillGeometry(geometry.Get(), brush_.Get());
    }

    const D2D1_POINT_2F label_point = pointOnCircle(center, radius + 42.0f, angle);
    const float token_size = 30.0f + 12.0f * intensity;
    render_target_->FillEllipse(D2D1::Ellipse(label_point, token_size * 0.5f, token_size * 0.5f), brush_.Get());

    brush_->SetColor(theme_.text);
    std::wstring icon = L"?";
    const std::wstring label = toLower(active.event.label);
    if (label.find(L"gun") != std::wstring::npos) {
        icon = L"!";
    } else if (label.find(L"foot") != std::wstring::npos) {
        icon = L"F";
    } else if (label.find(L"voice") != std::wstring::npos || label.find(L"ping") != std::wstring::npos) {
        icon = L"V";
    }

    const D2D1_RECT_F icon_rect = D2D1::RectF(label_point.x - 18.0f, label_point.y - 18.0f,
                                             label_point.x + 18.0f, label_point.y + 18.0f);
    render_target_->DrawTextW(icon.c_str(), static_cast<UINT32>(icon.size()), text_format_.Get(), icon_rect, brush_.Get());

    if (!config_.icon_only) {
        const std::wstring text = active.event.label + L" " + directionName(active.event.coarse_direction);
        const D2D1_RECT_F text_rect = D2D1::RectF(label_point.x - 78.0f,
                                                 label_point.y + 20.0f,
                                                 label_point.x + 78.0f,
                                                 label_point.y + 48.0f);
        render_target_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), text_format_.Get(), text_rect, brush_.Get());
    }
}

D2D1_COLOR_F OverlayService::colorForLabel(const std::wstring& label) const {
    const std::wstring normalized = toLower(label);
    if (normalized.find(L"gun") != std::wstring::npos) {
        return theme_.gunshot;
    }
    if (normalized.find(L"foot") != std::wstring::npos) {
        return theme_.footsteps;
    }
    if (normalized.find(L"voice") != std::wstring::npos || normalized.find(L"ping") != std::wstring::npos) {
        return theme_.voice;
    }
    return theme_.other;
}

bool OverlayService::isClassEnabled(const std::wstring& label) const {
    const std::wstring normalized = toLower(label);
    return std::find(disabled_classes_.begin(), disabled_classes_.end(), normalized) == disabled_classes_.end();
}

bool OverlayService::isCriticalLabel(const std::wstring& label) const {
    const std::wstring normalized = toLower(label);
    return normalized.find(L"gun") != std::wstring::npos || normalized.find(L"foot") != std::wstring::npos;
}

}  // namespace audio_assist::overlay
