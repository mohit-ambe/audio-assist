# Audio Assist Inference

Native ONNX inference scaffold for the `inference` branch.

## Scope

This branch owns only inference-specific code. Audio capture and feature extraction should live in the capture/feature branch and feed named float tensors into `audio_assist::inference::InferenceService`.

Core types:

- `FeatureTensor`: named input tensor with shape and payload
- `InferenceRequest`: one rolling audio window, one or more tensors, plus optional spatial audio metadata
- `InferenceResult`: per-class predictions plus the spatial payload needed by a visualization endpoint
- `InferenceService`: validates input, runs ONNX Runtime, applies labels, and returns ranked predictions

## ONNX Contract

The current runtime assumes:

- your feature pipeline passes one or more input tensors by exact ONNX input name
- the first ONNX output is a float tensor containing class scores or probabilities
- output order matches `InferenceConfig.class_labels`
- the default demo is configured for exactly 3 classes: `gunshots`, `footsteps`, `other`

## Build

```powershell
cmake -S . -B build `
  -DONNXRUNTIME_INCLUDE_DIR=C:\path\to\onnxruntime\include `
  -DONNXRUNTIME_LIBRARY=C:\path\to\onnxruntime\lib\onnxruntime.lib
cmake --build build --config Release --target audio_assist_inference
```

## Demo

```powershell
.\build\Release\audio_assist_inference.exe C:\path\to\model.onnx
```

## Integration

1. Build one `InferenceRequest` per rolling feature window.
2. Fill `tensors` with the ONNX input names, shapes, and float values.
3. Fill `spatial_audio` with directional descriptors for the overlay.
4. Call `loadModel(model_path)` once at startup.
5. Call `runInference(request)` for each window.
6. Forward `InferenceResult.predictions` and `InferenceResult.spatial_audio` into visualization.

Recommended spatial fields for a front 180-degree view:

- `left_rms`, `right_rms`, `mono_rms`, `peak`
- `interaural_level_difference_db`
- `interaural_time_difference_ms`
- `stereo_correlation`
- `estimated_azimuth_degrees`
- `azimuth_confidence`
- `front_hemisphere_levels`: sectorized energy values across `[-90, 90]`