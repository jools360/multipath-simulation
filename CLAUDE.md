# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# Filament 3D Demo with AirPixel Camera Tracking

## Overview
A 3D rendering demo using Google Filament engine that displays a scene with camera tracking from an AirPixel positioning system. GLB models are loaded via Filament's gltfio library. Live webcam feed is rendered as the background.

## Build System
- CMake project targeting Windows with Visual Studio 2022
- C++20 standard
- Static runtime linking (MultiThreaded)

### Dependencies
- **Filament**: 3D rendering engine (in `deps/`)
- **SDL2**: Window management (in `deps/SDL2-2.30.11/`)
- **OpenCV**: Image utilities (in `deps/opencv/build/`; webcam capture uses DirectShow directly)
- **Dear ImGui**: Immediate-mode GUI (v1.91.8, in `deps/imgui/`)
- **nlohmann/json**: Single-header JSON library (in `deps/nlohmann/json.hpp`)

### Build Commands
```bash
# Configure
cmake -B build -S .

# Build Release
cmake --build build --config Release

# Run
build/Release/FilamentCone.exe
```

### Compiling Materials
Material source files (`.mat`) must be compiled to `.filamat` using Filament's `matc` tool before use:
```bash
deps/bin/matc -o materials/shadow_plane.filamat -p desktop materials/shadow_plane.mat
```

### Known Build Gotcha
nlohmann/json's `assert_invariant()` method name-collides with Filament's macro from `utils/debug.h`. The workaround in `main.cpp` (lines 52-60) temporarily `#undef`s the macro before including `json.hpp`, then re-includes `utils/debug.h` after. Any new file that includes both must use the same pattern.

## Project Structure
- `src/main.cpp` - Main application with FilamentApp class
- `src/ConeGenerator.cpp/.h` - Procedural cone mesh generation
- `materials/` - Filament material files (.filamat)
- `AirPixel_logo.glb` - 3D logo model
- `RACELOGIC.glb` - Racelogic logo model
- `Ford_Mustang_Mach-E_2021.glb` - Car model
- `scene.json` - Scene configuration (auto-loaded on startup if present next to exe)

## Scene Architecture

### SceneObject System
All GLB models are managed as a unified `std::vector<SceneObject>`. Each SceneObject contains:
- `name`, `glbFilename` — identity
- `position`, `scale`, `yRotation`, `rot90[3]`, `aabbCenterOffset` — transform parameters
- `visible` — visibility toggle
- `asset` — runtime FilamentAsset pointer (not serialized)

The scene is fully data-driven via JSON:
- On startup, if `scene.json` exists next to the exe, it is loaded (all objects, camera, lighting, visibility)
- If no `scene.json` exists, hardcoded defaults (Logo + Racelogic) are used
- Save/Load via ImGui panel or native Windows file dialogs

### JSON Scene File Schema
```json
{
  "version": 1,
  "camera": { "distance", "yaw", "pitch", "target": [x,y,z], "autoOrbit" },
  "lighting": { "sunIntensity", "sunYaw", "sunPitch", "iblIntensity", "shadowStrength" },
  "groundPlane": { "worldY", "visible" },
  "cone": { "position": [x,y,z], "scale", "rot90": [x,y,z], "visible" },
  "cube": { "visible" },
  "trailMarkers": { "visible" },
  "glbObjects": [
    { "name", "filename", "position", "scale", "yRotation", "rot90", "aabbCenterOffset", "visible" }
  ]
}
```

### Default Scene Contents
- Red cone (1m tall, inverted, scale adjustable via X/C keys)
- Blue cube (procedural, hidden by default)
- AirPixel logo (GLB, scaled 16.5x, rotated to face camera)
- Racelogic logo (GLB, at position 2,0,0)
- Shadow ground plane (invisible, shadow catcher at fixed world Y=1.0m)
- Directional sun light (150,000 lux) with PCF shadows (4096 map, 30m range)
- Sky-like ambient IBL (intensity 5,000) with HDR reflections cubemap (RGBA16F, values up to 6.0)

## Shadow Ground Plane
- **Material**: `shadingModel: unlit` + `shadowMultiplier: true` + `blending: transparent`
- **How it works**: `unlit` prevents the plane from picking up lighting/IBL. `shadowMultiplier` modulates output by shadow factor: `fragColor = baseColor * (1 - shadowVisibility)`. Lit areas output zero (invisible), shadow areas output dark overlay.
- **Note**: `shadowMultiplier` with `shadingModel: lit` does NOT work in matc v69 — the shader ignores it. Must use `unlit`.
- **Parameter**: `shadowStrength` (0.5) controls shadow darkness
- **Position**: Fixed absolute world Y=1.0m, set after anchor transform overrides

## Rendering Architecture
- **Two-view compositing**: Background view (webcam quad + ortho camera) rendered first, main view (3D scene) rendered second
- **Main view**: Post-processing enabled (tone mapping), TRANSLUCENT blend mode so background pixels (alpha=0) preserve webcam
- **Background view**: Post-processing disabled, no shadows, separate scene/camera
- **Renderer**: ClearOptions set to not clear at beginFrame (preserves background view output)
- **No skybox** on main scene - transparency lets webcam show through

## GLB Model Loading
- Uses gltfio with UbershaderProvider (linked UBERARCHIVE_DEFAULT_DATA)
- ResourceLoader with STB texture provider for PNG/JPEG
- Models added to main scene via `mScene->addEntities()`
- `loadGlbObject(SceneObject&)` loads GLB and applies transform from SceneObject fields
- `updateObjectTransform(SceneObject&)` rebuilds transform from position/scale/rotation/aabbCenterOffset

## Webcam Background
- Direct DirectShow via SampleGrabber (no OpenCV capture needed)
- Source→SampleGrabber→NullRenderer filter graph, camera index 0
- MJPG codec at 1920x1080, configured via `IAMStreamConfig`
- `ISampleGrabberCB::BufferCB` delivers decoded BGR frames on DirectShow's streaming thread
- Graph runs with no reference clock (`SetSyncSource(NULL)`) for freerunning frame delivery
- SampleGrabber allocator configured with 4 buffers to prevent pipeline stalls
- Render loop synced to webcam via auto-reset Windows Event (`MsgWaitForMultipleObjects`) — eliminates beat-frequency stutter from two independent 30fps loops
- Tracking data snapshot paired with each webcam frame in callback for video-locked 3D overlay
- Texture uploaded via PixelBufferDescriptor with delete callback

## MP4 Recording
- **Toggle**: F5 starts/stops recording to `recording.mp4`
- **Resolution**: Captures at current window size (supports 1080p, 4K, etc.)
- **Pipeline**: Double-buffered offscreen RenderTargets → `readPixels` from previous frame (no GPU stall) → RGBA piped to FFmpeg NVENC HEVC encoder
- **Framerate**: Matched to webcam fps; wall-clock paced so video plays at real-time speed
- **Vulkan note**: readPixels returns top-down data (no flip needed, unlike OpenGL)
- **Stats**: On stop, writes `recording_stats.txt` with frame count, duration, and actual fps

## ImGui Scene Control Panel
- **Separate SDL window** with OpenGL context, positioned to the left of the render window
- **SDL2 backend**: `imgui_impl_sdl2` + `imgui_impl_opengl3` handle input and rendering
- **Sections**:
  - **Visibility**: Dynamic checkboxes for cone, cube, all GLB objects, ground plane, trail markers
  - **Cone**: Position, scale, +90° rotation buttons
  - **GLB Objects**: Per-object collapsible sections with position, Y rotation, scale, AABB offset, +90° rotation buttons, Remove button
  - **Sun Light**: Intensity, yaw, pitch
  - **IBL**: Intensity
  - **Ground Plane**: Y position, shadow strength
  - **Camera**: Auto orbit, distance, yaw, pitch, target (only when tracking inactive)
  - **Scene File**: Reload Scene, Save As... (native dialog), Open... (native dialog)
  - **Add GLB Object**: Browse... (native dialog), name input, Add button
  - **Recording**: Start/Stop, frame counter
- **Native file dialogs**: Windows `GetOpenFileName`/`GetSaveFileName` for scene JSON and GLB files
- **Recording**: ImGui view is NOT rendered to recording pipeline — recordings are clean 3D only

## Camera Tracking Integration

### Serial Port
- Port: COM5
- Baud rate: 115200
- Protocol: VIPS Universal Protocol (see `VIPS_Universal_Protocol.txt`)

### VIPS Protocol
- Binary format, little-endian
- Header: 0x24 0xD9
- Provides X, Y, Z position (meters) and Roll, Pitch, Yaw orientation (degrees)
- CRC-16 checksum

### Coordinate System Conversion
VIPS to Filament:
- VIPS X (east) -> Filament X (right)
- VIPS Y (north) -> Filament -Z (forward)
- VIPS Z (up) -> Filament Y (up)

### Camera Behavior
- Camera position tracks real camera movement relative to initial position
- Camera orientation follows tracking roll/pitch/yaw
- Scene is placed 5m in front of the initial camera position
- Yaw is negated for correct left/right mapping
- Orbit camera pitch clamped to ±89°

## Window
- Resolution: 1920x1080 (default, resizable)
- DPI-aware (per-monitor aware v2)
- Title bar shows FPS and tracking data (X, Y, Z, Roll, Pitch, Yaw)

## Controls
### When tracking inactive
- Left mouse drag: Orbit camera (pitch clamped to ±89°)
- Right mouse drag: Move light source
- Mouse wheel: Zoom
- ESC: Exit

### Cone controls
- Arrow keys: Move cone horizontally (1m increments)
- A/Z: Move cone up/down (0.1m increments)
- X/C: Scale cone by 25% (smaller/larger)
- R: Place cone 1m in front of camera
- G: Drop cone tip to ground plane (Y=1.0m)

### First GLB object controls (Shift + key)
- Shift+Arrows: Move horizontally (1m increments)
- Shift+A/Z: Move up/down (1m increments)
- Shift+X/C: Scale by 25%
- Shift+</>: Rotate by 15 degrees
- Shift+R: Place 2m in front of camera, facing camera

### Recording
- F5: Toggle MP4 recording on/off

### Other
- Delete: Clear trail markers
