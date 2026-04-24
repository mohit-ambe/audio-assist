#include "audio/AudioCaptureService.h"
#include "audio/FeaturePipeline.h"
#include "inference/InferenceService.h"
#include "overlay/OverlayService.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> g_running{true};

struct AppConfig {
    bool list_only = false;
    bool overlay_enabled = true;
    std::wstring device_id;
    std::wstring model_path;
    std::string input_name = "input";
    int duration_seconds = 0;
    float boost = 1.0F;
    bool peak_normalization_enabled = true;
    float target_peak = 0.35F;
    float peak_normalization_floor = 0.02F;
    float max_peak_gain = 8.0F;
    audio_assist::overlay::OverlayConfig overlay;
};

BOOL WINAPI onConsoleControl(DWORD control_type) {
    if (control_type == CTRL_C_EVENT || control_type == CTRL_CLOSE_EVENT || control_type == CTRL_BREAK_EVENT) {
        g_running.store(false);
        return TRUE;
    }
    return FALSE;
}

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        std::string fallback;
        fallback.reserve(text.size());
        for (wchar_t ch : text) {
            fallback.push_back(ch >= 0 && ch <= 0x7f ? static_cast<char>(ch) : '?');
        }
        return fallback;
    }

    std::string out(static_cast<std::size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), required, nullptr, nullptr);
    return out;
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

void printUsage() {
    std::wcout << L"audio_assist options:\n"
               << L"  --list                    List active render devices\n"
               << L"  --device <id>             Capture from a specific device id\n"
               << L"  --seconds <n>             Run for n seconds; 0 means until Ctrl+C (default 0)\n"
               << L"  --boost <value>           Multiply sample amplitude before analysis (default 1.0)\n"
               << L"  --no-peak-normalize       Disable peak normalization\n"
               << L"  --target-peak <0..1>      Normalize active frames to this peak (default 0.35)\n"
               << L"  --peak-floor <0..1>       Do not normalize below this source peak (default 0.02)\n"
               << L"  --max-peak-gain <value>   Maximum gain used for quiet sounds (default 8.0)\n"
               << L"  --model <path>            Optional ONNX model path for classifier inference\n"
               << L"  --input-name <name>       ONNX input tensor name (default input)\n"
               << L"  --no-overlay              Disable the topmost visual overlay\n"
               << L"  --overlay-size <px>       Overlay size in pixels (default 280)\n"
               << L"  --opacity <0..1>          Overlay opacity (default 0.78)\n"
               << L"  --min-confidence <0..1>   Overlay minimum confidence hint (default 0.25)\n"
               << L"  --silence-peak <0..1>     Hide overlay below this peak level (default 0.008)\n";
}

float rms(const std::vector<float>& values, std::size_t offset, std::size_t stride) {
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t i = offset; i < values.size(); i += stride) {
        sum += static_cast<double>(values[i]) * values[i];
        ++count;
    }
    return count == 0 ? 0.0F : static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
}

float peakLevel(const std::vector<float>& values) {
    float peak = 0.0F;
    for (float value : values) {
        peak = std::max(peak, std::abs(value));
    }
    return peak;
}

float stereoCorrelation(const audio_assist::AudioFrame& frame, float left_rms, float right_rms) {
    if (frame.channels != 2 || frame.data.size() < 2 || left_rms <= 0.0001F || right_rms <= 0.0001F) {
        return 0.0F;
    }

    double sum = 0.0;
    std::size_t pairs = 0;
    for (std::size_t i = 0; i + 1 < frame.data.size(); i += 2) {
        sum += static_cast<double>(frame.data[i]) * frame.data[i + 1];
        ++pairs;
    }

    return static_cast<float>(std::clamp(sum / (static_cast<double>(pairs) * left_rms * right_rms), -1.0, 1.0));
}

float estimateItdMs(const audio_assist::AudioFrame& frame) {
    if (frame.channels != 2 || frame.data.size() < 42 || frame.sample_rate == 0) {
        return 0.0F;
    }

    int best_lag = 0;
    double best_score = -1.0;
    constexpr int kMaxLag = 10;
    const int frame_count = static_cast<int>(frame.data.size() / 2);
    for (int lag = -kMaxLag; lag <= kMaxLag; ++lag) {
        double score = 0.0;
        for (int i = kMaxLag; i < frame_count - kMaxLag; ++i) {
            const int right_index = i + lag;
            score += static_cast<double>(frame.data[static_cast<std::size_t>(i) * 2]) *
                     frame.data[static_cast<std::size_t>(right_index) * 2 + 1];
        }
        if (score > best_score) {
            best_score = score;
            best_lag = lag;
        }
    }

    return static_cast<float>(best_lag) * 1000.0F / static_cast<float>(frame.sample_rate);
}

audio_assist::inference::SpatialAudioMetadata analyzeSpatialAudio(const audio_assist::AudioFrame& frame) {
    audio_assist::inference::SpatialAudioMetadata spatial;
    if (frame.channels != 2 || frame.data.empty()) {
        return spatial;
    }

    spatial.left_rms = rms(frame.data, 0, 2);
    spatial.right_rms = rms(frame.data, 1, 2);
    spatial.mono_rms = 0.5F * (spatial.left_rms + spatial.right_rms);
    spatial.peak = peakLevel(frame.data);
    spatial.interaural_level_difference_db =
        20.0F * std::log10((spatial.right_rms + 0.0001F) / (spatial.left_rms + 0.0001F));
    spatial.interaural_time_difference_ms = estimateItdMs(frame);
    spatial.stereo_correlation = stereoCorrelation(frame, spatial.left_rms, spatial.right_rms);

    const float balance = (spatial.right_rms - spatial.left_rms) / (spatial.right_rms + spatial.left_rms + 0.0001F);
    const float itd_balance = std::clamp(spatial.interaural_time_difference_ms / 0.7F, -1.0F, 1.0F);
    const float directional_balance = std::clamp((balance * 0.82F) + (itd_balance * 0.18F), -1.0F, 1.0F);
    spatial.estimated_azimuth_degrees = std::clamp(directional_balance * 105.0F, -90.0F, 90.0F);
    spatial.azimuth_confidence = std::clamp(std::abs(directional_balance) * 0.9F + spatial.peak * 0.25F, 0.0F, 1.0F);

    const std::vector<float> sectors{-90.0F, -60.0F, -30.0F, 0.0F, 30.0F, 60.0F, 90.0F};
    for (float angle : sectors) {
        const float distance = std::abs(angle - spatial.estimated_azimuth_degrees) / 70.0F;
        const float directional_lobe = std::pow(std::max(0.0F, 1.0F - distance), 2.2F);
        audio_assist::inference::DirectionalSectorLevel sector;
        sector.angle_degrees = angle;
        sector.energy = std::clamp(directional_lobe * spatial.peak, 0.0F, 1.0F);
        spatial.front_hemisphere_levels.push_back(sector);
    }

    return spatial;
}

void applyBoost(audio_assist::AudioFrame& frame, float boost) {
    if (boost == 1.0F) {
        return;
    }
    for (float& sample : frame.data) {
        sample = std::clamp(sample * boost, -1.0F, 1.0F);
    }
}

void applyPeakNormalization(audio_assist::AudioFrame& frame, const AppConfig& config) {
    if (!config.peak_normalization_enabled || frame.data.empty()) {
        return;
    }

    const float source_peak = peakLevel(frame.data);
    if (source_peak <= 0.0F || source_peak < config.peak_normalization_floor) {
        return;
    }

    const float unclamped_gain = config.target_peak / source_peak;
    const float gain = std::clamp(unclamped_gain, 0.0F, config.max_peak_gain);
    for (float& sample : frame.data) {
        sample = std::clamp(sample * gain, -1.0F, 1.0F);
    }
}

audio_assist::inference::InferenceRequest makeRequest(
    const audio_assist::FeatureTensor& tensor,
    const audio_assist::inference::SpatialAudioMetadata& spatial,
    const std::string& input_name) {
    audio_assist::inference::InferenceRequest request;
    request.window_start_ms = tensor.window_start_ms;
    request.window_duration_ms = static_cast<std::uint32_t>(tensor.window_end_ms - tensor.window_start_ms);
    request.spatial_audio = spatial;

    audio_assist::inference::FeatureTensor input;
    input.name = input_name;
    input.shape = {1, 1, static_cast<std::int64_t>(tensor.mel_bins), static_cast<std::int64_t>(tensor.time_steps)};
    input.values = tensor.data;
    request.tensors.push_back(std::move(input));
    return request;
}

audio_assist::inference::InferenceResult makeHeuristicResult(const audio_assist::inference::InferenceRequest& request) {
    audio_assist::inference::InferenceResult result;
    result.window_start_ms = request.window_start_ms;
    result.window_duration_ms = request.window_duration_ms;
    result.spatial_audio = request.spatial_audio;

    const float peak = request.spatial_audio.peak;
    const float mono = request.spatial_audio.mono_rms;
    const float gunshot = std::clamp((peak - 0.55F) * 1.7F + mono * 0.6F, 0.0F, 1.0F);
    const float footsteps = std::clamp((peak - 0.06F) * 0.9F + request.spatial_audio.azimuth_confidence * 0.35F, 0.0F, 0.95F);
    const float other = std::clamp(1.0F - std::max(gunshot, footsteps) * 0.8F, 0.05F, 1.0F);

    result.predictions = {
        {"gunshots", gunshot, 0},
        {"footsteps", footsteps, 1},
        {"other", other, 2},
    };
    std::sort(result.predictions.begin(), result.predictions.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.confidence > rhs.confidence; });
    return result;
}

std::wstring predictionText(const audio_assist::inference::InferenceResult& result) {
    if (result.predictions.empty()) {
        return L"none";
    }

    const auto& prediction = result.predictions.front();
    std::wostringstream out;
    out << std::wstring(prediction.label.begin(), prediction.label.end())
        << L":" << std::fixed << std::setprecision(2) << prediction.confidence;
    return out.str();
}

bool parseArgs(int argc, wchar_t* argv[], AppConfig& config) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--list") {
            config.list_only = true;
        } else if (arg == L"--device" && i + 1 < argc) {
            config.device_id = argv[++i];
        } else if (arg == L"--seconds" && i + 1 < argc) {
            config.duration_seconds = std::max(0, _wtoi(argv[++i]));
        } else if (arg == L"--boost" && i + 1 < argc) {
            config.boost = std::max(0.0F, static_cast<float>(_wtof(argv[++i])));
        } else if (arg == L"--no-peak-normalize") {
            config.peak_normalization_enabled = false;
        } else if (arg == L"--target-peak" && i + 1 < argc) {
            config.target_peak = std::clamp(static_cast<float>(_wtof(argv[++i])), 0.01F, 1.0F);
        } else if (arg == L"--peak-floor" && i + 1 < argc) {
            config.peak_normalization_floor = std::clamp(static_cast<float>(_wtof(argv[++i])), 0.0F, 1.0F);
        } else if (arg == L"--max-peak-gain" && i + 1 < argc) {
            config.max_peak_gain = std::max(1.0F, static_cast<float>(_wtof(argv[++i])));
        } else if (arg == L"--model" && i + 1 < argc) {
            config.model_path = argv[++i];
        } else if (arg == L"--input-name" && i + 1 < argc) {
            config.input_name = toUtf8(argv[++i]);
        } else if (arg == L"--no-overlay") {
            config.overlay_enabled = false;
        } else if (arg == L"--overlay-size" && i + 1 < argc) {
            config.overlay.size_px = std::max(160, _wtoi(argv[++i]));
        } else if (arg == L"--opacity" && i + 1 < argc) {
            config.overlay.opacity = std::clamp(static_cast<float>(_wtof(argv[++i])), 0.15F, 1.0F);
        } else if (arg == L"--min-confidence" && i + 1 < argc) {
            config.overlay.min_confidence = std::clamp(static_cast<float>(_wtof(argv[++i])), 0.0F, 1.0F);
        } else if (arg == L"--silence-peak" && i + 1 < argc) {
            config.overlay.silence_peak_threshold = std::clamp(static_cast<float>(_wtof(argv[++i])), 0.0F, 1.0F);
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    std::locale::global(std::locale(""));
    SetConsoleCtrlHandler(onConsoleControl, TRUE);

    AppConfig config;
    if (!parseArgs(argc, argv, config)) {
        printUsage();
        return 1;
    }

    const auto devices = audio_assist::AudioCaptureService::listOutputDevices();
    if (config.list_only) {
        for (const auto& device : devices) {
            std::wcout << (device.is_default ? L"* " : L"  ")
                       << device.name << L"\n    " << device.id << L"\n";
        }
        return 0;
    }

    audio_assist::AudioCaptureService capture;
    if (!capture.initialize(config.device_id, audio_assist::CaptureMode::EndpointLoopback) || !capture.start()) {
        std::wcerr << L"Failed to start audio capture." << std::endl;
        return 1;
    }

    audio_assist::FeaturePipeline feature_pipeline;
    std::unique_ptr<audio_assist::inference::InferenceService> inference;
    if (!config.model_path.empty()) {
        audio_assist::inference::InferenceConfig inference_config;
        inference_config.class_labels = {"gunshots", "footsteps", "other"};
        inference_config.top_k = inference_config.class_labels.size();
        inference = std::make_unique<audio_assist::inference::InferenceService>(std::move(inference_config));
        try {
            inference->loadModel(toUtf8(config.model_path));
        } catch (const std::exception& error) {
            std::cerr << "failed to load model: " << error.what() << '\n';
            capture.stop();
            return 2;
        }
    }

    std::unique_ptr<audio_assist::overlay::OverlayService> overlay;
    if (config.overlay_enabled) {
        overlay = std::make_unique<audio_assist::overlay::OverlayService>(config.overlay);
        overlay->start();
    }

    auto latest_spatial = audio_assist::inference::SpatialAudioMetadata{};
    auto latest_result = audio_assist::inference::InferenceResult{};
    std::uint64_t mono_frames = 0;
    std::uint64_t stereo_frames = 0;
    std::uint64_t tensors_seen = 0;
    const auto deadline = config.duration_seconds > 0
        ? std::optional<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now() + std::chrono::seconds(config.duration_seconds))
        : std::nullopt;

    while (g_running.load() && (!deadline || std::chrono::steady_clock::now() < *deadline)) {
        while (auto mono_frame = capture.getMonoFrame()) {
            applyBoost(*mono_frame, config.boost);
            applyPeakNormalization(*mono_frame, config);
            feature_pipeline.pushMonoFrame(*mono_frame);
            ++mono_frames;
        }

        while (auto stereo_frame = capture.getStereoFrame()) {
            applyBoost(*stereo_frame, config.boost);
            applyPeakNormalization(*stereo_frame, config);
            latest_spatial = analyzeSpatialAudio(*stereo_frame);
            if (overlay && latest_spatial.peak < config.overlay.silence_peak_threshold) {
                latest_result.spatial_audio = latest_spatial;
                overlay->update(latest_result);
            }
            ++stereo_frames;
        }

        while (const auto tensor = feature_pipeline.getNextTensor()) {
            ++tensors_seen;
            const auto request = makeRequest(*tensor, latest_spatial, config.input_name);
            try {
                latest_result = inference ? inference->runInference(request) : makeHeuristicResult(request);
                if (overlay) {
                    overlay->update(latest_result);
                }
            } catch (const std::exception& error) {
                std::cerr << "\ninference failed: " << error.what() << '\n';
            }
        }

        const auto metrics = capture.getMetrics();
        const auto feature_metrics = feature_pipeline.getMetrics();
        std::wostringstream line;
        line << L"\rdev=" << shortenText(metrics.active_device_name, 20)
             << L" in=" << metrics.input_sample_rate
             << L" out=" << metrics.output_sample_rate
             << L" frames=" << metrics.pushed_frames
             << L" drop=" << metrics.dropped_frames
             << L" boost=" << std::fixed << std::setprecision(1) << config.boost
             << L" peak=" << std::setprecision(3) << latest_spatial.peak
             << L" az=" << std::setprecision(1) << latest_spatial.estimated_azimuth_degrees
             << L" conf=" << std::setprecision(2) << latest_spatial.azimuth_confidence
             << L" tensors=" << feature_metrics.tensors_produced
             << L" pred=" << predictionText(latest_result);

        static std::size_t previous_width = 0;
        std::wstring output = line.str();
        if (output.size() < previous_width) {
            output.append(previous_width - output.size(), L' ');
        }
        previous_width = output.size();
        std::wcout << output << std::flush;

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    if (overlay) {
        overlay->stop();
    }
    capture.stop();

    std::wcout << L"\nmono frames=" << mono_frames
               << L", stereo frames=" << stereo_frames
               << L", tensors=" << tensors_seen << std::endl;
    return 0;
}
