# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview
Two applications built on Google Filament (Vulkan backend), Windows-only (Visual Studio 2022, C++20):

1. **FilamentCone** — 3D rendering demo with live webcam background and AirPixel camera tracking. GLB models overlaid on a live camera feed.
2. **GhostCar** — Overlays a semi-transparent 3D ghost car on VBOX HD2 video, synchronized via VBO GPS telemetry for lap comparison.

## Build Commands
```bash
cmake -B build -S .
cmake --build build --config Release
build/Release/FilamentCone.exe
build/Release/GhostCar.exe
```

Material source files (`.mat`) must be compiled to `.filamat` before use. **Must use `-a vulkan`** for the Vulkan backend:
```bash
deps/bin/matc -o materials/shadow_plane.filamat -p desktop -a vulkan materials/shadow_plane.mat
```

## Architecture

### Friend Function Pattern (Critical)
The app uses a central `FilamentApp` class (`src/FilamentApp.h`) that holds all state as private members. Each module is implemented as **free functions** that are `friend`s of `FilamentApp`, receiving a `FilamentApp*` as first parameter. `FilamentApp` exposes thin inline wrappers (e.g., `saveScene()` → `scene_save(this, f)`). This avoids splitting state across classes while keeping code in separate translation units.

Module naming convention: `module_functionName(FilamentApp* app, ...)` — e.g., `scene_save()`, `webcam_init()`, `tracking_openSerialPort()`, `recording_start()`, `distortion_setup()`, `imgui_buildPanel()`.

### Source Files — FilamentCone
| File | Purpose |
|------|---------|
| `src/main.cpp` | App entry point, main loop, input handling, Filament setup |
| `src/FilamentApp.h` | Central class with all state (no .cpp — methods are in main.cpp) |
| `src/Types.h` | Shared types: `SceneObject`, `CameraTrackingData`, protocol enums, constants |
| `src/SceneManager.cpp/h` | Scene JSON save/load, GLB model loading, file path resolution, native file dialogs |
| `src/WebcamCapture.cpp/h` | DirectShow webcam capture, background quad rendering |
| `src/TrackingSystem.cpp/h` | Serial/UDP tracking: VIPS and FreeD protocol parsing, coordinate conversion |
| `src/Recording.cpp/h` | MP4 recording via FFmpeg pipe (NVENC HEVC) |
| `src/LensDistortion.cpp/h` | Lens distortion correction from calibration JSON files |
| `src/ImGuiPanel.cpp/h` | ImGui control panel UI (separate SDL/OpenGL window) |
| `src/ImGuiFilamentBridge.cpp/h` | Renders ImGui draw data through Filament's rendering pipeline |
| `src/ConeGenerator.cpp/h` | Procedural cone mesh generation |

### Source Files — GhostCar
| File | Purpose |
|------|---------|
| `src/ghostcar_main.cpp` | Entry point (DPI awareness, timer precision) |
| `src/GhostCarApp.cpp/h` | Main application: Filament setup, two-view video compositing, ghost car loading/positioning, ImGui sidebar, playback loop |
| `src/VboParser.cpp/h` | VBO file parser, circuit database XML parser, circuit detection, lap detection, GPS-to-meters conversion |
| `materials/ghost_car.mat` | Custom material with `blending: fade` for semi-transparent ghost car |

### Known Build Gotcha
nlohmann/json's `assert_invariant()` method name-collides with Filament's macro from `utils/debug.h`. The workaround in `SceneManager.cpp` and `LensDistortion.cpp` temporarily `#undef`s the macro before including `json.hpp`, then re-includes `utils/debug.h` after. Any new file that includes both must use the same pattern.

### Dependencies (all vendored in `deps/`)
- **Filament**: 3D rendering engine (libs in `deps/lib/x86_64/mt/`)
- **SDL2**: Window management (`deps/SDL2-2.30.11/`)
- **OpenCV**: Image utilities (`deps/opencv/build/`; webcam uses DirectShow directly)
- **Dear ImGui**: Immediate-mode GUI v1.91.8 (`deps/imgui/`)
- **nlohmann/json**: Single-header JSON (`deps/nlohmann/json.hpp`)

## Scene System
All GLB models are managed as `std::vector<SceneObject>` in `FilamentApp`. Each `SceneObject` holds name, filename, position, scale, yRotation, rot90[3], aabbCenterOffset, visible flag, and a runtime `FilamentAsset*` pointer (not serialized).

Scene is data-driven via JSON (`scene.json` auto-loaded on startup next to exe). Save/Load via ImGui panel or native Windows file dialogs.

## Rendering Architecture
- **Two-view compositing**: Background view (webcam quad + ortho camera) rendered first, main view (3D scene) rendered second
- **Main view**: Post-processing enabled (tone mapping), TRANSLUCENT blend mode so background pixels (alpha=0) preserve webcam
- **Background view**: Post-processing disabled, no shadows, separate scene/camera
- **Renderer**: ClearOptions set to not clear at beginFrame (preserves background view output)
- **No skybox** on main scene — transparency lets webcam show through
- **GLB loading**: gltfio with UbershaderProvider (linked UBERARCHIVE_DEFAULT_DATA)

### Shadow Ground Plane
- **Material**: `shadingModel: unlit` + `shadowMultiplier: true` + `blending: transparent`
- `shadowMultiplier` with `shadingModel: lit` does NOT work in matc v69 — must use `unlit`
- Fixed absolute world Y=1.0m, set after anchor transform overrides

## Webcam Background
- DirectShow via SampleGrabber (Source→SampleGrabber→NullRenderer), MJPG 1920x1080
- Graph runs with no reference clock (`SetSyncSource(NULL)`) for freerunning delivery
- Render loop synced to webcam via auto-reset Windows Event (`MsgWaitForMultipleObjects`)
- Tracking data snapshot paired with each webcam frame in callback for video-locked 3D overlay

## Camera Tracking

### Protocols
- **VIPS**: AirPixel proprietary. Binary, little-endian, header `0x24 0xD9`, CRC-16. 6DOF (XYZ + RPY). Serial: 115200 baud, no parity.
- **FreeD**: Industry-standard broadcast protocol. Fixed 29-byte packets (type `0xD1`), 3-byte encoded values + checksum. Serial: 38400 baud, odd parity. UDP: port 6301.
- **Auto-detect** (default): Starts VIPS, switches to FreeD after 2 seconds if no valid VIPS data.

### Transports
- **RS-232**: Serial port (COM port dropdown + Connect button)
- **UDP**: Network listener (port input + Listen button)

### Coordinate Conversion (VIPS → Filament)
- VIPS X (east) → Filament X (right)
- VIPS Y (north) → Filament -Z (forward)
- VIPS Z (up) → Filament Y (up)
- Yaw is negated for correct left/right mapping

## MP4 Recording
- F5 toggles recording to `recording.mp4`
- Double-buffered offscreen RenderTargets → `readPixels` from previous frame (no GPU stall) → RGBA piped to FFmpeg NVENC HEVC
- Vulkan: readPixels returns top-down data (no flip needed, unlike OpenGL)
- ImGui panel is NOT included in recordings

## Controls
- **Mouse**: Left drag = orbit camera, Right drag = move light, Wheel = zoom
- **Cone**: Arrow keys (move), A/Z (up/down), X/C (scale), R (place in front), G (drop to ground)
- **First GLB object**: Shift+Arrows (move), Shift+A/Z (up/down), Shift+X/C (scale), Shift+</> (rotate), Shift+R (place in front)
- **F5**: Toggle MP4 recording
- **Delete**: Clear trail markers
- **ESC**: Exit

## GhostCar Application

### What It Does
Plays back a VBOX HD2 MP4 video with a semi-transparent 3D ghost car (funcup.glb) overlaid, showing a comparison lap's position in real time. The ghost car is positioned using GPS deltas transformed into the camera car's local coordinate frame.

### VBO File Format
- Space-delimited text from VBOX GPS logger, sections in `[brackets]`: `[header]`, `[channel units]`, `[comments]`, `[AVI]`, `[laptiming]`, `[column names]`, `[data]`
- **Longitude sign is inverted** in VBO files — must multiply by -1
- Lat/lon stored in minutes. Video sync via `avitime` column (ms into video)
- `[AVI]` section gives video base name and extension
- See `VBO file description.txt` for full format details

### Circuit Detection & Lap Extraction
- `StartFinishDataBase.xml` contains ~900 circuits with bounding boxes and start/finish line coordinates
- Circuit matched by checking if average GPS position falls within a bounding box
- Start/finish gate: two points define a perpendicular crossing line; laps detected via segment intersection with direction filtering

### Ghost Car Positioning
- GPS delta (ghost minus camera) converted to meters via `gpsToLocalMeters()`
- Transformed to camera-local coordinates using camera heading: right/forward decomposition
- Filament coordinates: X=right, Y=up, -Z=forward
- Ghost car yaw = heading difference between ghost and camera (negated for Filament)
- Ghost car pitch = `atan2(heightChange, horizontalDistance)` over a window of ±5 samples
- Camera pitch compensation (toggleable): tilts 3D camera to match camera car's road gradient so ghost car moves up/down in frame on hills

### Ghost Car Transparency
- Custom `ghost_car.mat` material with `blending: fade`, `shadingModel: lit`
- After GLB loads with gltfio/ubershader, all material instances on renderable entities are replaced with instances of this custom material
- Tint color and alpha adjustable at runtime via ImGui

### Playback
- Video frames paced to actual video FPS, scaled by adjustable speed (0.1x–5.0x)
- Sequential frame reads (no per-frame seeking) for performance
- Initial seek to camera lap's start `aviTime` on play
- Ghost car hidden when behind camera (relForward ≤ 0) or when lap ends
