#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <deque>
#include <mutex>
#include <atomic>
#include <chrono>

#include <math/vec3.h>
#include <math/vec4.h>
#include <math/mat4.h>

#include <gltfio/FilamentAsset.h>

constexpr int WINDOW_WIDTH = 1920;
constexpr int WINDOW_HEIGHT = 1080;

// Tracking protocol and transport enums
enum class TrackingProtocol { AutoDetect, VIPS, FreeD };
enum class TrackingTransport { Serial, UDP };

// VIPS Protocol constants
constexpr uint8_t VIPS_HEADER_1 = 0x24;
constexpr uint8_t VIPS_HEADER_2 = 0xD9;
constexpr uint32_t OUTPUT_ORIENTATION = 0x0004;

// FreeD Protocol constants
constexpr uint8_t FREED_MSG_D1 = 0xD1;
constexpr int FREED_PACKET_LEN = 29;

// Camera tracking data
struct CameraTrackingData {
    double x = 0.0;      // meters
    double y = 0.0;      // meters
    float z = 0.0f;      // meters
    float roll = 0.0f;   // degrees
    float pitch = 0.0f;  // degrees
    float yaw = 0.0f;    // degrees (heading)
    uint32_t vipsTimeMS = 0;  // VIPS message timestamp (ms, from AirPixel clock)
    int64_t localTimeUs = 0;  // local steady_clock timestamp (microseconds)
    uint8_t beacons = 0;      // number of satellites/beacons
    uint8_t solutionType = 0; // 0=none, 1=single, 2=DGPS, 3=RTK Float, 4=RTK Fixed
    bool valid = false;
};

// Textured vertex for background quad (includes tangents for Filament compatibility)
struct TexturedVertex {
    float position[3];
    float uv[2];
    int16_t tangents[4];
};

// Scene object: unified representation for GLB models
struct SceneObject {
    std::string name;
    std::string glbFilename;
    filament::math::float3 position = {0, 0, 0};
    float scale = 1.0f;
    float yRotation = 0.0f;        // radians
    int rot90[3] = {0, 0, 0};      // X, Y, Z rotation in 90-degree steps
    filament::math::float3 aabbCenterOffset = {0, 0, 0};  // offset to center the model
    bool visible = true;
    filament::gltfio::FilamentAsset* asset = nullptr;  // runtime, not serialized
};
