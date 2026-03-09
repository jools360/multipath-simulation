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

#include <string>
#include <vector>

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
    void findReflections(const float3& satDir, int prn, float elDeg, float azDeg);
    void updateSignalLines();
    void destroySignalLines();
    void buildImGuiPanel();
    void drawSkyPlot();
    void updateSidebarLayout();
    void saveSettings();
    void loadSettings();

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
    IndirectLight* mIndirectLight = nullptr;

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
    float3 mReceiverPos = {0, 1.5f, 0}; // Antenna phase center in model coords
    float mAntennaHeight = 1.5f;         // Antenna height above ground (metres)

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
    float mLineWidth = 1.0f;             // signal line width in metres

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

    // Receiver marker
    VertexBuffer* mReceiverVB = nullptr;
    IndexBuffer* mReceiverIB = nullptr;
    MaterialInstance* mReceiverMI = nullptr;
    Entity mReceiverEntity;

    // Ground plane
    VertexBuffer* mGroundVB = nullptr;
    IndexBuffer* mGroundIB = nullptr;
    MaterialInstance* mGroundMI = nullptr;
    Entity mGroundEntity;

    // Camera orbit controls
    float mCameraDistance = 500.0f;
    float mCameraYaw = 0.0f;       // radians
    float mCameraPitch = -1.0f;    // radians (negative = looking down)
    float3 mCameraTarget = {0, 0, 0};
    bool mDragging = false;
    bool mPanning = false;
    int mLastMouseX = 0, mLastMouseY = 0;

    // ImGui sidebar
    SDL_Window* mControlWindow = nullptr;
    SDL_GLContext mGLContext = nullptr;
    Uint32 mMainWindowID = 0;
    Uint32 mControlWindowID = 0;
    int mSidebarWidth = 400;
    bool mRunning = true;

    // File paths
    char mAlmanacPath[512] = "";
    char mBuildingModelPath[512] = "London.glb";

    // State flags
    bool mNeedsRecompute = true;
    bool mShowLOS = true;
    bool mShowBlocked = true;
    bool mShowReflected = true;
    float mMoveSpeed = 5.0f;  // meters per key press

    // Stats
    int mLOSCount = 0;
    int mBlockedCount = 0;
    int mReflectedCount = 0;

    // Window size
    int mWinWidth = 1920;
    int mWinHeight = 1080;
};
