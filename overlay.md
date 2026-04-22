# Audio Assist Directional Overlay

Windows-native MVP overlay for coarse directional awareness from the final stereo mix plus classifier output.

This is deliberately heuristic. The overlay does not receive engine source coordinates and does not recover true 3D positional audio. It estimates relative left/front/right awareness from stereo descriptors such as ILD, ITD, correlation, azimuth confidence, and channel energy, then renders a lightweight visual cue.

## Implemented

- Transparent, topmost Win32 overlay window
- Click-through mode by default, with `--interactive` for settings/testing
- Direct2D drawing and DirectWrite labels
- Configurable size, opacity, monitor, anchor, confidence threshold, persistence, and icon-only mode
- Per-class suppression through repeated `--disable-class <label>`
- Critical-sounds-only filter for urgent classes such as gunshots and footsteps
- Stdin ingestion for rolling classifier/spatial blocks
- Automatic cue decay and bounded active-event count to control visual spam
- Optional displayed-event logging with `--log <path>`

## Build

```powershell
cmake -S . -B build
cmake --build build --config Release --target audio_assist_overlay
```

If CMake cannot probe the local Windows SDK, the same sources compile with MSVC once the Visual Studio developer environment is active:

```powershell
cl /std:c++17 /EHsc /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I src `
  /Fe:build\audio_assist_overlay.exe `
  src\overlay\DirectionalEventParser.cpp src\overlay\OverlayService.cpp src\overlay\main.cpp `
  d2d1.lib dwrite.lib user32.lib gdi32.lib shell32.lib
```

## Run

The executable reads event blocks from stdin:

```powershell
Get-Content .\sample-events.txt | .\build\Release\audio_assist_overlay.exe --opacity 0.75 --anchor center
```

Useful options:

- `--size <px>`
- `--opacity <0..1>`
- `--anchor <center|top-left|top-right|bottom-left|bottom-right>`
- `--monitor <index>`
- `--persistence-ms <ms>`
- `--min-confidence <0..1>`
- `--icon-only`
- `--interactive`
- `--critical-only`
- `--disable-class <label>`
- `--log <path>`

## Input Format

```text
window_start_ms=1250 duration_ms=1000
spatial left_rms=0.42 right_rms=0.21 mono_rms=0.31 peak=0.73 ild_db=5.8 itd_ms=-0.19 stereo_corr=0.34 azimuth_deg=-32 azimuth_confidence=0.87
front_hemisphere_levels [-90:0.08] [-60:0.18] [-30:0.72] [0:0.41] [30:0.15] [60:0.06] [90:0.03]
class_index=1 label=footsteps confidence=0.7825
class_index=2 label=other confidence=0.6175
class_index=0 label=gunshots confidence=0.5475
```

Blank lines or a new `window_start_ms=` line flush the previous block into visual events.