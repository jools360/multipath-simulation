# AirPixel Graphics Engine

A 3D rendering application built on Google's Filament engine with live webcam background compositing and real-time camera tracking from an AirPixel positioning system. Designed for augmented reality demos where 3D objects are overlaid on a live camera feed and move correctly as the camera moves through space.

## Features

- Real-time 3D rendering with PBR materials and shadows
- Live webcam feed as background (DirectShow, 1080p MJPG)
- Camera tracking via VIPS or FreeD protocol over RS-232 or UDP
- GLB/glTF model loading with full transform controls
- Scene save/load via JSON files
- MP4 video recording (FFmpeg NVENC)
- ImGui control panel for real-time scene editing

## Building

### Prerequisites

- **CMake** 3.19+
- **Visual Studio 2022** with C++ tools
- **Dependencies** in `deps/` folder (not included in repo):
  - [Filament](https://github.com/google/filament/releases) SDK
  - [SDL2](https://github.com/libsdl-org/SDL/releases) 2.30.11
  - [OpenCV](https://opencv.org/releases/) 4.12.0
  - [Dear ImGui](https://github.com/ocornut/imgui) 1.91.8
  - [nlohmann/json](https://github.com/nlohmann/json) (single header)

### Build Steps

```bash
cmake -B build -S .
cmake --build build --config Release
build\Release\FilamentCone.exe
```

### Compiling Materials

Material source files (`.mat`) are compiled with Filament's `matc` tool:

```bash
deps\bin\matc -o materials\shadow_plane.filamat -p desktop materials\shadow_plane.mat
```

## Loading 3D Objects

The engine supports loading `.glb` (binary glTF) models. There are three ways to add objects to the scene:

### Method 1: Via the ImGui Control Panel (easiest)

1. Open the application - the control panel window appears to the left of the render window
2. Scroll to the **Add GLB Object** section at the bottom
3. Click **Browse...** to open a file dialog and select a `.glb` file
4. Enter a name for the object in the **Name** field
5. Click **Add** - the object will appear in the scene

Once added, each object gets its own collapsible section in the panel where you can adjust:
- **Position** (X, Y, Z)
- **Y Rotation** (degrees)
- **Scale** (uniform)
- **AABB Center Offset** (for adjusting the pivot point)
- **+90 degree rotation buttons** for X, Y, Z axes
- **Visibility** checkbox
- **Remove** button

### Method 2: Via Scene Files (JSON)

Create or edit a `scene.json` file and place it next to the executable. It will be loaded automatically on startup.

```json
{
  "version": 1,
  "glbObjects": [
    {
      "name": "My Model",
      "filename": "my_model.glb",
      "position": [0, 0, 0],
      "scale": 1.0,
      "yRotation": 0,
      "rot90": [0, 0, 0],
      "aabbCenterOffset": [0, 0, 0],
      "visible": true
    }
  ]
}
```

Place the `.glb` file next to the executable. The `rot90` array applies 90-degree rotation increments on each axis (e.g., `[1, 0, 0]` rotates 90 degrees around X). The `aabbCenterOffset` shifts the model's pivot point.

You can also use **Save As...** and **Open...** buttons in the control panel to save/load scene files via native file dialogs.

### Method 3: Keyboard Shortcut

Press **Shift+R** to place the first GLB object 2 metres in front of the current camera position, facing the camera. Useful for quick repositioning.

### Supported Formats

- `.glb` (binary glTF 2.0) - PBR materials, textures, and mesh data
- Models with metallic/roughness PBR materials will render with correct reflections
- Place GLB files next to the executable or use absolute paths

## Keyboard Controls

### Camera (when tracking is inactive)

| Key | Action |
|-----|--------|
| Left mouse drag | Orbit camera around target |
| Right mouse drag | Rotate sun light direction |
| Mouse wheel | Zoom in/out |
| ESC | Exit application |

### Cone Controls

| Key | Action |
|-----|--------|
| Arrow keys | Move cone horizontally (1m steps) |
| A / Z | Move cone up / down (0.1m steps) |
| X / C | Scale cone smaller / larger (25%) |
| R | Place cone 1m in front of camera |
| G | Drop cone to ground plane |

### First GLB Object Controls

| Key | Action |
|-----|--------|
| Shift + Arrow keys | Move object horizontally (1m steps) |
| Shift + A / Z | Move object up / down (1m steps) |
| Shift + X / C | Scale object smaller / larger (25%) |
| Shift + < / > | Rotate object by 15 degrees |
| Shift + R | Place 2m in front of camera, facing camera |

### Recording

| Key | Action |
|-----|--------|
| F5 | Start / stop MP4 recording |

### Other

| Key | Action |
|-----|--------|
| Delete | Clear all trail markers |

## Scene File Reference

The full `scene.json` schema supports saving/restoring the entire scene state:

```json
{
  "version": 1,
  "camera": {
    "distance": 8.0,
    "yaw": 0.785,
    "pitch": 0.5,
    "target": [0, 0.5, 0],
    "autoOrbit": true
  },
  "lighting": {
    "sunIntensity": 150000,
    "sunYaw": -0.8,
    "sunPitch": -0.5,
    "iblIntensity": 5000,
    "shadowStrength": 0.5
  },
  "groundPlane": {
    "worldY": 1.0,
    "visible": true
  },
  "cone": {
    "position": [0, 0, 0],
    "scale": 1.0,
    "rot90": [0, 0, 0],
    "visible": true
  },
  "cube": { "visible": false },
  "trailMarkers": { "visible": true },
  "glbObjects": []
}
```

## Project Structure

```
src/
  main.cpp              Core application loop and rendering
  FilamentApp.h          Application class with all state
  Types.h                Shared types (SceneObject, CameraTrackingData)
  ConeGenerator.cpp/h    Procedural cone mesh
  SceneManager.cpp/h     Scene save/load, GLB loading
  WebcamCapture.cpp/h    DirectShow webcam capture
  TrackingSystem.cpp/h   Camera tracking (VIPS/FreeD, Serial/UDP)
  Recording.cpp/h        MP4 recording pipeline
  ImGuiPanel.cpp/h       Control panel UI
materials/               Filament material sources (.mat) and compiled (.filamat)
```

## Camera Tracking

The engine supports two tracking protocols and two transport methods, selectable in the control panel:

### Protocols

- **VIPS** — AirPixel's proprietary protocol. Variable-length binary messages with 2-byte header (`0x24 0xD9`). Provides 6DOF tracking (X, Y, Z + Roll, Pitch, Yaw) with satellite/beacon status. Serial default: 115200 baud, no parity.
- **FreeD** — Industry-standard broadcast/virtual production protocol. Fixed 29-byte packets (message type `0xD1`) with 3-byte encoded angles and positions, plus checksum. Serial default: 38400 baud, odd parity. UDP default: port 6301.
- **Auto-detect** (default) — Automatically identifies the protocol from incoming data. On serial, starts at VIPS settings and switches to FreeD after 2 seconds if no valid VIPS data is received.

### Transports

- **RS-232** — Serial port connection. Select the COM port from the dropdown and click Connect.
- **UDP** — Network listener. Enter the port number (default 6301) and click Listen. Each UDP datagram should contain a complete protocol message.

### Usage

1. In the control panel under **Tracking Input**, select the transport (RS-232 or UDP) and protocol (Auto-detect, VIPS, or FreeD)
2. For RS-232: click Refresh, select a COM port, and click Connect
3. For UDP: enter the port number and click Listen
4. The status line shows the connection state and detected protocol in green
5. The camera position and orientation update in real-time from tracking data
6. 3D objects stay fixed in world space as the camera moves
7. The webcam feed is composited as the background for augmented reality
