#pragma once

#include "VboParser.h"

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
#include <filament/Texture.h>
#include <filament/IndirectLight.h>
#include <filament/Skybox.h>
#include <filament/Viewport.h>

#include <utils/EntityManager.h>

#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/TextureProvider.h>

#include <opencv2/videoio.hpp>
#include <opencv2/core.hpp>

#include <string>
#include <vector>

using namespace filament;
using namespace filament::math;
using namespace utils;

// Textured vertex for background quad
struct BgVertex {
    float position[3];
    float uv[2];
    int16_t tangents[4];
};

class GhostCarApp {
public:
    GhostCarApp();
    ~GhostCarApp();

    bool init();
    void run();

private:
    Material* loadMaterial(const std::string& name);
    void createBackgroundQuad();
    void createCamera();
    void createLighting();
    void loadGhostCar();
    void applyGhostTransparency();
    void updateGhostCarTransform(float x, float y, float z, float yRotRad, float pitchRad);
    void hideGhostCar();
    void showGhostCar();
    bool uploadVideoFrame(const cv::Mat& frame);
    void buildImGuiPanel();
    void updateSidebarLayout();
    int findSampleAtAviTime(const VboFile& vbo, int startIdx, int endIdx, double aviTimeMs);
    int findSampleAtElapsed(const VboFile& vbo, int startIdx, int endIdx, double elapsedSec);
    float computePitch(const VboFile& vbo, int sampleIdx, int windowSamples = 5);
    void saveSettings();
    void loadSettings();
    void createDrivingLine(const LapInfo& lap);
    void updateDrivingLine(int camIdx);
    void destroyDrivingLine();

    // Filament core
    SDL_Window* mWindow = nullptr;
    Engine* mEngine = nullptr;
    SwapChain* mSwapChain = nullptr;
    Renderer* mRenderer = nullptr;
    Scene* mScene = nullptr;
    View* mView = nullptr;
    Camera* mCamera = nullptr;
    Entity mCameraEntity;

    // Background video
    Texture* mVideoTexture = nullptr;
    Material* mBackgroundMaterial = nullptr;
    MaterialInstance* mBackgroundMI = nullptr;
    VertexBuffer* mBackgroundVB = nullptr;
    IndexBuffer* mBackgroundIB = nullptr;
    Entity mBackgroundEntity;
    Scene* mBackgroundScene = nullptr;
    View* mBackgroundView = nullptr;
    Camera* mBackgroundCamera = nullptr;
    Entity mBackgroundCameraEntity;

    // Ghost car
    filament::gltfio::AssetLoader* mAssetLoader = nullptr;
    filament::gltfio::MaterialProvider* mMaterialProvider = nullptr;
    filament::gltfio::ResourceLoader* mResourceLoader = nullptr;
    filament::gltfio::TextureProvider* mStbDecoder = nullptr;
    filament::gltfio::FilamentAsset* mGhostAsset = nullptr;
    Material* mGhostMaterial = nullptr;
    std::vector<MaterialInstance*> mGhostMIs;

    // Lighting
    Entity mSunLight;
    IndirectLight* mIndirectLight = nullptr;
    Texture* mReflectionsCubemap = nullptr;

    // Video playback
    cv::VideoCapture mVideoCapture;
    int mVideoWidth = 1920;
    int mVideoHeight = 1080;
    double mVideoFps = 25.0;
    bool mVideoOpen = false;

    // VBO data (camera)
    VboFile mVbo;
    std::vector<CircuitInfo> mCircuitDb;
    CircuitInfo mCircuit;
    bool mCircuitDetected = false;
    std::vector<LapInfo> mLaps;
    int mFastestLapIdx = -1;

    // VBO data (ghost - optional, separate file)
    VboFile mGhostVbo;
    CircuitInfo mGhostCircuit;
    bool mGhostCircuitDetected = false;
    std::vector<LapInfo> mGhostLaps;
    int mGhostFastestLapIdx = -1;
    bool mGhostVboLoaded = false;
    char mGhostVboPath[512] = "";

    // Lap selection & playback
    int mCameraLapIdx = -1;
    int mGhostLapIdx = -1;
    bool mPlaying = false;
    bool mPaused = false;
    double mPlayStartTime = 0;
    double mPlayElapsedSec = 0;
    double mNextFrameTime = 0;  // wall-clock time when next video frame is due
    int mCurrentVideoSampleIdx = 0;
    float mPlaybackSpeed = 1.0f;

    // Ghost car parameters
    float mGhostAlpha = 0.5f;
    float mGhostTint[3] = {0.3f, 0.7f, 1.0f};
    float mCameraFovH = 90.0f;
    float mCameraHeight = 1.1f;
    float mCameraPitchOffset = 0.0f;  // degrees, manual camera pitch adjustment
    float mCameraPanOffset = 0.0f;    // degrees, manual camera pan (yaw) adjustment
    float mDataOffsetMs = 0.0f;       // ms offset between video and GPS data (positive = data ahead of video)
    float mGhostYOffset = 0.0f;
    float mModelRotOffset = 180.0f;  // degrees, to align model forward direction
    bool mGhostVisible = true;
    float mGhostScale = 1.3f;
    bool mPitchCompensation = true;

    // Driving line overlay (rendered as triangle strip ribbon)
    Material* mLineMaterial = nullptr;
    bool mShowDrivingLine = false;
    float mLineWidth = 0.5f;  // meters
    Entity mLineEntity;
    VertexBuffer* mLineVB = nullptr;
    IndexBuffer* mLineIB = nullptr;
    MaterialInstance* mLineMI = nullptr;
    int mLineSampleCount = 0;   // number of GPS samples in the line
    int mLineVertexCount = 0;   // = mLineSampleCount * 2 (left/right per sample)

    // ImGui sidebar
    SDL_Window* mControlWindow = nullptr;
    SDL_GLContext mGLContext = nullptr;
    Uint32 mMainWindowID = 0;
    Uint32 mControlWindowID = 0;
    int mSidebarWidth = 350;
    bool mRunning = true;

    // File paths
    char mVboPath[512] = "C:\\Users\\julia\\Dropbox\\Claude Code\\GhostCar\\VBOX data\\VBOX0006.vbo";
    char mDbPath[512] = "";
    bool mVboLoaded = false;
    bool mUseSeparateGhostVbo = false;

    // Window size
    int mWinWidth = 1920;
    int mWinHeight = 1080;
};
