#pragma once

#include "audio/CaptureAudioFrame.h"
#include "audio/CaptureRingBuffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace audio_assist::capture {

class WindowsAudioCaptureService {
public:
    WindowsAudioCaptureService();
    ~WindowsAudioCaptureService();

    WindowsAudioCaptureService(const WindowsAudioCaptureService&) = delete;
    WindowsAudioCaptureService& operator=(const WindowsAudioCaptureService&) = delete;

    bool initialize(const std::wstring& device_id, AudioCaptureMode mode);
    bool start();
    void stop();

    std::optional<CaptureAudioFrame> getMonoFrame();
    std::optional<CaptureAudioFrame> getStereoFrame();
    AudioCaptureMetrics getMetrics() const;

    static std::vector<CaptureDeviceInfo> listOutputDevices();

private:
    struct StreamState;

    bool openStream(StreamState& state);
    void closeStream(StreamState& state);
    void captureLoop();
    void processChunk(const float* interleaved, std::uint32_t frames, std::uint16_t channels);
    void produceFrames();
    std::wstring resolveDeviceId() const;

    std::wstring configured_device_id_;
    AudioCaptureMode mode_ = AudioCaptureMode::EndpointLoopback;

    const std::uint32_t target_sample_rate_ = 16000;
    const std::uint32_t frame_duration_ms_ = 20;
    const std::size_t frame_samples_ = (target_sample_rate_ * frame_duration_ms_) / 1000;

    CaptureRingBuffer<CaptureAudioFrame> mono_frames_{256};
    CaptureRingBuffer<CaptureAudioFrame> stereo_frames_{256};

    mutable std::mutex metrics_mutex_;
    AudioCaptureMetrics metrics_;

    std::vector<float> resample_source_;
    std::vector<float> pending_output_;
    double resample_position_ = 0.0;
    mutable std::mutex resample_mutex_;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::thread capture_thread_;
};

}  // namespace audio_assist::capture
