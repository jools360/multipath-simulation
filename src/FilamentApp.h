#pragma once

#include "Types.h"
#include "ConeGenerator.h"

#include <SDL.h>

#include <filament/Engine.h>
#include <filament/Viewport.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Renderer.h>
#include <filament/Camera.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Texture.h>
#include <filament/IndirectLight.h>
#include <filament/Skybox.h>
#include <filament/RenderTarget.h>

#include <utils/EntityManager.h>

#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/TextureProvider.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <queue>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// Forward declare DirectShow types used as member pointers
struct IGraphBuilder;
struct ICaptureGraphBuilder2;
struct IMediaControl;
struct IBaseFilter;
// ISampleGrabber defined in WebcamCapture.h
class ISampleGrabber;
class SampleGrabberCallback;
#endif

using namespace filament;
using namespace filament::math;
using namespace utils;

class FilamentApp;

// Free function declarations (used in inline wrappers below)
std::string scene_resolveFilePath(const std::string& filename);
std::string scene_exeDirPath(const std::string& filename);

// Friend function declarations
// SceneManager
void scene_save(FilamentApp* app, const std::string& filename);
void scene_load(FilamentApp* app, const std::string& filename);
void scene_loadGlbObject(FilamentApp* app, SceneObject& obj);
void scene_destroyAllGlbObjects(FilamentApp* app);
void scene_updateObjectTransform(FilamentApp* app, SceneObject& obj);

// WebcamCapture
void webcam_init(FilamentApp* app, int deviceIndex);
void webcam_shutdown(FilamentApp* app);
bool webcam_updateFrame(FilamentApp* app);
void webcam_createBackgroundQuad(FilamentApp* app);
void webcam_setupGradientBackground(FilamentApp* app);
std::vector<std::string> webcam_enumerateDevices();
void webcam_setCameraExposure(FilamentApp* app, long value, bool autoExp);

// TrackingSystem
bool tracking_openSerialPort(FilamentApp* app, const std::string& portName);
void tracking_closeSerialPort(FilamentApp* app);
bool tracking_openUdpPort(FilamentApp* app, int port);
void tracking_closeUdpPort(FilamentApp* app);
std::vector<std::string> tracking_enumerateComPorts();
void tracking_serialReadThread(FilamentApp* app);
void tracking_udpReadThread(FilamentApp* app);
bool tracking_parseVIPSMessage(FilamentApp* app, const uint8_t* data, size_t length);
bool tracking_parseFreeDMessage(FilamentApp* app, const uint8_t* data, size_t length);
void tracking_updateSceneFromTracking(FilamentApp* app, bool newWebcamFrame);

// Recording
void recording_start(FilamentApp* app);
void recording_stop(FilamentApp* app);

// LensDistortion
void distortion_loadCalibration(FilamentApp* app, const std::string& path);
void distortion_generateUVMap(FilamentApp* app);
void distortion_setup(FilamentApp* app);
void distortion_teardown(FilamentApp* app);

// ImGuiPanel
void imgui_buildPanel(FilamentApp* app);
void imgui_toggleEntityVisibility(FilamentApp* app, Entity entity, bool visible);
void imgui_toggleGlbVisibility(FilamentApp* app, filament::gltfio::FilamentAsset* asset, bool visible);

class FilamentApp {
public:
    FilamentApp();
    ~FilamentApp();

    bool init();
    void run();

    // These remain as methods since they're core app logic
    void createCone();
    void createLighting();
    void createCamera();
    void createModels();
    void updateConeTransform();
    Material* loadMaterial(const std::string& name);
    void update(float deltaTime, bool newWebcamFrame);
    bool render();
    void regenerateCubemap();
    void updateSidebarLayout();
    bool loadHdrEnvironment(const std::string& path);
    void clearHdrEnvironment();

    // Thin wrappers calling free functions
    void saveScene(const std::string& f) { scene_save(this, f); }
    void loadScene(const std::string& f) { scene_load(this, f); }
    void loadGlbObject(SceneObject& o) { scene_loadGlbObject(this, o); }
    void destroyAllGlbObjects() { scene_destroyAllGlbObjects(this); }
    void updateObjectTransform(SceneObject& o) { scene_updateObjectTransform(this, o); }
    std::string resolveFilePath(const std::string& f) { return scene_resolveFilePath(f); }
    std::string exeDirPath(const std::string& f) { return scene_exeDirPath(f); }

    void initWebcam(int idx = 0) { webcam_init(this, idx); }
    void shutdownWebcam() { webcam_shutdown(this); }
    bool updateWebcamFrame() { return webcam_updateFrame(this); }
    void createBackgroundQuad() { webcam_createBackgroundQuad(this); }
    std::vector<std::string> enumerateVideoDevices() { return webcam_enumerateDevices(); }
    void setupGradientBackground() { webcam_setupGradientBackground(this); }
    void setCameraExposure(long v, bool a) { webcam_setCameraExposure(this, v, a); }

    bool openSerialPort(const std::string& p) { return tracking_openSerialPort(this, p); }
    void closeSerialPort() { tracking_closeSerialPort(this); }
    bool openUdpPort(int port) { return tracking_openUdpPort(this, port); }
    void closeUdpPort() { tracking_closeUdpPort(this); }
    std::vector<std::string> enumerateComPorts() { return tracking_enumerateComPorts(); }
    void serialReadThread() { tracking_serialReadThread(this); }
    void udpReadThread() { tracking_udpReadThread(this); }
    bool parseVIPSMessage(const uint8_t* d, size_t l) { return tracking_parseVIPSMessage(this, d, l); }
    bool parseFreeDMessage(const uint8_t* d, size_t l) { return tracking_parseFreeDMessage(this, d, l); }
    void updateSceneFromTracking(bool n) { tracking_updateSceneFromTracking(this, n); }

    void startRecording() { recording_start(this); }
    void stopRecording() { recording_stop(this); }

    void loadDistortionCalibration(const std::string& p) { distortion_loadCalibration(this, p); }
    void setupDistortion() { distortion_setup(this); }
    void teardownDistortion() { distortion_teardown(this); }
    void generateDistortionUVMap() { distortion_generateUVMap(this); }

    void buildImGuiPanel() { imgui_buildPanel(this); }
    void toggleEntityVisibility(Entity e, bool v) { imgui_toggleEntityVisibility(this, e, v); }
    void toggleGlbVisibility(filament::gltfio::FilamentAsset* a, bool v) { imgui_toggleGlbVisibility(this, a, v); }

private:
    // Friend declarations for free functions that access private members
    friend void scene_save(FilamentApp*, const std::string&);
    friend void scene_load(FilamentApp*, const std::string&);
    friend void scene_loadGlbObject(FilamentApp*, SceneObject&);
    friend void scene_destroyAllGlbObjects(FilamentApp*);
    friend void scene_updateObjectTransform(FilamentApp*, SceneObject&);
    friend filament::gltfio::FilamentAsset* scene_loadGlbModel(FilamentApp*, const std::string&);

    friend void webcam_init(FilamentApp*, int);
    friend void webcam_shutdown(FilamentApp*);
    friend bool webcam_updateFrame(FilamentApp*);
    friend void webcam_createBackgroundQuad(FilamentApp*);
    friend void webcam_setupGradientBackground(FilamentApp*);
    friend void webcam_setCameraExposure(FilamentApp*, long, bool);

    friend bool tracking_openSerialPort(FilamentApp*, const std::string&);
    friend void tracking_closeSerialPort(FilamentApp*);
    friend bool tracking_openUdpPort(FilamentApp*, int);
    friend void tracking_closeUdpPort(FilamentApp*);
    friend void tracking_serialReadThread(FilamentApp*);
    friend void tracking_udpReadThread(FilamentApp*);
    friend bool tracking_parseVIPSMessage(FilamentApp*, const uint8_t*, size_t);
    friend bool tracking_parseFreeDMessage(FilamentApp*, const uint8_t*, size_t);
    friend void tracking_updateSceneFromTracking(FilamentApp*, bool);

    friend void recording_start(FilamentApp*);
    friend void recording_stop(FilamentApp*);

    friend void distortion_loadCalibration(FilamentApp*, const std::string&);
    friend void distortion_generateUVMap(FilamentApp*);
    friend void distortion_setup(FilamentApp*);
    friend void distortion_teardown(FilamentApp*);

    friend void imgui_buildPanel(FilamentApp*);
    friend void imgui_toggleEntityVisibility(FilamentApp*, Entity, bool);
    friend void imgui_toggleGlbVisibility(FilamentApp*, filament::gltfio::FilamentAsset*, bool);

    SDL_Window* mWindow = nullptr;
    Engine* mEngine = nullptr;
    SwapChain* mSwapChain = nullptr;
    Renderer* mRenderer = nullptr;
    Scene* mScene = nullptr;
    View* mView = nullptr;
    Camera* mCamera = nullptr;
    Entity mCameraEntity;
    Skybox* mSkybox = nullptr;

    // Cone
    Entity mConeEntity;
    VertexBuffer* mConeVB = nullptr;
    IndexBuffer* mConeIB = nullptr;
    Material* mRedMaterial = nullptr;
    MaterialInstance* mConeMI = nullptr;
    std::vector<Vertex> mConeVertices;
    std::vector<uint16_t> mConeIndices;

    Entity mSunLight;
    IndirectLight* mIndirectLight = nullptr;

    // Tracking protocol & transport
    TrackingProtocol mTrackingProtocol = TrackingProtocol::AutoDetect;
    TrackingProtocol mDetectedProtocol = TrackingProtocol::AutoDetect;
    TrackingTransport mTrackingTransport = TrackingTransport::Serial;

    // Serial port / camera tracking
    HANDLE mSerialPort = INVALID_HANDLE_VALUE;
    HANDLE mSerialEvent = NULL;
    std::thread mSerialThread;
    std::atomic<bool> mSerialRunning{false};

    // UDP tracking
    SOCKET mUdpSocket = INVALID_SOCKET;
    int mUdpPort = 6301;
    char mUdpPortBuf[8] = "6301";
    std::thread mUdpThread;
    std::atomic<bool> mUdpRunning{false};
    bool mUdpListening = false;
    std::mutex mTrackingMutex;
    CameraTrackingData mTrackingData;
    CameraTrackingData mTrackingPrev;
    CameraTrackingData mTrackingDisplay;
    std::chrono::steady_clock::time_point mTrackingLatestLocalTime;

    // Tracking delay buffer
    std::deque<CameraTrackingData> mTrackingBuffer;
    float mTrackingDelayMs = 0.0f;
    CameraTrackingData mInitialTracking;
    bool mTrackingInitialized = false;
    float mSmoothedYaw = 0.0f;
    float mSmoothedPitch = 0.0f;
    float mSmoothedRoll = 0.0f;
    bool mOrientationSmoothed = false;
    float mOrientSmoothAlpha = 0.6f;
    float3 mAnchorWorldPos = {0, 0, 0};
    float3 mDebugOffset = {0, 0, 0};
    std::atomic<int> mSerialMsgCount{0};
    std::atomic<int> mWebcamCBCount{0};
    std::atomic<uint64_t> mTrackingSeq{0};
    uint64_t mLastTrailSeq = 0;

    // Trail queue
    std::vector<CameraTrackingData> mTrailQueue;
    std::mutex mTrailQueueMutex;

    // Per-frame tracking debug
    float mDbgRawYaw = 0.0f;
    float mDbgSmoothedYaw = 0.0f;
    float mDbgYawFil = 0.0f;
    float mDbgRawPitch = 0.0f;
    float mDbgSmoothedPitch = 0.0f;
    float mDbgRawX = 0.0f;
    float mDbgRawY = 0.0f;
    float mDbgRawZ = 0.0f;
    float mDbgSmoothedX = 0.0f;
    float mDbgSmoothedY = 0.0f;
    float mDbgSmoothedZ = 0.0f;

    // COM port selection
    std::vector<std::string> mComPorts;
    int mSelectedComPort = -1;
    std::string mConnectedPortName;

    // Video device selection
    std::vector<std::string> mVideoDevices;
    int mSelectedVideoDevice = -1;
    std::string mConnectedVideoDeviceName;

    // Webcam background
    Texture* mWebcamTexture = nullptr;
    Material* mBackgroundMaterial = nullptr;
    MaterialInstance* mBackgroundMI = nullptr;
    VertexBuffer* mBackgroundVB = nullptr;
    IndexBuffer* mBackgroundIB = nullptr;
    Entity mBackgroundEntity;
    Scene* mBackgroundScene = nullptr;
    View* mBackgroundView = nullptr;
    Camera* mBackgroundCamera = nullptr;
    Entity mBackgroundCameraEntity;
    std::vector<uint8_t> mWebcamFrameData;
    CameraTrackingData mWebcamTracking;
    CameraTrackingData mWebcamTrackingDisplay;
    std::mutex mWebcamMutex;
    std::atomic<bool> mWebcamNewFrame{false};
    int mWebcamWidth = 1920;
    int mWebcamHeight = 1080;
    int mWebcamFps = 30;
    bool mWebcamEnabled = false;
    bool mUseGradientBg = false;

    // DirectShow webcam capture
    IGraphBuilder* mDSGraph = nullptr;
    ICaptureGraphBuilder2* mDSCapture = nullptr;
    IMediaControl* mDSControl = nullptr;
    IBaseFilter* mDSSourceFilter = nullptr;
    IBaseFilter* mDSGrabberFilter = nullptr;
    IBaseFilter* mDSNullFilter = nullptr;
    ISampleGrabber* mDSSampleGrabber = nullptr;
    SampleGrabberCallback* mDSCallback = nullptr;
    HANDLE mWebcamFrameEvent = NULL;
    bool mCOMInitialized = false;
    long mCamExposure = -6;
    long mCamExposureMin = -11;
    long mCamExposureMax = 1;
    long mCamExposureStep = 1;
    bool mCamExposureAuto = true;
    bool mCamExposureAvailable = false;

    // Camera orbit controls
    float mCameraDistance = 8.0f;
    float mCameraYaw = 0.785f;
    float mCameraPitch = 0.5f;
    float3 mCameraTarget = {0.0f, 0.5f, 0.0f};
    bool mMouseDragging = false;
    int mLastMouseX = 0;
    int mLastMouseY = 0;

    // FPS tracking
    float mFpsAccumulator = 0.0f;
    int mFrameCount = 0;
    float mCurrentFps = 0.0f;

    // Light control
    float mLightYaw = -0.8f;
    float mLightPitch = -0.5f;
    bool mLightDragging = false;

    // glTF/GLB loading infrastructure
    filament::gltfio::AssetLoader* mAssetLoader = nullptr;
    filament::gltfio::MaterialProvider* mMaterialProvider = nullptr;
    filament::gltfio::ResourceLoader* mResourceLoader = nullptr;
    filament::gltfio::TextureProvider* mStbDecoder = nullptr;

    // GLB model objects
    std::vector<SceneObject> mGlbObjects;

    // Trail markers
    std::vector<Entity> mTrailMarkers;
    MaterialInstance* mTrailMI = nullptr;
    VertexBuffer* mTrailVB = nullptr;
    IndexBuffer* mTrailIB = nullptr;
    float3 mLastCamPos = {0, 0, 0};
    float3 mLastCamForward = {0, 0, -1};
    bool mTrailReady = false;

    // Procedural blue cube
    Entity mCubeEntity;
    VertexBuffer* mCubeVB = nullptr;
    IndexBuffer* mCubeIB = nullptr;
    MaterialInstance* mCubeMI = nullptr;

    // Shadow ground plane
    Entity mGroundEntity;
    VertexBuffer* mGroundVB = nullptr;
    IndexBuffer* mGroundIB = nullptr;
    MaterialInstance* mGroundMI = nullptr;

    float mGroundWorldY = 1.0f;
    float3 mConePos = {0, 0, 0};
    float mConeScale = 1.0f;
    int mConeRot[3] = {0, 0, 0};
    bool mRunning = true;

    // CSV logging
    FILE* mCsvFile = nullptr;
    FILE* mCsvSerialFile = nullptr;
    std::chrono::steady_clock::time_point mCsvStartTime;
    std::atomic<int64_t> mLastVideoFrameUTC_us{0};

    // MP4 recording
    std::string mRecordOutputPath;  // set to exe_dir/recording.mp4 by default
    FILE* mRecordPipe = nullptr;
    bool mRecording = false;
    int mRecordFrameCount = 0;
    std::chrono::steady_clock::time_point mRecordStartTime;
    std::thread mRecordThread;
    std::mutex mRecordMutex;
    std::condition_variable mRecordCV;
    std::queue<uint8_t*> mRecordQueue;
    std::atomic<bool> mRecordThreadRunning{false};

    // Double-buffered offscreen RT for recording
    Texture* mRecordColor[2] = {};
    Texture* mRecordDepth[2] = {};
    RenderTarget* mRecordRT[2] = {};
    View* mRecordBgView = nullptr;
    View* mRecordMainView = nullptr;
    int mRecordFrameIndex = 0;
    int mRecordWidth = 0;
    int mRecordHeight = 0;
    std::chrono::steady_clock::time_point mRecordNextCapture;

    // ImGui sidebar (owned overlay or floating window)
    SDL_Window* mControlWindow = nullptr;
    SDL_GLContext mGLContext = nullptr;
    Uint32 mMainWindowID = 0;
    Uint32 mControlWindowID = 0;
    bool mSidebarVisible = true;
    bool mSidebarDocked = true;  // true = overlays main window, false = floating
    int mSidebarWidth = 300;  // scaled by DPI in init()

    // Visibility toggles
    bool mShowCone = true;
    bool mShowCube = false;
    bool mShowGroundPlane = true;
    bool mShowTrailMarkers = true;
    bool mAutoOrbit = true;

    // Screen-space reflections
    bool mSSREnabled = false;
    float mSSRThickness = 0.1f;
    float mSSRBias = 0.01f;
    float mSSRMaxDistance = 10.0f;
    float mSSRStride = 2.0f;

    // Exposure
    float mExposure = 15.0f;  // EV100

    // Light params
    float mSunIntensity = 150000.0f;
    float mIBLIntensity = 5000.0f;
    float mShadowStrength = 0.5f;

    // Cubemap environment params
    Texture* mReflectionsCubemap = nullptr;
    float mEnvGroundBrightness = 1.0f;
    float mEnvSkyBrightness = 1.0f;
    float mEnvHorizonBrightness = 1.0f;
    float mEnvLightIntensity = 1.0f;
    float mEnvSkyBlueness = 1.0f;
    bool mUseHdrEnv = false;
    std::string mHdrEnvPath;

    // Scene file
    char mSceneFilename[256] = "scene.json";
    char mNewGlbFilename[256] = "";
    char mNewGlbName[128] = "";

    // Lens distortion
    bool mDistortionEnabled = false;
    bool mDistortionCalibLoaded = false;
    bool mDistortionSetup = false;
    bool mDistortionShowGrid = false;
    int mDistortionFrameDbg = 0;
    std::string mDistortionCalibFile;
    std::string mDistortionCameraName;
    int mDistortionImageW = 1920;
    int mDistortionImageH = 1080;
    double mDistK1 = 0.0, mDistK2 = 0.0, mDistK3 = 0.0;
    double mDistP1 = 0.0, mDistP2 = 0.0;
    double mDistFx = 0.0, mDistFy = 0.0;
    double mDistCx = 0.0, mDistCy = 0.0;

    // Distortion pipeline resources
    Texture* mDistortionColor = nullptr;
    Texture* mDistortionDepth = nullptr;
    RenderTarget* mDistortionRT = nullptr;
    Texture* mDistortionUVMap = nullptr;
    int mDistortionUVMapW = 0;
    int mDistortionUVMapH = 0;
    // Overscan bounds: the range of source UVs after forward distortion.
    // Camera projection is widened so the RT covers this range.
    float mDistortionOverscanMinU = 0.0f;
    float mDistortionOverscanMaxU = 1.0f;
    float mDistortionOverscanMinV = 0.0f;
    float mDistortionOverscanMaxV = 1.0f;
    View* mDistortionOffscreenView = nullptr;  // renders 3D scene to RT
    View* mDistortionView = nullptr;           // distortion quad output view
    Scene* mDistortionScene = nullptr;
    Camera* mDistortionCamera = nullptr;
    Entity mDistortionCameraEntity;
    Entity mDistortionEntity;
    VertexBuffer* mDistortionVB = nullptr;
    IndexBuffer* mDistortionIB = nullptr;
    Material* mDistortionMaterial = nullptr;
    MaterialInstance* mDistortionMI = nullptr;
};
