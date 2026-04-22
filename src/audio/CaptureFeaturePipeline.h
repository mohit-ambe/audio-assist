#pragma once

#include "audio/CaptureAudioFrame.h"
#include "audio/CaptureFeatureTensor.h"
#include "audio/CaptureRingBuffer.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace audio_assist::capture {

class CaptureFeaturePipeline {
public:
    CaptureFeaturePipeline();

    void pushMonoFrame(const CaptureAudioFrame& frame);
    std::optional<CaptureFeatureTensor> getNextTensor();
    CaptureFeaturePipelineMetrics getMetrics() const;

    std::uint32_t modelSampleRate() const { return sample_rate_; }
    std::uint32_t melBins() const { return mel_bins_; }
    std::uint32_t timeSteps() const { return time_steps_; }

private:
    void produceAvailableTensors(const CaptureAudioFrame& frame);
    CaptureFeatureTensor buildTensor(std::size_t window_offset, std::uint64_t frame_timestamp_ms) const;
    std::vector<float> computePowerSpectrum(const std::vector<float>& windowed) const;
    std::vector<float> computeMelEnergies(const std::vector<float>& power_spectrum) const;
    std::vector<std::vector<float>> buildMelFilterBank() const;

    const std::uint32_t sample_rate_ = 16000;
    const std::size_t model_window_samples_ = 15360;  // 960 ms
    const std::size_t model_hop_samples_ = 1600;      // 100 ms
    const std::size_t stft_window_samples_ = 400;     // 25 ms
    const std::size_t stft_hop_samples_ = 160;        // 10 ms
    const std::size_t fft_size_ = 512;
    const std::uint32_t mel_bins_ = 64;
    const std::uint32_t time_steps_ = 94;
    const std::size_t max_buffer_samples_ = 32000;

    std::vector<float> rolling_mono_;
    std::uint64_t total_samples_seen_ = 0;
    std::uint64_t next_window_start_sample_ = 0;
    std::vector<float> analysis_window_;
    std::vector<std::vector<float>> mel_filter_bank_;

    CaptureRingBuffer<CaptureFeatureTensor> ready_tensors_{64};

    mutable std::mutex mutex_;
    CaptureFeaturePipelineMetrics metrics_;
};

}  // namespace audio_assist::capture
