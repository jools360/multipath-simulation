# Multipath Simulation

Three 3D rendering applications built on Google's Filament engine (Vulkan backend), Windows-only.

## Applications

### GNSSMultipath

A GNSS satellite signal multipath simulation tool. Loads a 3D building model (GLB), computes satellite positions from a GPS YUMA almanac, and uses CPU ray tracing to determine line-of-sight, blockage, and single-bounce reflections off building surfaces.

**Features:**
- GPS YUMA almanac parsing with Keplerian orbit propagation
- BVH-accelerated ray tracing against 3D building geometry
- Signal classification: LOS (green), blocked (red), reflected (orange)
- Single-bounce specular reflection detection using mirror-image method
- Car body signal blocking via oriented bounding box aligned to heading
- KML trajectory loading with Gaussian-smoothed playback along the route
- GLB car model (default Ford Mustang Mach-E) with ground clamping and heading
- Configurable model origin alignment (lat/lon/height) for trajectory overlay
- Sky plot with colour-coded satellite positions
- Signal analysis table with per-satellite status and multipath delay
- Adjustable IBL intensity for car/building lighting balance
- Orbit camera with follow-behind mode during playback
- MP4 video recording (F5) via FFmpeg NVENC HEVC with sidebar compositing
- ImGui sidebar overlay on main window, toggled with Tab
- Settings save/load for all parameters

### FilamentCone

A 3D rendering demo with live webcam background compositing and real-time camera tracking from an AirPixel positioning system. GLB models are overlaid on a live camera feed and move correctly as the camera moves through space.

**Features:**
- Real-time 3D rendering with PBR materials and shadows
- Live webcam feed as background (DirectShow, 1080p MJPG)
- Camera tracking via VIPS or FreeD protocol over RS-232 or UDP
- GLB/glTF model loading with full transform controls
- Scene save/load via JSON files
- MP4 video recording (FFmpeg NVENC)
- ImGui control panel for real-time scene editing

### GhostCar

Overlays a semi-transparent 3D ghost car on VBOX HD2 video, synchronized via VBO GPS telemetry for lap comparison. Load a VBO file, select a video lap and ghost lap, and watch the ghost car appear on the track showing the relative position of the comparison lap.

**Features:**
- VBO file parsing with automatic circuit detection (~900 circuits in database)
- Lap extraction from start/finish line crossings
- Semi-transparent ghost car (funcup.glb) positioned using GPS deltas
- Ghost car pitch follows road gradient from GPS height data
- Camera pitch compensation (tilts 3D camera to match road angle)
- Driving line overlay showing the full lap GPS path with height
- Adjustable playback speed (0.1x - 5.0x)
- Camera controls: FOV, height, pitch, pan
- Video-to-GPS data offset adjustment for sync alignment
- Settings save/load (auto-loads on startup)
- ImGui sidebar control panel

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
```

### Compiling Materials

Material source files (`.mat`) must be compiled with Filament's `matc` tool using the `-a vulkan` flag:

```bash
deps/bin/matc -o materials/ghost_car.filamat -p desktop -a vulkan materials/ghost_car.mat
```

## GhostCar Usage

1. Run `build/Release/GhostCar.exe`
2. The VBO file path defaults to the sample data — click **Load VBO**
3. The circuit is auto-detected and laps are extracted
4. Select a **Video** lap and a **Ghost** lap from the lap list (fastest is highlighted green)
5. Click **Play** to start playback
6. Use the sidebar to adjust:
   - **Alpha** — ghost car transparency
   - **Scale / Y Offset / Rotation** — ghost car positioning
   - **Pitch Compensation** — tilt camera to match road gradient
   - **Show Driving Line** — overlay the lap's GPS path
   - **Speed** — playback speed
   - **Camera FOV / Height / Pitch / Pan** — camera adjustments
   - **Data Offset** — shift GPS data timing vs video (ms)
7. Press **Space** to pause/resume, adjust settings while paused
8. Click **Save Settings** to persist all adjustments

### VBO File Format

VBOX HD2 data logger files (`.vbo`) are space-delimited text with sections in `[brackets]`. The `[AVI]` section links to the corresponding MP4 video file. Longitude values are sign-inverted in VBO files. See `VBO file description.txt` for full format details.

## FilamentCone Usage

### Keyboard Controls

| Key | Action |
|-----|--------|
| Arrow keys | Move cone horizontally |
| A / Z | Move cone up / down |
| X / C | Scale cone |
| Shift + Arrows | Move first GLB object |
| Shift + A / Z | Move GLB object up / down |
| Shift + X / C | Scale GLB object |
| Shift + < / > | Rotate GLB object |
| F5 | Toggle MP4 recording |
| Delete | Clear trail markers |
| ESC | Exit |

### Camera Tracking

Supports VIPS (AirPixel proprietary) and FreeD protocols over RS-232 or UDP. Auto-detect mode identifies the protocol automatically. Configure in the ImGui control panel.

## GNSSMultipath Usage

1. Run `build/Release/GNSSMultipath.exe`
2. In **Files**, browse for a building model GLB (e.g. `London.glb`) and a YUMA almanac file (`.alm`)
3. Click **Load Almanac** to parse satellite data
4. In **Receiver**, set your latitude, longitude, and antenna height
5. In **Date & Time**, set the UTC time for satellite position computation
6. Signal lines appear automatically: green (LOS), red (blocked), orange (reflected)
7. In **Trajectory**, browse for a KML file and click **Load KML**
8. Adjust **Model Origin Lat/Lon/Height** to align the trajectory with the 3D model
9. Click **Play** to animate the receiver along the trajectory
10. Use **Save Settings** / **Load Settings** to persist all parameters

### Controls

| Input | Action |
|-------|--------|
| Left drag | Orbit camera |
| Middle drag | Pan camera |
| Scroll wheel | Zoom in/out |
| Arrow keys | Move receiver horizontally |
| A / Z | Move receiver up / down |
| Space | Play / pause trajectory playback |
| Tab | Toggle sidebar visibility |
| F5 | Toggle MP4 video recording |
| Q / ESC | Exit |

## Project Structure

```
src/
  main.cpp              FilamentCone entry point and main loop
  FilamentApp.h          FilamentCone application class
  ghostcar_main.cpp      GhostCar entry point
  GhostCarApp.cpp/h      GhostCar application
  VboParser.cpp/h        VBO file and circuit database parser
  gnss_main.cpp          GNSSMultipath entry point
  GNSSApp.cpp/h          GNSSMultipath application
  GNSSAlmanac.cpp/h      YUMA almanac parser and orbit propagation
  GNSSRayTrace.cpp/h     BVH ray tracer and GLB mesh extraction
  SceneManager.cpp/h     Scene save/load, GLB loading
  WebcamCapture.cpp/h    DirectShow webcam capture
  TrackingSystem.cpp/h   Camera tracking (VIPS/FreeD)
  Recording.cpp/h        MP4 recording pipeline
  ImGuiPanel.cpp/h       FilamentCone control panel UI
  ConeGenerator.cpp/h    Procedural cone mesh
materials/               Filament material sources (.mat)
assets/                  3D models (funcup.glb)
```
