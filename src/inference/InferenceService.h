#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audio_assist::inference {

struct FeatureTensor {
    std::string name;
    std::vector<std::int64_t> shape;
    std::vector<float> values;
};

struct DirectionalSectorLevel {
    float angle_degrees = 0.0F;
    float energy = 0.0F;
};

struct SpatialAudioMetadata {
    float left_rms = 0.0F;
    float right_rms = 0.0F;
    float mono_rms = 0.0F;
    float peak = 0.0F;
    float interaural_level_difference_db = 0.0F;
    float interaural_time_difference_ms = 0.0F;
    float stereo_correlation = 0.0F;
    float estimated_azimuth_degrees = 0.0F;
    float azimuth_confidence = 0.0F;
    std::vector<DirectionalSectorLevel> front_hemisphere_levels;
};

struct InferenceRequest {
    std::uint64_t window_start_ms = 0;
    std::uint32_t window_duration_ms = 0;
    std::vector<FeatureTensor> tensors;
    SpatialAudioMetadata spatial_audio;
};

struct ClassPrediction {
    std::string label;
    float confidence = 0.0F;
    std::size_t class_index = 0;
};

struct InferenceResult {
    std::uint64_t window_start_ms = 0;
    std::uint32_t window_duration_ms = 0;
    std::vector<ClassPrediction> predictions;
    SpatialAudioMetadata spatial_audio;
};

struct InferenceConfig {
    std::vector<std::string> class_labels;
    std::size_t top_k = 5;
    bool sort_descending = true;
};

class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;

    virtual void loadModel(const std::string& model_path) = 0;
    [[nodiscard]] virtual InferenceResult run(const InferenceRequest& request) = 0;
};

class InferenceService {
public:
    explicit InferenceService(
        InferenceConfig config = {},
        std::unique_ptr<InferenceBackend> backend = nullptr);

    void loadModel(const std::string& model_path);
    [[nodiscard]] InferenceResult runInference(const InferenceRequest& request) const;
    [[nodiscard]] const InferenceConfig& config() const noexcept;

private:
    InferenceConfig config_;
    std::unique_ptr<InferenceBackend> backend_;

    [[nodiscard]] InferenceResult normalizeResult(const InferenceResult& raw_result) const;
};

}  // namespace audio_assist::inference
