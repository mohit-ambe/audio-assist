#pragma once

#include <cstdint>
#include <string>

namespace audio_assist::overlay {

enum class CoarseDirection {
    Left,
    FrontLeft,
    CenterAmbiguous,
    FrontRight,
    Right,
    RearUnknown
};

enum class OverlayAnchor {
    Center,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
};

struct OverlayConfig {
    int monitor_id = 0;
    int size_px = 360;
    float opacity = 0.82f;
    bool click_through = true;
    bool icon_only = false;
    bool critical_sounds_only = false;
    std::uint32_t persistence_ms = 650;
    OverlayAnchor anchor = OverlayAnchor::Center;
};

struct VisualEvent {
    std::wstring label;
    float confidence = 0.0f;
    CoarseDirection coarse_direction = CoarseDirection::CenterAmbiguous;
    float intensity = 0.0f;
    std::uint32_t ttl_ms = 650;
    std::uint64_t created_at_ms = 0;
};

}  // namespace audio_assist::overlay
