#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace audio_assist {

struct AudioFrame {
    std::uint64_t timestamp_ms = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint32_t num_samples = 0;
    std::vector<float> data;
};

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
    bool is_default = false;
};

struct CaptureMetrics {
    std::uint32_t input_sample_rate = 0;
    std::uint32_t output_sample_rate = 0;
    std::uint64_t pushed_frames = 0;
    std::uint64_t dropped_frames = 0;
    double capture_latency_ms = 0.0;
    std::wstring active_device_name;
    bool device_healthy = false;
};

enum class CaptureMode {
    EndpointLoopback,
    ApplicationLoopback
};

}  // namespace audio_assist
