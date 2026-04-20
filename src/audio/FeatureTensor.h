#pragma once

#include <cstdint>
#include <vector>

namespace audio_assist {

struct FeatureTensor {
    std::uint64_t window_start_ms = 0;
    std::uint64_t window_end_ms = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t mel_bins = 0;
    std::uint32_t time_steps = 0;
    std::vector<float> data;
};

struct FeaturePipelineMetrics {
    std::uint64_t tensors_produced = 0;
    std::uint64_t tensors_dropped = 0;
    std::uint64_t frames_received = 0;
    std::uint64_t samples_buffered = 0;
};

}  // namespace audio_assist
