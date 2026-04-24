#pragma once

#include "inference/InferenceService.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace audio_assist::overlay {

struct OverlayConfig {
    int size_px = 280;
    float opacity = 0.78F;
    float min_confidence = 0.25F;
    float silence_peak_threshold = 0.008F;
    bool click_through = true;
    bool icon_only = false;
};

class OverlayService {
public:
    explicit OverlayService(OverlayConfig config = {});
    ~OverlayService();

    OverlayService(const OverlayService&) = delete;
    OverlayService& operator=(const OverlayService&) = delete;

    bool start();
    void stop();
    void update(const inference::InferenceResult& result);
    long long windowProc(void* hwnd, unsigned int message, unsigned long long wparam, long long lparam);

private:
    struct SharedState {
        inference::InferenceResult result;
        bool has_result = false;
    };

    void windowThread();
    void paint(void* hwnd);

    OverlayConfig config_;
    std::atomic<bool> running_{false};
    std::atomic<void*> hwnd_{nullptr};
    std::thread thread_;

    std::mutex state_mutex_;
    SharedState state_;
};

}  // namespace audio_assist::overlay
