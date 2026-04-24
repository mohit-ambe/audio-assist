#include "overlay/OverlayService.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace audio_assist::overlay {
namespace {

constexpr wchar_t kWindowClass[] = L"AudioAssistOverlayWindow";
constexpr COLORREF kTransparentColor = RGB(1, 2, 3);

COLORREF colorForEnergy(float energy) {
    const auto clamped = std::clamp(energy, 0.0F, 1.0F);
    const auto red = static_cast<int>(80.0F + 70.0F * clamped);
    const auto green = static_cast<int>(205.0F + 40.0F * clamped);
    const auto blue = static_cast<int>(230.0F + 25.0F * clamped);
    return RGB(red, green, blue);
}

std::wstring toWide(const std::string& text) {
    if (text.empty()) {
        return L"";
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (required <= 0) {
        return std::wstring(text.begin(), text.end());
    }

    std::wstring wide(static_cast<std::size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), required);
    return wide;
}

std::wstring bestLabel(const inference::InferenceResult& result) {
    if (result.predictions.empty()) {
        return L"Listening";
    }

    std::wostringstream out;
    out << toWide(result.predictions.front().label) << L" "
        << static_cast<int>(std::round(result.predictions.front().confidence * 100.0F)) << L"%";
    return out.str();
}

POINT pointOnCircle(int center, int radius, float angle_degrees) {
    const float radians = (angle_degrees - 90.0F) * 3.14159265F / 180.0F;
    return POINT{
        center + static_cast<int>(std::cos(radians) * radius),
        center + static_cast<int>(std::sin(radians) * radius)
    };
}

float sectorEnergyAt(const inference::InferenceResult& result, float angle_degrees) {
    if (result.spatial_audio.front_hemisphere_levels.empty()) {
        const float distance = std::abs(angle_degrees - result.spatial_audio.estimated_azimuth_degrees) / 70.0F;
        const float lobe = std::pow(std::max(0.0F, 1.0F - distance), 2.2F);
        return std::clamp(lobe * result.spatial_audio.peak, 0.0F, 1.0F);
    }

    const auto& sectors = result.spatial_audio.front_hemisphere_levels;
    if (angle_degrees <= sectors.front().angle_degrees) {
        return sectors.front().energy;
    }
    if (angle_degrees >= sectors.back().angle_degrees) {
        return sectors.back().energy;
    }

    for (std::size_t i = 0; i + 1 < sectors.size(); ++i) {
        const auto& left = sectors[i];
        const auto& right = sectors[i + 1];
        if (angle_degrees >= left.angle_degrees && angle_degrees <= right.angle_degrees) {
            const float span = right.angle_degrees - left.angle_degrees;
            const float t = span <= 0.0F ? 0.0F : (angle_degrees - left.angle_degrees) / span;
            return left.energy + (right.energy - left.energy) * t;
        }
    }

    return 0.0F;
}

float strongestSectorEnergy(const inference::InferenceResult& result) {
    float strongest = 0.0F;
    for (const auto& sector : result.spatial_audio.front_hemisphere_levels) {
        strongest = std::max(strongest, sector.energy);
    }
    return strongest;
}

LRESULT CALLBACK overlayWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* service = reinterpret_cast<OverlayService*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        service = static_cast<OverlayService*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(service));
    }

    if (service != nullptr) {
        return static_cast<LRESULT>(service->windowProc(hwnd, message, wparam, lparam));
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

OverlayService::OverlayService(OverlayConfig config) : config_(config) {}

OverlayService::~OverlayService() {
    stop();
}

bool OverlayService::start() {
    if (running_.exchange(true)) {
        return true;
    }

    thread_ = std::thread(&OverlayService::windowThread, this);
    return true;
}

void OverlayService::stop() {
    const bool was_running = running_.exchange(false);

    if (was_running) {
        if (HWND hwnd = static_cast<HWND>(hwnd_.load())) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
    } else if (HWND hwnd = static_cast<HWND>(hwnd_.load())) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

void OverlayService::update(const inference::InferenceResult& result) {
    {
        std::scoped_lock lock(state_mutex_);
        state_.result = result;
        state_.has_result = true;
    }

    if (HWND hwnd = static_cast<HWND>(hwnd_.load())) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void OverlayService::windowThread() {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = overlayWindowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&window_class);

    const int size = std::max(config_.size_px, 160);
    const int screen_width = GetSystemMetrics(SM_CXSCREEN);
    const int screen_height = GetSystemMetrics(SM_CYSCREEN);
    const int x = std::max(0, (screen_width - size) / 2);
    const int y = std::max(0, (screen_height - size) / 2);

    DWORD ex_style = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    if (config_.click_through) {
        ex_style |= WS_EX_TRANSPARENT;
    }

    HWND hwnd = CreateWindowExW(
        ex_style,
        kWindowClass,
        L"Audio Assist",
        WS_POPUP,
        x,
        y,
        size,
        size,
        nullptr,
        nullptr,
        instance,
        this);

    if (hwnd == nullptr) {
        running_.store(false);
        return;
    }

    const BYTE alpha = static_cast<BYTE>(std::clamp(config_.opacity, 0.15F, 1.0F) * 255.0F);
    SetLayeredWindowAttributes(hwnd, kTransparentColor, alpha, LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    hwnd_.store(hwnd);

    MSG message{};
    while (running_.load() && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    hwnd_.store(nullptr);
}

long long OverlayService::windowProc(void* raw_hwnd, unsigned int message, unsigned long long wparam, long long lparam) {
    HWND hwnd = static_cast<HWND>(raw_hwnd);
    switch (message) {
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
    }
}

void OverlayService::paint(void* raw_hwnd) {
    HWND hwnd = static_cast<HWND>(raw_hwnd);

    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rect{};
    GetClientRect(hwnd, &rect);

    HBRUSH background = CreateSolidBrush(kTransparentColor);
    FillRect(dc, &rect, background);
    DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 238, 240));

    inference::InferenceResult result;
    bool has_result = false;
    {
        std::scoped_lock lock(state_mutex_);
        result = state_.result;
        has_result = state_.has_result;
    }

    const int width = rect.right - rect.left;
    const int center = width / 2;
    const int radius = std::max(48, center - 28);

    HPEN listening_pen = CreatePen(PS_SOLID, 1, RGB(56, 118, 128));
    HGDIOBJ old_pen = SelectObject(dc, listening_pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, center - radius, center - radius, center + radius, center + radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(listening_pen);

    if (has_result && result.spatial_audio.peak >= config_.silence_peak_threshold) {
        constexpr int kBarCount = 36;
        const int inner_radius = static_cast<int>(radius * 0.70F);
        const float peak_scale = std::clamp(result.spatial_audio.peak * 2.2F, 0.35F, 1.0F);
        const float sector_reference = std::max(strongestSectorEnergy(result), 0.001F);

        for (int i = 0; i < kBarCount; ++i) {
            const float angle = -90.0F + (180.0F * static_cast<float>(i) / static_cast<float>(kBarCount - 1));
            const float directional_energy = std::clamp(sectorEnergyAt(result, angle), 0.0F, 1.0F);
            const float directional_contrast = std::pow(std::clamp(directional_energy / sector_reference, 0.0F, 1.0F), 1.75F);
            const float ripple = 0.025F * (1.0F + std::sin(static_cast<float>(i) * 1.7F + result.spatial_audio.peak * 18.0F));
            const float energy = std::clamp(directional_contrast * peak_scale + ripple, 0.0F, 1.0F);
            const int outer_radius = inner_radius + static_cast<int>((radius - inner_radius) * std::clamp(energy, 0.12F, 1.0F));
            const POINT inner = pointOnCircle(center, inner_radius, angle);
            const POINT outer = pointOnCircle(center, outer_radius, angle);

            HPEN bar_pen = CreatePen(PS_SOLID, energy > 0.45F ? 3 : 2, colorForEnergy(energy));
            HGDIOBJ previous_pen = SelectObject(dc, bar_pen);
            MoveToEx(dc, inner.x, inner.y, nullptr);
            LineTo(dc, outer.x, outer.y);
            SelectObject(dc, previous_pen);
            DeleteObject(bar_pen);
        }

        HPEN glow_pen = CreatePen(PS_SOLID, 1, colorForEnergy(std::clamp(result.spatial_audio.peak, 0.0F, 1.0F)));
        HGDIOBJ previous_pen = SelectObject(dc, glow_pen);
        HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, center - inner_radius, center - inner_radius, center + inner_radius, center + inner_radius);
        SelectObject(dc, previous_brush);
        SelectObject(dc, previous_pen);
        DeleteObject(glow_pen);

        if (!config_.icon_only && !result.predictions.empty() &&
            result.predictions.front().confidence >= config_.min_confidence) {
            const std::wstring text = bestLabel(result);
            RECT text_rect{12, width - 48, width - 12, width - 14};
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &text_rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    EndPaint(hwnd, &ps);
}

}  // namespace audio_assist::overlay
