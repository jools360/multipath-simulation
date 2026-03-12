#pragma once

#include "GNSSAlmanac.h"
#include "GNSSRayTrace.h"

#include <SDL.h>

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Renderer.h>
#include <filament/Camera.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Skybox.h>
#include <filament/IndirectLight.h>
#include <filament/Viewport.h>

#include <utils/EntityManager.h>

#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/TextureProvider.h>

#include <filament/RenderTarget.h>
#include <filament/Texture.h>
#include <backend/PixelBufferDescriptor.h>

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

using namespace filament;
using namespace filament::math;
using namespace utils;

struct SignalResult {
    int prn = 0;
    enum Type { LOS, BLOCKED, REFLECTED } type = BLOCKED;
    float3 satDirection = {0,0,0};      // unit vector from receiver toward satellite
    float3 hitPoint = {0,0,0};          // building hit point (blocked) or reflection point
    float3 reflectionNormal = {0,0,0};  // normal at reflection point
    float3 reflEndpoint = {0,0,0};      // where the reflected incoming ray enters the scene (sky end)
    float extraPathM = 0;               // extra path length in meters (multipath delay)
    float elevationDeg = 0;
    float azimuthDeg = 0;
};

class GNSSApp {
public:
    GNSSApp();
    ~GNSSApp();

    bool init();
    void run();

private:
    Material* loadMaterial(const std::string& name);
    void createCamera();
    void createLighting();
    void loadBuildingModel(const std::string& path);
    void analyzeSignals();
    void findReflections(const float3& antennaPos, const float3& satDir, int prn, float elDeg, float azDeg);
    void updateSignalLines();
    void destroySignalLines();
    void buildImGuiPanel();
    void drawSkyPlot();
    void updateSidebarLayout();
    void saveSettings();
    void loadSettings();
    void startRecording();
    void stopRecording();

    // Trajectory
    bool loadKML(const std::string& path);
    float3 latLonToModel(double lat, double lon);
    void buildTrajectoryLine();
    void destroyTrajectoryLine();

    // Filament core
    SDL_Window* mWindow = nullptr;
    Engine* mEngine = nullptr;
    SwapChain* mSwapChain = nullptr;
    Renderer* mRenderer = nullptr;
    Scene* mScene = nullptr;
    View* mView = nullptr;
    Camera* mCamera = nullptr;
    Entity mCameraEntity;
    Skybox* mSkybox = nullptr;

    // Building model (rendered by Filament)
    filament::gltfio::AssetLoader* mAssetLoader = nullptr;
    filament::gltfio::MaterialProvider* mMaterialProvider = nullptr;
    filament::gltfio::ResourceLoader* mResourceLoader = nullptr;
    filament::gltfio::TextureProvider* mStbDecoder = nullptr;
    filament::gltfio::FilamentAsset* mBuildingAsset = nullptr;

    // Lighting
    Entity mSunLight;
    Entity mFillLight;
    Entity mBackLight;
    IndirectLight* mIndirectLight = nullptr;
    float mIBLIntensity = 50000.0f;

    // Mesh data for ray tracing (CPU-side)
    gnss::BVH mBVH;
    bool mMeshLoaded = false;

    // Satellite data
    std::vector<AlmanacEntry> mAlmanac;
    std::vector<SatellitePosition> mSatPositions;
    bool mAlmanacLoaded = false;

    // Receiver configuration
    double mReceiverLat = 51.5074;    // London default
    double mReceiverLon = -0.1278;
    double mGroundElevation = 11.0;   // Ground elevation above WGS-84 ellipsoid (metres) - London ~11m
    float3 mReceiverPos = {0, 1.5f, 0}; // Car base position in model coords
    float mAntennaHeight = 1.5f;         // Height above ground for car base (metres)
    float mAntennaRoofOffset = 1.8f;     // Antenna height above car base (metres) - on roof

    // UTC time for satellite computation
    int mYear = 2024, mMonth = 1, mDay = 15;
    int mHour = 12, mMinute = 0;
    float mSecond = 0.0f;
    float mElevationMask = 10.0f; // degrees

    // Model-world mapping
    // Geographic coordinates of the model's bounding box centre
    double mModelOriginLat = 51.5074;
    double mModelOriginLon = -0.1278;
    float mModelOriginHeight = 0.0f;   // vertical offset of model origin (metres)
    float3 mModelCenter = {0, 0, 0};  // model bounding box centre XZ (set on model load)
    float mModelNorthRotation = 0.0f;  // degrees: rotation from model -Z to true north

    // Trajectory (loaded from KML)
    struct TrajectoryPoint {
        double lat, lon;
        float3 modelPos;
        float cumDist;       // cumulative distance from start (metres)
    };
    std::vector<TrajectoryPoint> mTrajectory;
    bool mTrajectoryLoaded = false;
    float mTrajectoryTotalDist = 0;
    char mKmlPath[512] = "";

    // Trajectory playback
    bool mPlaying = false;
    bool mPlaybackPaused = false;
    float mPlaybackSpeed = 10.0f;    // m/s (~36 km/h)
    float mPlaybackDist = 0;         // current distance along path (metres)
    double mLastPlaybackTime = 0;
    bool mFollowCamera = true;       // camera follows receiver during playback

    // Trajectory line rendering
    VertexBuffer* mTrajectoryVB = nullptr;
    IndexBuffer* mTrajectoryIB = nullptr;
    MaterialInstance* mTrajectoryMI = nullptr;
    Entity mTrajectoryEntity;
    int mTrajectoryVertCount = 0;

    // Signal analysis results
    std::vector<SignalResult> mSignals;
    float mDomeRadius = 300.0f;          // visual dome radius for signal lines (meters)
    float mMaxReflectionRange = 200.0f;  // max distance to check for reflections (meters)
    float mLineWidth = 0.2f;             // signal line width in metres

    // Signal line rendering (3 batches: LOS, blocked, reflected)
    Material* mLineMaterial = nullptr;
    MaterialInstance* mLOSMI = nullptr;
    MaterialInstance* mBlockedMI = nullptr;
    MaterialInstance* mReflectedMI = nullptr;

    VertexBuffer* mLOSVB = nullptr;
    IndexBuffer* mLOSIB = nullptr;
    Entity mLOSEntity;
    int mLOSVertexCount = 0;

    VertexBuffer* mBlockedVB = nullptr;
    IndexBuffer* mBlockedIB = nullptr;
    Entity mBlockedEntity;
    int mBlockedVertexCount = 0;

    VertexBuffer* mReflectedVB = nullptr;
    IndexBuffer* mReflectedIB = nullptr;
    Entity mReflectedEntity;
    int mReflectedVertexCount = 0;

    // Receiver car model
    filament::gltfio::FilamentAsset* mCarAsset = nullptr;
    float mReceiverYaw = 0.0f;  // heading in radians (Filament Y-axis rotation)
    float mCarScale = 1.0f;
    char mCarModelPath[512] = "Ford_Mustang_Mach-E_2021.glb";
    void loadCarModel(const std::string& path);

    // Camera orbit controls
    float mCameraDistance = 500.0f;
    float mCameraYaw = 0.0f;       // radians
    float mCameraPitch = -1.0f;    // radians (negative = looking down)
    float3 mCameraTarget = {0, 0, 0};
    bool mDragging = false;
    bool mPanning = false;
    int mLastMouseX = 0, mLastMouseY = 0;

    // ImGui sidebar (overlay on main window)
    SDL_Window* mControlWindow = nullptr;
    SDL_GLContext mGLContext = nullptr;
    Uint32 mMainWindowID = 0;
    Uint32 mControlWindowID = 0;
    int mSidebarWidth = 400;
    bool mSidebarVisible = true;
    bool mRunning = true;

    // Video recording (double-buffered offscreen RT → FFmpeg pipe)
    bool mRecording = false;
    Texture* mRecordColor[2] = {};
    Texture* mRecordDepth[2] = {};
    RenderTarget* mRecordRT[2] = {};
    View* mRecordView = nullptr;
    int mRecordFrameIndex = 0;
    int mRecordWidth = 0;
    int mRecordHeight = 0;
    FILE* mRecordPipe = nullptr;
    std::thread mRecordThread;
    std::mutex mRecordMutex;
    std::condition_variable mRecordCV;
    std::queue<uint8_t*> mRecordQueue;
    std::atomic<bool> mRecordThreadRunning{false};
    int mRecordFrameCount = 0;
    std::chrono::steady_clock::time_point mRecordStartTime;
    std::chrono::steady_clock::time_point mRecordNextCapture;
    int mRecordFps = 30;

    // Sidebar capture for compositing into recording
    std::vector<uint8_t> mSidebarCapture;
    int mSidebarCaptureW = 0, mSidebarCaptureH = 0;
    std::mutex mSidebarCaptureMutex;

    // File paths
    char mAlmanacPath[512] = "";
    char mBuildingModelPath[512] = "London.glb";

    // State flags
    bool mNeedsRecompute = true;
    bool mShowLOS = true;
    bool mShowBlocked = true;
    bool mShowReflected = true;
    bool mShowTrajectory = true;
    float mMoveSpeed = 5.0f;  // meters per key press

    // Stats
    int mLOSCount = 0;
    int mBlockedCount = 0;
    int mReflectedCount = 0;

    // Window size
    int mWinWidth = 1920;
    int mWinHeight = 1080;
};
