#include "overlay/DirectionalEventParser.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <regex>
#include <sstream>

namespace audio_assist::overlay {

namespace {

constexpr float kEpsilon = 1.0e-6f;

std::wstring widen(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

float readFloatField(const std::string& line, const std::string& name, float fallback = 0.0f) {
    const std::regex field(name + R"(=([-+]?[0-9]*\.?[0-9]+))");
    std::smatch match;
    if (std::regex_search(line, match, field)) {
        return std::stof(match[1].str());
    }
    return fallback;
}

std::uint64_t readUInt64Field(const std::string& line, const std::string& name, std::uint64_t fallback = 0) {
    const std::regex field(name + R"(=([0-9]+))");
    std::smatch match;
    if (std::regex_search(line, match, field)) {
        return static_cast<std::uint64_t>(std::stoull(match[1].str()));
    }
    return fallback;
}

std::uint32_t readUInt32Field(const std::string& line, const std::string& name, std::uint32_t fallback = 0) {
    return static_cast<std::uint32_t>(readUInt64Field(line, name, fallback));
}

float directionCertainty(const ParsedInputEvent& event) {
    const float lr_sum = std::max(event.left_rms + event.right_rms, kEpsilon);
    const float energy_separation = std::fabs(event.left_rms - event.right_rms) / lr_sum;
    const float decorrelation = std::clamp(1.0f - std::fabs(event.stereo_corr), 0.0f, 1.0f);
    return std::clamp((0.62f * event.azimuth_confidence) + (0.28f * energy_separation) + (0.10f * decorrelation), 0.0f, 1.0f);
}

CoarseDirection directionFromAzimuth(float azimuth_deg) {
    if (azimuth_deg <= -50.0f) {
        return CoarseDirection::Left;
    }
    if (azimuth_deg <= -15.0f) {
        return CoarseDirection::FrontLeft;
    }
    if (azimuth_deg < 15.0f) {
        return CoarseDirection::CenterAmbiguous;
    }
    if (azimuth_deg < 50.0f) {
        return CoarseDirection::FrontRight;
    }
    return CoarseDirection::Right;
}

float eventIntensity(const ParsedInputEvent& event, float confidence) {
    const float peak_term = std::clamp(event.peak, 0.0f, 1.0f);
    const float rms_term = std::clamp(event.mono_rms * 2.5f, 0.0f, 1.0f);
    return std::clamp((0.55f * confidence) + (0.30f * peak_term) + (0.15f * rms_term), 0.0f, 1.0f);
}

}  // namespace

CoarseDirection estimateDirection(const ParsedInputEvent& event, float class_confidence) {
    const float certainty = directionCertainty(event);
    if (class_confidence >= 0.80f && certainty < 0.25f) {
        return CoarseDirection::RearUnknown;
    }

    if (event.azimuth_confidence >= 0.35f) {
        return directionFromAzimuth(event.azimuth_deg);
    }

    const float energy_delta = event.left_rms - event.right_rms;
    const float abs_ild = std::fabs(event.ild_db);
    if (energy_delta > 0.06f || event.ild_db > 3.0f) {
        return abs_ild > 7.0f ? CoarseDirection::Left : CoarseDirection::FrontLeft;
    }
    if (energy_delta < -0.06f || event.ild_db < -3.0f) {
        return abs_ild > 7.0f ? CoarseDirection::Right : CoarseDirection::FrontRight;
    }
    return CoarseDirection::CenterAmbiguous;
}

std::optional<ParsedInputEvent> parseInputBlock(const std::vector<std::string>& lines) {
    if (lines.empty()) {
        return std::nullopt;
    }

    ParsedInputEvent parsed;
    bool saw_window = false;
    bool saw_spatial = false;

    const std::regex class_line(R"(^class_index=([0-9]+)\s+label=([^\s]+)\s+confidence=([-+]?[0-9]*\.?[0-9]+))");
    const std::regex sector(R"(\[([-+]?[0-9]+):([-+]?[0-9]*\.?[0-9]+)\])");

    for (const std::string& line : lines) {
        if (line.rfind("window_start_ms=", 0) == 0) {
            parsed.window_start_ms = readUInt64Field(line, "window_start_ms");
            parsed.duration_ms = readUInt32Field(line, "duration_ms");
            saw_window = true;
            continue;
        }

        if (line.rfind("spatial ", 0) == 0) {
            parsed.left_rms = readFloatField(line, "left_rms");
            parsed.right_rms = readFloatField(line, "right_rms");
            parsed.mono_rms = readFloatField(line, "mono_rms");
            parsed.peak = readFloatField(line, "peak");
            parsed.ild_db = readFloatField(line, "ild_db");
            parsed.itd_ms = readFloatField(line, "itd_ms");
            parsed.stereo_corr = readFloatField(line, "stereo_corr");
            parsed.azimuth_deg = readFloatField(line, "azimuth_deg");
            parsed.azimuth_confidence = readFloatField(line, "azimuth_confidence");
            saw_spatial = true;
            continue;
        }

        if (line.rfind("front_hemisphere_levels", 0) == 0) {
            auto begin = std::sregex_iterator(line.begin(), line.end(), sector);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                parsed.front_hemisphere_levels.emplace_back(std::stoi((*it)[1].str()), std::stof((*it)[2].str()));
            }
            continue;
        }

        std::smatch match;
        if (std::regex_search(line, match, class_line)) {
            const float confidence = std::stof(match[3].str());
            VisualEvent event;
            event.label = widen(match[2].str());
            event.confidence = confidence;
            event.coarse_direction = CoarseDirection::CenterAmbiguous;
            event.intensity = confidence;
            parsed.visual_events.push_back(event);
        }
    }

    if (!saw_window || !saw_spatial || parsed.visual_events.empty()) {
        return std::nullopt;
    }

    return parsed;
}

std::vector<VisualEvent> readVisualEventsFromStream(std::istream& input, float min_confidence, std::uint32_t ttl_ms) {
    std::vector<VisualEvent> output;
    std::vector<std::string> block;
    std::string line;

    auto flush_block = [&]() {
        const auto parsed = parseInputBlock(block);
        block.clear();
        if (!parsed) {
            return;
        }

        for (VisualEvent event : parsed->visual_events) {
            if (event.confidence < min_confidence) {
                continue;
            }
            event.coarse_direction = estimateDirection(*parsed, event.confidence);
            event.intensity = eventIntensity(*parsed, event.confidence);
            event.ttl_ms = ttl_ms;
            output.push_back(event);
        }
    };

    while (std::getline(input, line)) {
        if (line.empty()) {
            flush_block();
            continue;
        }

        if (line.rfind("window_start_ms=", 0) == 0 && !block.empty()) {
            flush_block();
        }
        block.push_back(line);
    }

    flush_block();
    return output;
}

}  // namespace audio_assist::overlay
