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
