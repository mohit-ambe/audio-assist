# Audio Assist Prototype

This repository contains a Windows-first prototype for OS audio capture using WASAPI loopback. The current MVP focuses on endpoint loopback capture, fixed-size framed PCM output, and capture telemetry that can feed a later DSP or ML stage.

## Implemented

- Active render device enumeration with default-device labeling
- Manual device selection by endpoint id
- Shared-mode WASAPI loopback capture
- Conversion to normalized float PCM
- Continuous resampling to 16 kHz
- Fixed 20 ms frame emission for mono and stereo paths
- SPSC lock-free ring buffers for downstream consumers
- Start/stop lifecycle without process restart
- Basic stream telemetry: sample rates, dropped frames, latency estimate, active device name
- Default-device hot-swap handling and retry behavior after device invalidation

## Files

- `src/audio/AudioCaptureService.h` and `src/audio/AudioCaptureService.cpp`: capture service implementation
- `src/audio/AudioFrame.h`: shared frame, device, and metrics data structures
- `src/audio/LockFreeRingBuffer.h`: lock-free single-producer/single-consumer buffer
- `src/main.cpp`: simple console harness for device listing and live capture

## Build

The project includes a minimal `CMakeLists.txt` for a Windows MSVC-style build, but `cmake` was not available on the current machine during implementation verification.

Typical usage once the toolchain is installed:

```powershell
cmake -S . -B build
cmake --build build --config Release
.\build\Release\audio_assist.exe --list
.\build\Release\audio_assist.exe --seconds 30
.\build\Release\audio_assist.exe --seconds 30 --boost 1.5
```

## Current limitation

`CaptureMode::ApplicationLoopback` is defined in the interface for future work, but this prototype only implements `EndpointLoopback`. That keeps the MVP on the most robust Windows-supported path while leaving room to add process-targeted loopback later.
