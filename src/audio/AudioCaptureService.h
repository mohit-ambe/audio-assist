#pragma once

#include "audio/AudioFrame.h"
#include "audio/LockFreeRingBuffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace audio_assist {

class AudioCaptureService {
public:
    AudioCaptureService();
    ~AudioCaptureService();

    AudioCaptureService(const AudioCaptureService&) = delete;
    AudioCaptureService& operator=(const AudioCaptureService&) = delete;

    bool initialize(const std::wstring& device_id, CaptureMode mode);
    bool start();
    void stop();

    std::optional<AudioFrame> getMonoFrame();
    std::optional<AudioFrame> getStereoFrame();
    CaptureMetrics getMetrics() const;

    static std::vector<DeviceInfo> listOutputDevices();

private:
    struct StreamState;

    bool openStream(StreamState& state);
    void closeStream(StreamState& state);
    void captureLoop();
    void processChunk(const float* interleaved, std::uint32_t frames, std::uint16_t channels);
    void produceFrames();
    std::wstring resolveDeviceId() const;

    std::wstring configured_device_id_;
    CaptureMode mode_ = CaptureMode::EndpointLoopback;

    const std::uint32_t target_sample_rate_ = 16000;
    const std::uint32_t frame_duration_ms_ = 20;
    const std::size_t frame_samples_ = (target_sample_rate_ * frame_duration_ms_) / 1000;

    LockFreeRingBuffer<AudioFrame> mono_frames_{256};
    LockFreeRingBuffer<AudioFrame> stereo_frames_{256};

    mutable std::mutex metrics_mutex_;
    CaptureMetrics metrics_;

    std::vector<float> resample_source_;
    std::vector<float> pending_output_;
    double resample_position_ = 0.0;
    mutable std::mutex resample_mutex_;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::thread capture_thread_;
};

}  // namespace audio_assist
