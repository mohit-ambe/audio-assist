#include "inference/InferenceService.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef AUDIO_ASSIST_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace audio_assist::inference {
namespace {

std::size_t elementCount(const std::vector<std::int64_t>& shape) {
    if (shape.empty()) {
        return 0U;
    }

    std::size_t count = 1U;
    for (const std::int64_t dim : shape) {
        if (dim <= 0) {
            throw std::invalid_argument("Tensor shapes must contain only positive dimensions");
        }
        count *= static_cast<std::size_t>(dim);
    }
    return count;
}

void validateRequest(const InferenceRequest& request) {
    if (request.tensors.empty()) {
        throw std::invalid_argument("InferenceRequest must contain at least one tensor");
    }

    for (const auto& tensor : request.tensors) {
        if (tensor.name.empty()) {
            throw std::invalid_argument("Each input tensor must have a name");
        }
        if (tensor.shape.empty()) {
            throw std::invalid_argument("Each input tensor must include a shape");
        }
        if (tensor.values.size() != elementCount(tensor.shape)) {
            throw std::invalid_argument("Tensor value count does not match its declared shape");
        }
    }

    for (const auto& sector : request.spatial_audio.front_hemisphere_levels) {
        if (sector.angle_degrees < -90.0F || sector.angle_degrees > 90.0F) {
            throw std::invalid_argument("Directional sector angles must stay within the front 180-degree field [-90, 90]");
        }
    }
}

#ifdef AUDIO_ASSIST_HAS_ONNXRUNTIME
class OnnxInferenceBackend final : public InferenceBackend {
public:
    OnnxInferenceBackend()
        : environment_(ORT_LOGGING_LEVEL_WARNING, "audio_assist_inference"),
          session_options_() {
        session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
        session_options_.SetIntraOpNumThreads(1);
    }

    void loadModel(const std::string& model_path) override {
        if (model_path.empty()) {
            throw std::invalid_argument("Model path cannot be empty");
        }

        session_ = std::make_unique<Ort::Session>(
            environment_,
            std::wstring(model_path.begin(), model_path.end()).c_str(),
            session_options_);

        Ort::AllocatorWithDefaultOptions allocator;

        output_name_buffers_.clear();
        output_names_.clear();
        const std::size_t output_count = session_->GetOutputCount();
        output_name_buffers_.reserve(output_count);
        output_names_.reserve(output_count);

        for (std::size_t i = 0; i < output_count; ++i) {
            output_name_buffers_.push_back(session_->GetOutputNameAllocated(i, allocator));
            output_names_.push_back(output_name_buffers_.back().get());
        }
    }

    [[nodiscard]] InferenceResult run(const InferenceRequest& request) override {
        if (!session_) {
            throw std::runtime_error("loadModel must be called before runInference");
        }

        validateRequest(request);

        std::vector<const char*> input_names;
        std::vector<Ort::Value> input_values;
        input_names.reserve(request.tensors.size());
        input_values.reserve(request.tensors.size());

        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
        for (const auto& tensor : request.tensors) {
            input_names.push_back(tensor.name.c_str());
            input_values.push_back(Ort::Value::CreateTensor<float>(
                memory_info,
                const_cast<float*>(tensor.values.data()),
                tensor.values.size(),
                tensor.shape.data(),
                tensor.shape.size()));
        }

        auto output_values = session_->Run(
            Ort::RunOptions{nullptr},
            input_names.data(),
            input_values.data(),
            input_values.size(),
            output_names_.data(),
            output_names_.size());

        if (output_values.empty()) {
            throw std::runtime_error("Model returned no outputs");
        }

        Ort::Value& output_tensor = output_values.front();
        if (!output_tensor.IsTensor()) {
            throw std::runtime_error("Expected the first ONNX output to be a tensor");
        }

        auto shape_info = output_tensor.GetTensorTypeAndShapeInfo();
        const std::size_t prediction_count = shape_info.GetElementCount();
        const float* scores = output_tensor.GetTensorData<float>();

        InferenceResult result;
        result.window_start_ms = request.window_start_ms;
        result.window_duration_ms = request.window_duration_ms;
        result.spatial_audio = request.spatial_audio;
        result.predictions.reserve(prediction_count);

        for (std::size_t index = 0; index < prediction_count; ++index) {
            ClassPrediction prediction;
            prediction.class_index = index;
            prediction.confidence = scores[index];
            result.predictions.push_back(std::move(prediction));
        }

        return result;
    }

private:
    Ort::Env environment_;
    Ort::SessionOptions session_options_;
    std::unique_ptr<Ort::Session> session_;
    std::vector<Ort::AllocatedStringPtr> output_name_buffers_;
    std::vector<const char*> output_names_;
};
#else
class OnnxInferenceBackend final : public InferenceBackend {
public:
    void loadModel(const std::string& model_path) override {
        if (model_path.empty()) {
            throw std::invalid_argument("Model path cannot be empty");
        }
        throw std::runtime_error("ONNX Runtime support is disabled. Configure with AUDIO_ASSIST_ENABLE_ONNXRUNTIME=ON.");
    }

    [[nodiscard]] InferenceResult run(const InferenceRequest& request) override {
        validateRequest(request);
        throw std::runtime_error("ONNX Runtime support is disabled. Configure with AUDIO_ASSIST_ENABLE_ONNXRUNTIME=ON.");
    }
};
#endif

}  // namespace

InferenceService::InferenceService(
    InferenceConfig config,
    std::unique_ptr<InferenceBackend> backend)
    : config_(std::move(config)),
      backend_(std::move(backend)) {
    if (!backend_) {
        backend_ = std::make_unique<OnnxInferenceBackend>();
    }
}

void InferenceService::loadModel(const std::string& model_path) {
    backend_->loadModel(model_path);
}

InferenceResult InferenceService::runInference(const InferenceRequest& request) const {
    validateRequest(request);
    return normalizeResult(backend_->run(request));
}

const InferenceConfig& InferenceService::config() const noexcept {
    return config_;
}

InferenceResult InferenceService::normalizeResult(const InferenceResult& raw_result) const {
    InferenceResult normalized = raw_result;

    for (auto& prediction : normalized.predictions) {
        if (prediction.label.empty() && prediction.class_index < config_.class_labels.size()) {
            prediction.label = config_.class_labels[prediction.class_index];
        }
    }

    if (config_.sort_descending) {
        std::sort(normalized.predictions.begin(), normalized.predictions.end(),
            [](const ClassPrediction& lhs, const ClassPrediction& rhs) {
                return lhs.confidence > rhs.confidence;
            });
    }

    if (config_.top_k > 0U && normalized.predictions.size() > config_.top_k) {
        normalized.predictions.resize(config_.top_k);
    }

    return normalized;
}

}  // namespace audio_assist::inference
