#pragma once

#include "overlay/DirectionalOverlayTypes.h"

#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace audio_assist::overlay {

struct ParsedInputEvent {
    std::uint64_t window_start_ms = 0;
    std::uint32_t duration_ms = 0;
    float left_rms = 0.0f;
    float right_rms = 0.0f;
    float mono_rms = 0.0f;
    float peak = 0.0f;
    float ild_db = 0.0f;
    float itd_ms = 0.0f;
    float stereo_corr = 0.0f;
    float azimuth_deg = 0.0f;
    float azimuth_confidence = 0.0f;
    std::vector<std::pair<int, float>> front_hemisphere_levels;
    std::vector<VisualEvent> visual_events;
};

std::optional<ParsedInputEvent> parseInputBlock(const std::vector<std::string>& lines);
CoarseDirection estimateDirection(const ParsedInputEvent& event, float class_confidence);
std::vector<VisualEvent> readVisualEventsFromStream(std::istream& input, float min_confidence, std::uint32_t ttl_ms);

}  // namespace audio_assist::overlay
