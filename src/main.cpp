#include "audio/AudioCaptureService.h"
#include "audio/FeaturePipeline.h"
#include "audio/FeatureTensor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using audio_assist::AudioCaptureService;
using audio_assist::CaptureMode;
using audio_assist::FeaturePipeline;

namespace {

double computePeakLevel(const audio_assist::AudioFrame& frame) {
    double peak = 0.0;
    for (float sample : frame.data) {
        peak = std::max(peak, static_cast<double>(std::fabs(sample)));
    }
    return peak;
}

void applyBoost(audio_assist::AudioFrame& frame, float boost) {
    if (boost == 1.0f) {
        return;
    }

    for (float& sample : frame.data) {
        sample = std::clamp(sample * boost, -1.0f, 1.0f);
    }
}

std::wstring makeMeter(double level, std::size_t width) {
    const auto clamped = std::clamp(level, 0.0, 1.0);
    const auto filled = static_cast<std::size_t>(std::llround(clamped * static_cast<double>(width)));

    std::wstring meter;
    meter.reserve(width);
    for (std::size_t i = 0; i < width; ++i) {
        meter.push_back(i < filled ? L'#' : L'.');
    }
    return meter;
}

std::wstring shortenText(const std::wstring& text, std::size_t max_length) {
    if (text.size() <= max_length) {
        return text;
    }

    if (max_length <= 3) {
        return text.substr(0, max_length);
    }

    return text.substr(0, max_length - 3) + L"...";
}

double computeTensorActivity(const audio_assist::FeatureTensor& tensor) {
    double peak = 0.0;
    for (float value : tensor.data) {
        peak = std::max(peak, static_cast<double>(std::fabs(value)));
    }
    return std::min(peak / 12.0, 1.0);
}

void printUsage() {
    std::wcout << L"audio_assist options:\n"
               << L"  --list                 List active render devices\n"
               << L"  --device <id>          Capture from a specific device id\n"
               << L"  --seconds <n>          Run capture for n seconds (default 10)\n"
               << L"  --boost <value>        Multiply sample amplitude before meters/features (default 1.0)\n";
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    std::locale::global(std::locale(""));

    bool list_only = false;
    std::wstring device_id;
    int duration_seconds = 10;
    float boost = 1.0f;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--list") {
            list_only = true;
        } else if (arg == L"--device" && i + 1 < argc) {
            device_id = argv[++i];
        } else if (arg == L"--seconds" && i + 1 < argc) {
            duration_seconds = std::max(1, _wtoi(argv[++i]));
        } else if (arg == L"--boost" && i + 1 < argc) {
            boost = std::max(0.0f, static_cast<float>(_wtof(argv[++i])));
        } else {
            printUsage();
            return 1;
        }
    }

    const auto devices = AudioCaptureService::listOutputDevices();
    if (devices.empty()) {
        std::wcerr << L"No active render devices found." << std::endl;
    }

    if (list_only) {
        for (const auto& device : devices) {
            std::wcout << (device.is_default ? L"* " : L"  ")
                       << device.name << L"\n    " << device.id << L"\n";
        }
        return 0;
    }

    AudioCaptureService service;
    if (!service.initialize(device_id, CaptureMode::EndpointLoopback)) {
        std::wcerr << L"Failed to initialize audio capture service." << std::endl;
        return 1;
    }

    if (!service.start()) {
        std::wcerr << L"Failed to start audio capture service." << std::endl;
        return 1;
    }

    FeaturePipeline feature_pipeline;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_seconds);
    std::uint64_t mono_frames = 0;
    std::uint64_t stereo_frames = 0;
    std::uint64_t tensors_seen = 0;
    double recent_mono_peak = 0.0;
    double recent_left_peak = 0.0;
    double recent_right_peak = 0.0;
    double recent_tensor_activity = 0.0;
    std::uint64_t recent_tensor_start_ms = 0;
    std::uint64_t recent_tensor_end_ms = 0;
    auto last_mono_update = std::chrono::steady_clock::now();
    auto last_stereo_update = std::chrono::steady_clock::now();
    auto last_tensor_update = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() < deadline) {
        while (auto mono_frame = service.getMonoFrame()) {
            applyBoost(*mono_frame, boost);
            ++mono_frames;
            recent_mono_peak = computePeakLevel(*mono_frame);
            feature_pipeline.pushMonoFrame(*mono_frame);
            last_mono_update = std::chrono::steady_clock::now();
        }

        while (auto stereo_frame = service.getStereoFrame()) {
            applyBoost(*stereo_frame, boost);
            ++stereo_frames;

            double left_peak = 0.0;
            double right_peak = 0.0;
            for (std::size_t i = 0; i + 1 < stereo_frame->data.size(); i += 2) {
                left_peak = std::max(left_peak, static_cast<double>(std::fabs(stereo_frame->data[i])));
                right_peak = std::max(right_peak, static_cast<double>(std::fabs(stereo_frame->data[i + 1])));
            }

            recent_left_peak = left_peak;
            recent_right_peak = right_peak;
            last_stereo_update = std::chrono::steady_clock::now();
        }

        while (const auto tensor = feature_pipeline.getNextTensor()) {
            ++tensors_seen;
            recent_tensor_activity = computeTensorActivity(*tensor);
            recent_tensor_start_ms = tensor->window_start_ms;
            recent_tensor_end_ms = tensor->window_end_ms;
            last_tensor_update = std::chrono::steady_clock::now();
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_mono_update > std::chrono::milliseconds(250)) {
            recent_mono_peak = 0.0;
        }
        if (now - last_stereo_update > std::chrono::milliseconds(250)) {
            recent_left_peak = 0.0;
            recent_right_peak = 0.0;
        }
        if (now - last_tensor_update > std::chrono::milliseconds(350)) {
            recent_tensor_activity = 0.0;
        }

        const auto metrics = service.getMetrics();
        const auto feature_metrics = feature_pipeline.getMetrics();
        std::wostringstream line;
        line << L"\rdev=" << shortenText(metrics.active_device_name, 18)
             << L" in=" << metrics.input_sample_rate
             << L" out=" << metrics.output_sample_rate
             << L" pushed=" << metrics.pushed_frames
             << L" drop=" << metrics.dropped_frames
             << L" boost=" << std::fixed << std::setprecision(1) << boost
             << L" lat=" << metrics.capture_latency_ms << L"ms"
             << L" M=" << std::setprecision(3) << recent_mono_peak
             << L" L=" << std::setprecision(3) << recent_left_peak
             << L" R=" << std::setprecision(3) << recent_right_peak
             << L" T[" << makeMeter(recent_tensor_activity, 6) << L"]"
             << L" mel=" << feature_pipeline.melBins() << L"x" << feature_pipeline.timeSteps()
             << L" ready=" << feature_metrics.tensors_produced;

        std::wstring output = line.str();
        static std::size_t previous_width = 0;
        if (output.size() < previous_width) {
            output.append(previous_width - output.size(), L' ');
        }
        previous_width = output.size();

        std::wcout << output << std::flush;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    service.stop();

    std::wcout << L"\nmono frames consumed=" << mono_frames
               << L", stereo frames consumed=" << stereo_frames
               << L", feature tensors consumed=" << tensors_seen << std::endl;

    return 0;
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "inference/InferenceService.h"

namespace {

audio_assist::inference::InferenceRequest makeDemoRequest() {
    audio_assist::inference::InferenceRequest request;
    request.window_start_ms = 1250;
    request.window_duration_ms = 1000;

    audio_assist::inference::FeatureTensor tensor;
    tensor.name = "input";
    tensor.shape = {1, 1, 4, 4};
    tensor.values = {
        0.10F, 0.20F, 0.30F, 0.40F,
        0.15F, 0.25F, 0.35F, 0.45F,
        0.50F, 0.40F, 0.30F, 0.20F,
        0.55F, 0.45F, 0.35F, 0.25F
    };

    request.tensors.push_back(std::move(tensor));
    request.spatial_audio.left_rms = 0.42F;
    request.spatial_audio.right_rms = 0.21F;
    request.spatial_audio.mono_rms = 0.31F;
    request.spatial_audio.peak = 0.73F;
    request.spatial_audio.interaural_level_difference_db = 5.8F;
    request.spatial_audio.interaural_time_difference_ms = -0.19F;
    request.spatial_audio.stereo_correlation = 0.34F;
    request.spatial_audio.estimated_azimuth_degrees = -32.0F;
    request.spatial_audio.azimuth_confidence = 0.87F;
    request.spatial_audio.front_hemisphere_levels = {
        {-90.0F, 0.08F},
        {-60.0F, 0.18F},
        {-30.0F, 0.72F},
        {0.0F, 0.41F},
        {30.0F, 0.15F},
        {60.0F, 0.06F},
        {90.0F, 0.03F}
    };
    return request;
}

void printResult(const audio_assist::inference::InferenceResult& result) {
    std::cout << "window_start_ms=" << result.window_start_ms
              << " duration_ms=" << result.window_duration_ms << '\n';
    std::cout << "spatial left_rms=" << result.spatial_audio.left_rms
              << " right_rms=" << result.spatial_audio.right_rms
              << " mono_rms=" << result.spatial_audio.mono_rms
              << " peak=" << result.spatial_audio.peak
              << " ild_db=" << result.spatial_audio.interaural_level_difference_db
              << " itd_ms=" << result.spatial_audio.interaural_time_difference_ms
              << " stereo_corr=" << result.spatial_audio.stereo_correlation
              << " azimuth_deg=" << result.spatial_audio.estimated_azimuth_degrees
              << " azimuth_confidence=" << result.spatial_audio.azimuth_confidence
              << '\n';
    std::cout << "front_hemisphere_levels";
    for (const auto& sector : result.spatial_audio.front_hemisphere_levels) {
        std::cout << " [" << sector.angle_degrees << ':' << sector.energy << ']';
    }
    std::cout << '\n';

    for (const auto& prediction : result.predictions) {
        std::cout << "class_index=" << prediction.class_index
                  << " label=" << prediction.label
                  << " confidence=" << std::fixed << std::setprecision(4) << prediction.confidence
                  << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: audio_assist_inference <model.onnx>\n";
        return 1;
    }

    audio_assist::inference::InferenceConfig config;
    config.class_labels = {
        "gunshots",
        "footsteps",
        "other"
    };
    config.top_k = config.class_labels.size();

    try {
        audio_assist::inference::InferenceService inference_service(config);
        inference_service.loadModel(argv[1]);
        printResult(inference_service.runInference(makeDemoRequest()));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "inference failed: " << error.what() << '\n';
        return 2;
    }
}