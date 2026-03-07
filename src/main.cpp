/*
 * Filament Red Cone Demo
 *
 * A simple application demonstrating a 3D red cone with PBR lighting
 * using Google's Filament rendering engine.
 */

#include "FilamentApp.h"
#include "SceneManager.h"
#include "WebcamCapture.h"
#include "TrackingSystem.h"
#include "Recording.h"
#include "LensDistortion.h"
#include "ImGuiPanel.h"

#include <SDL.h>

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <filament/Renderer.h>
#include <filament/Camera.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/LightManager.h>
#include <filament/Skybox.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/IndirectLight.h>
#include <filament/RenderTarget.h>

#include <utils/EntityManager.h>

#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/TextureProvider.h>
#include <gltfio/materials/uberarchive.h>

#include <math/vec3.h>
#include <math/vec4.h>
#include <math/mat4.h>

#include <fstream>
#include <vector>
#include <iostream>
#include <cmath>
#include <string>

#include <opencv2/opencv.hpp>

#include "stb_image.h"

#include "ConeGenerator.h"
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <SDL_opengl.h>

#ifdef _WIN32
#include <SDL_syswm.h>
#include <windows.h>
#include <timeapi.h>
#include <fcntl.h>
#include <io.h>
#pragma comment(lib, "winmm.lib")
#endif

#include <thread>
#include <atomic>
#include <mutex>
#include <future>
#include <queue>

FilamentApp::FilamentApp() = default;

FilamentApp::~FilamentApp() {
    teardownDistortion();
    stopRecording();

    if (mCsvFile) {
        fclose(mCsvFile);
        mCsvFile = nullptr;
    }
    if (mCsvSerialFile) {
        fclose(mCsvSerialFile);
        mCsvSerialFile = nullptr;
    }
    closeSerialPort();

    // Cleanup ImGui
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (mGLContext) {
        SDL_GL_DeleteContext(mGLContext);
        mGLContext = nullptr;
    }
    if (mControlWindow) {
        SDL_DestroyWindow(mControlWindow);
        mControlWindow = nullptr;
    }

    // Stop DirectShow capture graph
    shutdownWebcam();
    if (mCOMInitialized) { CoUninitialize(); mCOMInitialized = false; }

    if (mEngine) {
        // Webcam cleanup
        if (mBackgroundView) mEngine->destroy(mBackgroundView);
        if (mBackgroundScene) mEngine->destroy(mBackgroundScene);
        if (mBackgroundCamera) {
            mEngine->destroyCameraComponent(mBackgroundCameraEntity);
            EntityManager::get().destroy(mBackgroundCameraEntity);
        }
        if (mBackgroundEntity) {
            mEngine->getRenderableManager().destroy(mBackgroundEntity);
            EntityManager::get().destroy(mBackgroundEntity);
        }
        if (mBackgroundMI) mEngine->destroy(mBackgroundMI);
        if (mBackgroundMaterial) mEngine->destroy(mBackgroundMaterial);
        if (mBackgroundVB) mEngine->destroy(mBackgroundVB);
        if (mBackgroundIB) mEngine->destroy(mBackgroundIB);
        if (mWebcamTexture) mEngine->destroy(mWebcamTexture);

        // Destroy trail markers
        for (auto& marker : mTrailMarkers) {
            mEngine->getRenderableManager().destroy(marker);
            EntityManager::get().destroy(marker);
        }
        mTrailMarkers.clear();
        if (mTrailMI) mEngine->destroy(mTrailMI);
        if (mTrailVB) mEngine->destroy(mTrailVB);
        if (mTrailIB) mEngine->destroy(mTrailIB);

        // Destroy GLB assets
        for (auto& obj : mGlbObjects) {
            if (obj.asset) mAssetLoader->destroyAsset(obj.asset);
            obj.asset = nullptr;
        }
        mGlbObjects.clear();

        // Destroy blue cube
        if (mCubeEntity) {
            mEngine->getRenderableManager().destroy(mCubeEntity);
            EntityManager::get().destroy(mCubeEntity);
        }
        if (mCubeMI) mEngine->destroy(mCubeMI);
        if (mCubeVB) mEngine->destroy(mCubeVB);
        if (mCubeIB) mEngine->destroy(mCubeIB);

        // Destroy ground plane
        if (mGroundEntity) {
            mEngine->getRenderableManager().destroy(mGroundEntity);
            EntityManager::get().destroy(mGroundEntity);
        }
        if (mGroundMI) mEngine->destroy(mGroundMI);
        if (mGroundVB) mEngine->destroy(mGroundVB);
        if (mGroundIB) mEngine->destroy(mGroundIB);

        // Destroy gltfio infrastructure
        if (mResourceLoader) {
            delete mResourceLoader;
            mResourceLoader = nullptr;
        }
        if (mMaterialProvider) {
            mMaterialProvider->destroyMaterials();
            delete mMaterialProvider;
        }
        if (mStbDecoder) {
            delete mStbDecoder;
        }
        if (mAssetLoader) {
            filament::gltfio::AssetLoader::destroy(&mAssetLoader);
        }

        // Destroy cone renderable before its material
        EntityManager& em = EntityManager::get();
        mEngine->getRenderableManager().destroy(mConeEntity);
        em.destroy(mConeEntity);

        mEngine->destroy(mConeMI);
        mEngine->destroy(mRedMaterial);
        mEngine->destroy(mConeIB);
        mEngine->destroy(mConeVB);

        mEngine->destroyCameraComponent(mCameraEntity);
        em.destroy(mCameraEntity);
        em.destroy(mSunLight);
        if (mIndirectLight) mEngine->destroy(mIndirectLight);
        if (mReflectionsCubemap) mEngine->destroy(mReflectionsCubemap);

        mEngine->destroy(mView);
        mEngine->destroy(mScene);
        mEngine->destroy(mRenderer);
        mEngine->destroy(mSwapChain);
        Engine::destroy(&mEngine);
    }

    if (mWindow) {
        SDL_DestroyWindow(mWindow);
    }
    SDL_Quit();
}

Material* FilamentApp::loadMaterial(const std::string& name) {
    std::vector<std::string> searchPaths = {
        name,
        "./" + name,
        "materials/" + name
    };

#ifdef _WIN32
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir(exePath);
    size_t lastSlash = exeDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        exeDir = exeDir.substr(0, lastSlash + 1);
        searchPaths.insert(searchPaths.begin(), exeDir + name);
    }
#endif

    std::ifstream matFile;
    for (const auto& path : searchPaths) {
        matFile.open(path, std::ios::binary);
        if (matFile) {
            std::cout << "Found material at: " << path << std::endl;
            break;
        }
    }

    if (!matFile) {
        std::cerr << "Could not find material: " << name << std::endl;
        return nullptr;
    }

    std::vector<uint8_t> matData(
        (std::istreambuf_iterator<char>(matFile)),
        std::istreambuf_iterator<char>()
    );
    std::cout << "Material size: " << matData.size() << " bytes" << std::endl;

    return Material::Builder()
        .package(matData.data(), matData.size())
        .build(*mEngine);
}

bool FilamentApp::init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "Failed to initialize SDL: " << SDL_GetError() << std::endl;
        return false;
    }

    mWindow = SDL_CreateWindow(
        "Filament Red Cone",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    if (!mWindow) {
        std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
        return false;
    }

    void* nativeWindow = nullptr;
#ifdef _WIN32
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(mWindow, &wmInfo);
    nativeWindow = (void*)wmInfo.info.win.window;
#else
    nativeWindow = (void*)mWindow;
#endif

    mEngine = Engine::create(Engine::Backend::VULKAN);
    if (!mEngine) {
        std::cerr << "Failed to create Filament engine" << std::endl;
        return false;
    }

    mSwapChain = mEngine->createSwapChain(nativeWindow);
    if (!mSwapChain) {
        std::cerr << "Failed to create swap chain" << std::endl;
        return false;
    }

    mRenderer = mEngine->createRenderer();

    // No frame pacing - we render only when a new webcam frame arrives
    Renderer::FrameRateOptions frameRateOptions;
    frameRateOptions.interval = 0;
    mRenderer->setFrameRateOptions(frameRateOptions);

    // Don't clear at beginFrame - the background view will fill the swap chain
    Renderer::ClearOptions clearOpts;
    clearOpts.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
    clearOpts.clear = false;
    clearOpts.discard = true;
    mRenderer->setClearOptions(clearOpts);

    mScene = mEngine->createScene();

    mView = mEngine->createView();
    mView->setScene(mScene);
    mView->setViewport({0, 0, WINDOW_WIDTH, WINDOW_HEIGHT});

    // Enable shadows and post-processing
    mView->setShadowingEnabled(true);
    mView->setPostProcessingEnabled(true);

    // TRANSLUCENT blend mode: background pixels (alpha=0) preserve webcam
    mView->setBlendMode(View::BlendMode::TRANSLUCENT);

    // PCF for sharp shadows
    mView->setShadowType(View::ShadowType::PCF);

    // Enable dithering to reduce color banding
    mView->setDithering(View::Dithering::TEMPORAL);

    // Enable screen-space reflections
    {
        View::ScreenSpaceReflectionsOptions ssr;
        ssr.enabled = mSSREnabled;
        ssr.thickness = mSSRThickness;
        ssr.bias = mSSRBias;
        ssr.maxDistance = mSSRMaxDistance;
        ssr.stride = mSSRStride;
        mView->setScreenSpaceReflectionsOptions(ssr);
    }

    // Load material
    mRedMaterial = loadMaterial("red_material.filamat");

    // Initialize gltfio for GLB model loading
    mMaterialProvider = filament::gltfio::createUbershaderProvider(
        mEngine, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);

    filament::gltfio::AssetConfiguration assetConfig = {};
    assetConfig.engine = mEngine;
    assetConfig.materials = mMaterialProvider;
    mAssetLoader = filament::gltfio::AssetLoader::create(assetConfig);

    // Get exe directory for resource path
    std::string resPath = ".";
#ifdef _WIN32
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string exeDir(exePath);
        size_t lastSlash = exeDir.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            resPath = exeDir.substr(0, lastSlash + 1);
        }
    }
#endif

    mStbDecoder = filament::gltfio::createStbProvider(mEngine);
    mResourceLoader = new filament::gltfio::ResourceLoader({mEngine, resPath.c_str(), true});
    mResourceLoader->addTextureProvider("image/png", mStbDecoder);
    mResourceLoader->addTextureProvider("image/jpeg", mStbDecoder);

    createCamera();
    createLighting();
    createCone();
    createModels();

    // Enumerate video devices and initialize webcam (device 0)
    mVideoDevices = enumerateVideoDevices();
    if (!mVideoDevices.empty()) {
        mSelectedVideoDevice = 0;
    }
    if (!mUseGradientBg) {
        initWebcam(0);
    }

    // Enumerate COM ports and auto-connect to COM5 if available
    mComPorts = enumerateComPorts();
    for (int i = 0; i < (int)mComPorts.size(); i++) {
        if (mComPorts[i] == "COM5") {
            mSelectedComPort = i;
            if (openSerialPort("COM5")) {
                mConnectedPortName = "COM5";
            }
            break;
        }
    }

    // Store main window ID for event routing
    mMainWindowID = SDL_GetWindowID(mWindow);

    // Create OpenGL-backed SDL window for ImGui sidebar
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // Query DPI scale for high-resolution displays
    float dpiScale = 1.0f;
    {
        int displayIdx = SDL_GetWindowDisplayIndex(mWindow);
        float ddpi = 0, hdpi = 0, vdpi = 0;
        if (SDL_GetDisplayDPI(displayIdx, &ddpi, &hdpi, &vdpi) == 0 && ddpi > 0) {
            dpiScale = ddpi / 96.0f;
            if (dpiScale < 1.0f) dpiScale = 1.0f;
        }
        std::cout << "Display DPI: " << ddpi << ", scale factor: " << dpiScale << std::endl;
    }

    mSidebarWidth = (int)(300 * dpiScale);

    // Create borderless GL window for docked sidebar
    int mainX, mainY;
    SDL_GetWindowPosition(mWindow, &mainX, &mainY);
    mControlWindow = SDL_CreateWindow("Scene Controls",
        mainX, mainY,
        mSidebarWidth, WINDOW_HEIGHT,
        SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!mControlWindow) {
        std::cerr << "Failed to create control window: " << SDL_GetError() << std::endl;
        return false;
    }
    mGLContext = SDL_GL_CreateContext(mControlWindow);
    if (!mGLContext) {
        std::cerr << "Failed to create GL context: " << SDL_GetError() << std::endl;
        return false;
    }
    SDL_GL_SetSwapInterval(0);
    mControlWindowID = SDL_GetWindowID(mControlWindow);

    // Make control window owned by main window (always stays on top of it)
#ifdef _WIN32
    {
        SDL_SysWMinfo ctrlWmi, mainWmi;
        SDL_VERSION(&ctrlWmi.version);
        SDL_VERSION(&mainWmi.version);
        SDL_GetWindowWMInfo(mControlWindow, &ctrlWmi);
        SDL_GetWindowWMInfo(mWindow, &mainWmi);
        SetWindowLongPtr(ctrlWmi.info.win.window, GWLP_HWNDPARENT,
            (LONG_PTR)mainWmi.info.win.window);
    }
#endif

    // Position sidebar overlapping main window
    updateSidebarLayout();

    // Initialize ImGui with SDL2 + OpenGL3 backends
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // Load a scalable TrueType font at DPI-appropriate size
    float fontSize = 18.0f * dpiScale;
    bool fontLoaded = false;
    const char* fontPaths[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
        "C:\\Windows\\Fonts\\verdana.ttf",
    };
    for (const char* path : fontPaths) {
        std::ifstream fontFile(path, std::ios::binary | std::ios::ate);
        if (!fontFile) continue;
        size_t fontDataSize = fontFile.tellg();
        fontFile.seekg(0, std::ios::beg);
        void* fontData = IM_ALLOC(fontDataSize);
        fontFile.read((char*)fontData, fontDataSize);
        fontFile.close();
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(fontData, (int)fontDataSize, fontSize);
        if (font) {
            std::cout << "Loaded font: " << path << " at " << fontSize << "px" << std::endl;
            fontLoaded = true;
            break;
        }
    }
    if (!fontLoaded) {
        io.Fonts->AddFontDefault();
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpiScale);
    style.WindowRounding = 6.0f;
    style.FrameRounding = 3.0f;

    ImGui_ImplSDL2_InitForOpenGL(mControlWindow, mGLContext);
    ImGui_ImplOpenGL3_Init("#version 130");

    // Open CSV logs for tracking analysis
    mCsvStartTime = std::chrono::steady_clock::now();
    mCsvFile = fopen("frame_log.csv", "w");
    if (mCsvFile) {
        fprintf(mCsvFile, "time_ms,new_webcam,rendered,delta_ms,raw_yaw,smooth_yaw,yaw_fil_rad,raw_pitch,smooth_pitch,raw_x,raw_y,raw_z,smooth_x,smooth_y,smooth_z\n");
    }
    mCsvSerialFile = fopen("serial_log.csv", "w");
    if (mCsvSerialFile) {
        fprintf(mCsvSerialFile, "serial_utc_us,last_video_utc_us,vips_ms,yaw,pitch,roll,x,y,z\n");
    }

    return true;
}

void FilamentApp::createCamera() {
    EntityManager& em = EntityManager::get();
    mCameraEntity = em.create();

    auto& tcm = mEngine->getTransformManager();
    tcm.create(mCameraEntity);

    mCamera = mEngine->createCamera(mCameraEntity);
    mView->setCamera(mCamera);

    const float aspect = (float)WINDOW_WIDTH / (float)WINDOW_HEIGHT;
    mCamera->setProjection(45.0f, aspect, 0.1f, 500.0f);

    // EV100 = log2(aperture² / shutterSpeed * 100/ISO)
    // With f/16, ISO 100: shutterSpeed = 256 / 2^EV100
    float shutterSpeed = 256.0f / std::pow(2.0f, mExposure);
    mCamera->setExposure(16.0f, shutterSpeed, 100.0f);

    mCamera->lookAt(
        {5.0f, 4.0f, 5.0f},
        {0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}
    );
}

void FilamentApp::updateSidebarLayout() {
    int mainW, mainH;
    SDL_GetWindowSize(mWindow, &mainW, &mainH);
    int mainX, mainY;
    SDL_GetWindowPosition(mWindow, &mainX, &mainY);

#ifdef _WIN32
    // Update window style based on docked/floating state
    SDL_SysWMinfo ctrlWmi;
    SDL_VERSION(&ctrlWmi.version);
    SDL_GetWindowWMInfo(mControlWindow, &ctrlWmi);
    HWND ctrlHwnd = ctrlWmi.info.win.window;

    if (mSidebarDocked) {
        // Borderless for docked mode
        LONG style = GetWindowLong(ctrlHwnd, GWL_STYLE);
        style = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU)) | WS_POPUP;
        SetWindowLong(ctrlHwnd, GWL_STYLE, style);
        SetWindowPos(ctrlHwnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    } else {
        // Title bar + resize for floating mode
        LONG style = GetWindowLong(ctrlHwnd, GWL_STYLE);
        style = (style & ~WS_POPUP) | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
        SetWindowLong(ctrlHwnd, GWL_STYLE, style);
        SetWindowPos(ctrlHwnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
#endif

    if (mSidebarDocked && mSidebarVisible) {
        // Docked: sidebar overlays left edge of main window, views stay full size
        SDL_SetWindowPosition(mControlWindow, mainX, mainY);
        SDL_SetWindowSize(mControlWindow, mSidebarWidth, mainH);
        SDL_ShowWindow(mControlWindow);
        SDL_RaiseWindow(mControlWindow);

        mView->setViewport({0, 0, (uint32_t)mainW, (uint32_t)mainH});
        mCamera->setProjection(45.0f, (float)mainW / (float)mainH, 0.1f, 500.0f);
        if (mBackgroundView) {
            mBackgroundView->setViewport({0, 0, (uint32_t)mainW, (uint32_t)mainH});
        }
    } else if (!mSidebarDocked && mSidebarVisible) {
        // Floating: position to the left of main window on first switch
        SDL_ShowWindow(mControlWindow);

        mView->setViewport({0, 0, (uint32_t)mainW, (uint32_t)mainH});
        mCamera->setProjection(45.0f, (float)mainW / (float)mainH, 0.1f, 500.0f);
        if (mBackgroundView) {
            mBackgroundView->setViewport({0, 0, (uint32_t)mainW, (uint32_t)mainH});
        }
    } else {
        // Hidden
        SDL_HideWindow(mControlWindow);

        mView->setViewport({0, 0, (uint32_t)mainW, (uint32_t)mainH});
        mCamera->setProjection(45.0f, (float)mainW / (float)mainH, 0.1f, 500.0f);
        if (mBackgroundView) {
            mBackgroundView->setViewport({0, 0, (uint32_t)mainW, (uint32_t)mainH});
        }
    }

    // Rebuild distortion pipeline on layout change if calibration is loaded
    if (mDistortionCalibLoaded && mDistortionEnabled) {
        teardownDistortion();
        generateDistortionUVMap();
        setupDistortion();
    }
}

void FilamentApp::createLighting() {
    EntityManager& em = EntityManager::get();

    mSunLight = em.create();

    LightManager::ShadowOptions shadowOpts;
    shadowOpts.mapSize = 4096;
    shadowOpts.shadowFar = 30.0f;
    shadowOpts.stable = true;

    LightManager::Builder(LightManager::Type::SUN)
        .color(Color::toLinear<ACCURATE>({1.0f, 0.98f, 0.95f}))
        .intensity(150000.0f)
        .direction({-0.3f, -1.0f, 0.2f})
        .sunAngularRadius(3.0f)
        .castShadows(true)
        .shadowOptions(shadowOpts)
        .build(*mEngine, mSunLight);

    mScene->addEntity(mSunLight);

    // Ambient light
    static float3 sh[9] = {
        {0.8f, 0.85f, 1.0f},
        {0.0f, 0.05f, 0.1f},
        {0.3f, 0.3f, 0.35f},
        {0.0f, 0.0f, 0.0f},
        {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    };

    // Create reflections cubemap texture (pixels filled by regenerateCubemap)
    const uint32_t cubeDim = 128;
    mReflectionsCubemap = Texture::Builder()
        .width(cubeDim)
        .height(cubeDim)
        .levels(0xFF)
        .format(Texture::InternalFormat::RGBA16F)
        .sampler(Texture::Sampler::SAMPLER_CUBEMAP)
        .usage(Texture::Usage::DEFAULT | Texture::Usage::COLOR_ATTACHMENT
             | Texture::Usage::GEN_MIPMAPPABLE)
        .build(*mEngine);

    regenerateCubemap();

    mIndirectLight = IndirectLight::Builder()
        .irradiance(3, sh)
        .reflections(mReflectionsCubemap)
        .intensity(5000.0f)
        .build(*mEngine);
    mScene->setIndirectLight(mIndirectLight);
}

void FilamentApp::regenerateCubemap() {
    if (!mReflectionsCubemap) return;

    const uint32_t cubeDim = 128;
    size_t facePixels = cubeDim * cubeDim;
    size_t faceSize = facePixels * 4 * sizeof(uint16_t);
    size_t totalSize = faceSize * 6;
    uint16_t* allPixels = new uint16_t[facePixels * 4 * 6];

    auto toHalf = [](float f) -> uint16_t {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        uint32_t sign = (bits >> 16) & 0x8000;
        int32_t exp = ((bits >> 23) & 0xFF) - 127 + 15;
        uint32_t mant = bits & 0x7FFFFF;
        if (exp <= 0) return (uint16_t)sign;
        if (exp >= 31) return (uint16_t)(sign | 0x7C00);
        return (uint16_t)(sign | (exp << 10) | (mant >> 13));
    };

    auto cubeDir = [](int face, float u, float v) -> float3 {
        switch (face) {
            case 0: return normalize(float3{ 1.0f, -v,   -u});
            case 1: return normalize(float3{-1.0f, -v,    u});
            case 2: return normalize(float3{    u,  1.0f,  v});
            case 3: return normalize(float3{    u, -1.0f, -v});
            case 4: return normalize(float3{    u, -v,  1.0f});
            case 5: return normalize(float3{   -u, -v, -1.0f});
            default: return {0, 1, 0};
        }
    };

    const float gnd = mEnvGroundBrightness;
    const float sky = mEnvSkyBrightness;
    const float hor = mEnvHorizonBrightness;
    const float lit = mEnvLightIntensity;

    for (int face = 0; face < 6; face++) {
        uint16_t* facePtr = allPixels + face * facePixels * 4;
        for (uint32_t y = 0; y < cubeDim; y++) {
            for (uint32_t x = 0; x < cubeDim; x++) {
                float u = 2.0f * ((float)x + 0.5f) / cubeDim - 1.0f;
                float v = 2.0f * ((float)y + 0.5f) / cubeDim - 1.0f;
                float3 dir = cubeDir(face, u, v);

                float r, g, b;

                if (dir.y < -0.01f) {
                    float t = std::min(1.0f, -dir.y * 3.0f);
                    float xz = dir.x / -dir.y;
                    float zz = dir.z / -dir.y;
                    float checker = (fmodf(fabsf(xz * 2.0f), 2.0f) < 1.0f) !=
                                    (fmodf(fabsf(zz * 2.0f), 2.0f) < 1.0f) ? 1.2f : 0.8f;
                    r = g = b = checker * t * gnd;
                } else if (dir.y < 0.08f) {
                    float horizon = std::max(0.0f, 1.0f - fabsf(dir.y) * 12.5f);
                    horizon = horizon * horizon;
                    r = (1.5f + 6.0f * horizon) * hor;
                    g = (1.55f + 6.0f * horizon) * hor;
                    b = (1.6f + 6.0f * horizon) * hor;
                } else {
                    float elevation = dir.y;
                    float skyT = std::min(1.0f, elevation * 2.0f);
                    float base = 2.0f + 3.0f * (1.0f - skyT);
                    // Sky blue tint: reduce red, keep green, boost blue
                    float tint = (0.3f + 0.7f * (1.0f - skyT)) * mEnvSkyBlueness;
                    r = (base - tint * 0.6f) * sky;
                    g = (base + tint * 0.1f) * sky;
                    b = (base + tint * 0.5f) * sky;
                    if (r < 0.0f) r = 0.0f;
                }

                // Studio light 1: key light above-front-right
                {
                    float3 ld = normalize(float3{0.5f, 0.7f, -0.3f});
                    float d = dot(dir, ld);
                    if (d > 0.85f) {
                        float i = (d - 0.85f) / 0.15f;
                        i = i * i * 20.0f * lit;
                        r += i; g += i; b += i;
                    }
                }
                // Studio light 2: fill light from left
                {
                    float3 ld = normalize(float3{-0.6f, 0.5f, 0.2f});
                    float d = dot(dir, ld);
                    if (d > 0.85f) {
                        float i = (d - 0.85f) / 0.15f;
                        i = i * i * 15.0f * lit;
                        r += i * 0.9f; g += i * 0.92f; b += i;
                    }
                }
                // Studio light 3: rim light from behind
                {
                    float3 ld = normalize(float3{0.0f, 0.3f, 0.8f});
                    float d = dot(dir, ld);
                    if (d > 0.87f) {
                        float i = (d - 0.87f) / 0.13f;
                        i = i * i * 16.0f * lit;
                        r += i; g += i * 0.95f; b += i * 0.9f;
                    }
                }
                // Studio light 4: broad overhead softbox
                {
                    float3 ld = normalize(float3{0.0f, 1.0f, 0.0f});
                    float d = dot(dir, ld);
                    if (d > 0.75f) {
                        float i = (d - 0.75f) / 0.25f;
                        i = i * i * 8.0f * lit;
                        r += i; g += i; b += i;
                    }
                }

                size_t idx = (y * cubeDim + x) * 4;
                facePtr[idx + 0] = toHalf(r);
                facePtr[idx + 1] = toHalf(g);
                facePtr[idx + 2] = toHalf(b);
                facePtr[idx + 3] = toHalf(1.0f);
            }
        }
    }

    Texture::FaceOffsets offsets;
    for (int i = 0; i < 6; i++) offsets[i] = i * faceSize;

    Texture::PixelBufferDescriptor pbd(
        allPixels, totalSize,
        Texture::Format::RGBA, Texture::Type::HALF,
        [](void* buf, size_t, void*) { delete[] static_cast<uint16_t*>(buf); }
    );
    mReflectionsCubemap->setImage(*mEngine, 0, std::move(pbd), offsets);
    mReflectionsCubemap->generateMipmaps(*mEngine);
}

bool FilamentApp::loadHdrEnvironment(const std::string& path) {
    if (!mReflectionsCubemap || !mEngine) return false;

    int hdrW = 0, hdrH = 0, hdrC = 0;
    float* hdrData = stbi_loadf(path.c_str(), &hdrW, &hdrH, &hdrC, 3);
    if (!hdrData) {
        std::cerr << "Failed to load HDR: " << path << std::endl;
        return false;
    }
    std::cout << "Loaded HDR: " << path << " (" << hdrW << "x" << hdrH << ", " << hdrC << " channels)" << std::endl;

    // Destroy old cubemap and create new one at appropriate resolution
    // Use cubemap face size = hdrHeight / 2 (common for equirectangular), clamped to reasonable range
    uint32_t cubeDim = std::max(64u, std::min(2048u, (uint32_t)(hdrH / 2)));
    // Round to nearest power of 2
    uint32_t pot = 64;
    while (pot < cubeDim) pot *= 2;
    cubeDim = pot;

    // If existing cubemap is different size, recreate it
    if (mReflectionsCubemap->getWidth() != cubeDim) {
        mEngine->destroy(mReflectionsCubemap);
        mReflectionsCubemap = Texture::Builder()
            .width(cubeDim)
            .height(cubeDim)
            .levels(0xFF)
            .format(Texture::InternalFormat::RGBA16F)
            .sampler(Texture::Sampler::SAMPLER_CUBEMAP)
            .usage(Texture::Usage::DEFAULT | Texture::Usage::COLOR_ATTACHMENT
                 | Texture::Usage::GEN_MIPMAPPABLE)
            .build(*mEngine);

        // Update indirect light to use new cubemap
        if (mIndirectLight) mEngine->destroy(mIndirectLight);
        static float3 sh[9] = {
            {0.8f, 0.85f, 1.0f},
            {0.0f, 0.05f, 0.1f},
            {0.3f, 0.3f, 0.35f},
            {0.0f, 0.0f, 0.0f},
            {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
        };
        mIndirectLight = IndirectLight::Builder()
            .irradiance(3, sh)
            .reflections(mReflectionsCubemap)
            .intensity(mIBLIntensity)
            .build(*mEngine);
        mScene->setIndirectLight(mIndirectLight);
    }

    auto toHalf = [](float f) -> uint16_t {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        uint32_t sign = (bits >> 16) & 0x8000;
        int32_t exp = ((bits >> 23) & 0xFF) - 127 + 15;
        uint32_t mant = bits & 0x7FFFFF;
        if (exp <= 0) return (uint16_t)sign;
        if (exp >= 31) return (uint16_t)(sign | 0x7C00);
        return (uint16_t)(sign | (exp << 10) | (mant >> 13));
    };

    // Convert equirectangular to cubemap faces
    // Cubemap face directions: +X, -X, +Y, -Y, +Z, -Z
    size_t facePixels = (size_t)cubeDim * cubeDim;
    size_t faceSize = facePixels * 4 * sizeof(uint16_t);
    size_t totalSize = faceSize * 6;
    uint16_t* allPixels = new uint16_t[facePixels * 4 * 6];

    auto cubeDir = [](int face, float u, float v) -> float3 {
        switch (face) {
            case 0: return normalize(float3{ 1.0f, -v,   -u});  // +X
            case 1: return normalize(float3{-1.0f, -v,    u});  // -X
            case 2: return normalize(float3{    u,  1.0f,  v});  // +Y
            case 3: return normalize(float3{    u, -1.0f, -v});  // -Y
            case 4: return normalize(float3{    u, -v,  1.0f});  // +Z
            case 5: return normalize(float3{   -u, -v, -1.0f});  // -Z
            default: return {0, 1, 0};
        }
    };

    const float PI = 3.14159265358979f;

    for (int face = 0; face < 6; face++) {
        uint16_t* facePtr = allPixels + face * facePixels * 4;
        for (uint32_t y = 0; y < cubeDim; y++) {
            for (uint32_t x = 0; x < cubeDim; x++) {
                float u = 2.0f * ((float)x + 0.5f) / cubeDim - 1.0f;
                float v = 2.0f * ((float)y + 0.5f) / cubeDim - 1.0f;
                float3 dir = cubeDir(face, u, v);

                // Convert direction to equirectangular UV
                float theta = atan2f(dir.x, dir.z);  // longitude [-PI, PI]
                float phi = asinf(std::max(-1.0f, std::min(1.0f, dir.y)));  // latitude [-PI/2, PI/2]

                float eqU = (theta / PI + 1.0f) * 0.5f;  // [0, 1]
                float eqV = 0.5f - phi / PI;              // [0, 1], top=0

                // Bilinear sample from HDR image
                float fx = eqU * hdrW - 0.5f;
                float fy = eqV * hdrH - 0.5f;
                int ix0 = (int)floorf(fx);
                int iy0 = (int)floorf(fy);
                float fracX = fx - ix0;
                float fracY = fy - iy0;

                auto sampleHdr = [&](int sx, int sy) -> float3 {
                    sx = ((sx % hdrW) + hdrW) % hdrW;
                    sy = std::max(0, std::min(hdrH - 1, sy));
                    const float* p = hdrData + (sy * hdrW + sx) * 3;
                    return {p[0], p[1], p[2]};
                };

                float3 s00 = sampleHdr(ix0, iy0);
                float3 s10 = sampleHdr(ix0 + 1, iy0);
                float3 s01 = sampleHdr(ix0, iy0 + 1);
                float3 s11 = sampleHdr(ix0 + 1, iy0 + 1);
                float3 color = s00 * (1 - fracX) * (1 - fracY) +
                               s10 * fracX * (1 - fracY) +
                               s01 * (1 - fracX) * fracY +
                               s11 * fracX * fracY;

                size_t idx = (y * cubeDim + x) * 4;
                facePtr[idx + 0] = toHalf(color.x);
                facePtr[idx + 1] = toHalf(color.y);
                facePtr[idx + 2] = toHalf(color.z);
                facePtr[idx + 3] = toHalf(1.0f);
            }
        }
    }

    stbi_image_free(hdrData);

    Texture::FaceOffsets offsets;
    for (int i = 0; i < 6; i++) offsets[i] = i * faceSize;

    Texture::PixelBufferDescriptor pbd(
        allPixels, totalSize,
        Texture::Format::RGBA, Texture::Type::HALF,
        [](void* buf, size_t, void*) { delete[] static_cast<uint16_t*>(buf); }
    );
    mReflectionsCubemap->setImage(*mEngine, 0, std::move(pbd), offsets);
    mReflectionsCubemap->generateMipmaps(*mEngine);

    mUseHdrEnv = true;
    mHdrEnvPath = path;
    std::cout << "HDR environment loaded: " << cubeDim << "x" << cubeDim << " cubemap" << std::endl;
    return true;
}

void FilamentApp::clearHdrEnvironment() {
    mUseHdrEnv = false;
    mHdrEnvPath.clear();

    // If cubemap was resized for HDR, recreate at default 128
    if (mReflectionsCubemap && mReflectionsCubemap->getWidth() != 128) {
        mEngine->destroy(mReflectionsCubemap);
        mReflectionsCubemap = Texture::Builder()
            .width(128)
            .height(128)
            .levels(0xFF)
            .format(Texture::InternalFormat::RGBA16F)
            .sampler(Texture::Sampler::SAMPLER_CUBEMAP)
            .usage(Texture::Usage::DEFAULT | Texture::Usage::COLOR_ATTACHMENT
                 | Texture::Usage::GEN_MIPMAPPABLE)
            .build(*mEngine);

        if (mIndirectLight) mEngine->destroy(mIndirectLight);
        static float3 sh[9] = {
            {0.8f, 0.85f, 1.0f},
            {0.0f, 0.05f, 0.1f},
            {0.3f, 0.3f, 0.35f},
            {0.0f, 0.0f, 0.0f},
            {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
        };
        mIndirectLight = IndirectLight::Builder()
            .irradiance(3, sh)
            .reflections(mReflectionsCubemap)
            .intensity(mIBLIntensity)
            .build(*mEngine);
        mScene->setIndirectLight(mIndirectLight);
    }

    regenerateCubemap();
}

void FilamentApp::createCone() {
    ConeGenerator::generate(0.33f, 1.0f, 64, mConeVertices, mConeIndices);
    std::cout << "Generated cone: " << mConeVertices.size() << " vertices, "
              << mConeIndices.size() << " indices" << std::endl;

    mConeVB = VertexBuffer::Builder()
        .vertexCount(static_cast<uint32_t>(mConeVertices.size()))
        .bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3,
                   offsetof(Vertex, position), sizeof(Vertex))
        .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::SHORT4,
                   offsetof(Vertex, tangents), sizeof(Vertex))
        .normalized(VertexAttribute::TANGENTS)
        .build(*mEngine);

    mConeVB->setBufferAt(*mEngine, 0,
        VertexBuffer::BufferDescriptor(mConeVertices.data(),
            mConeVertices.size() * sizeof(Vertex)));

    mConeIB = IndexBuffer::Builder()
        .indexCount(static_cast<uint32_t>(mConeIndices.size()))
        .bufferType(IndexBuffer::IndexType::USHORT)
        .build(*mEngine);

    mConeIB->setBuffer(*mEngine,
        IndexBuffer::BufferDescriptor(mConeIndices.data(),
            mConeIndices.size() * sizeof(uint16_t)));

    if (mRedMaterial) {
        mConeMI = mRedMaterial->createInstance();
        mConeMI->setParameter("baseColor", RgbaType::sRGB, {0.85f, 0.15f, 0.15f, 1.0f});
        mConeMI->setParameter("metallic", 0.0f);
        mConeMI->setParameter("roughness", 0.35f);
    }

    EntityManager& em = EntityManager::get();
    mConeEntity = em.create();

    RenderableManager::Builder(1)
        .boundingBox({{-0.33f, 0.0f, -0.33f}, {0.33f, 1.0f, 0.33f}})
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                  mConeVB, mConeIB, 0, static_cast<uint32_t>(mConeIndices.size()))
        .material(0, mConeMI)
        .culling(false)
        .receiveShadows(true)
        .castShadows(true)
        .build(*mEngine, mConeEntity);

    mScene->addEntity(mConeEntity);

    auto& tcm = mEngine->getTransformManager();
    tcm.create(mConeEntity);

    mConePos = {0.0f, 1.0f, 0.0f};
    mConeRot[0] = 2;  // 180 on X (flip upside down)
    mConeRot[1] = 0;
    mConeRot[2] = 0;
    updateConeTransform();

    std::cout << "Cone entity added to scene" << std::endl;
}

void FilamentApp::createModels() {
    // Try loading scene.json from exe directory; fall back to hardcoded defaults
    std::string scenePath = resolveFilePath("scene.json");
    if (!scenePath.empty()) {
        std::cout << "Loading startup scene from " << scenePath << std::endl;
        loadScene("scene.json");
    } else {
        std::cout << "No scene.json found, using defaults" << std::endl;
        {
            SceneObject logo;
            logo.name = "Logo";
            logo.glbFilename = "AirPixel_logo.glb";
            logo.position = {0.0f, 1.5f, -1.86f};
            logo.scale = 16.5f;
            logo.yRotation = 0.0f;
            logo.rot90[0] = 1;
            logo.rot90[1] = -1;
            logo.rot90[2] = 0;
            logo.aabbCenterOffset = {0.0564f, 0.0f, -0.0184f};
            logo.visible = true;
            loadGlbObject(logo);
            mGlbObjects.push_back(std::move(logo));
        }

        {
            SceneObject race;
            race.name = "Racelogic";
            race.glbFilename = "RACELOGIC.glb";
            race.position = {2.0f, 0.0f, 0.0f};
            race.scale = 1.0f;
            race.visible = true;
            loadGlbObject(race);
            mGlbObjects.push_back(std::move(race));
        }
    }

    // Create procedural blue cube
    if (mRedMaterial) {
        static Vertex cubeVerts[8] = {
            {{-0.5f, -0.5f, -0.5f}, {0, 0, 0, 32767}},
            {{ 0.5f, -0.5f, -0.5f}, {0, 0, 0, 32767}},
            {{ 0.5f,  0.5f, -0.5f}, {0, 0, 0, 32767}},
            {{-0.5f,  0.5f, -0.5f}, {0, 0, 0, 32767}},
            {{-0.5f, -0.5f,  0.5f}, {0, 0, 0, 32767}},
            {{ 0.5f, -0.5f,  0.5f}, {0, 0, 0, 32767}},
            {{ 0.5f,  0.5f,  0.5f}, {0, 0, 0, 32767}},
            {{-0.5f,  0.5f,  0.5f}, {0, 0, 0, 32767}},
        };

        static uint16_t cubeIndices[36] = {
            4, 5, 6,  4, 6, 7,
            1, 0, 3,  1, 3, 2,
            3, 7, 6,  3, 6, 2,
            0, 1, 5,  0, 5, 4,
            1, 2, 6,  1, 6, 5,
            0, 4, 7,  0, 7, 3,
        };

        mCubeVB = VertexBuffer::Builder()
            .vertexCount(8)
            .bufferCount(1)
            .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3,
                       offsetof(Vertex, position), sizeof(Vertex))
            .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::SHORT4,
                       offsetof(Vertex, tangents), sizeof(Vertex))
            .normalized(VertexAttribute::TANGENTS)
            .build(*mEngine);

        mCubeVB->setBufferAt(*mEngine, 0,
            VertexBuffer::BufferDescriptor(cubeVerts, sizeof(cubeVerts)));

        mCubeIB = IndexBuffer::Builder()
            .indexCount(36)
            .bufferType(IndexBuffer::IndexType::USHORT)
            .build(*mEngine);

        mCubeIB->setBuffer(*mEngine,
            IndexBuffer::BufferDescriptor(cubeIndices, sizeof(cubeIndices)));

        mCubeMI = mRedMaterial->createInstance();
        mCubeMI->setParameter("baseColor", RgbaType::sRGB, {0.15f, 0.25f, 0.85f, 1.0f});
        mCubeMI->setParameter("metallic", 0.0f);
        mCubeMI->setParameter("roughness", 0.4f);

        EntityManager& em = EntityManager::get();
        mCubeEntity = em.create();

        RenderableManager::Builder(1)
            .boundingBox({{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}})
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                      mCubeVB, mCubeIB, 0, 36)
            .material(0, mCubeMI)
            .culling(false)
            .receiveShadows(true)
            .castShadows(true)
            .build(*mEngine, mCubeEntity);

        auto& tcm = mEngine->getTransformManager();
        tcm.create(mCubeEntity);
        auto ti = tcm.getInstance(mCubeEntity);
        mat4f transform = mat4f::translation(float3{0.0f, 0.5f, -2.0f});
        tcm.setTransform(ti, transform);

        std::cout << "Blue cube added to scene" << std::endl;
    }

    // Create shadow ground plane
    {
        Material* shadowMat = loadMaterial("shadow_plane.filamat");
        if (!shadowMat) {
            std::cerr << "Failed to load shadow_plane material" << std::endl;
        }
    if (shadowMat) {
        const float GROUND_Y = 0.0f;
        const float HALF_SIZE = 100.0f;

        static Vertex groundVerts[4] = {
            {{-HALF_SIZE, 0.0f, -HALF_SIZE}, {0, 0, 0, 32767}},
            {{ HALF_SIZE, 0.0f, -HALF_SIZE}, {0, 0, 0, 32767}},
            {{ HALF_SIZE, 0.0f,  HALF_SIZE}, {0, 0, 0, 32767}},
            {{-HALF_SIZE, 0.0f,  HALF_SIZE}, {0, 0, 0, 32767}},
        };

        static uint16_t groundIndices[6] = {
            0, 2, 1,  0, 3, 2
        };

        mGroundVB = VertexBuffer::Builder()
            .vertexCount(4)
            .bufferCount(1)
            .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3,
                       offsetof(Vertex, position), sizeof(Vertex))
            .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::SHORT4,
                       offsetof(Vertex, tangents), sizeof(Vertex))
            .normalized(VertexAttribute::TANGENTS)
            .build(*mEngine);

        mGroundVB->setBufferAt(*mEngine, 0,
            VertexBuffer::BufferDescriptor(groundVerts, sizeof(groundVerts)));

        mGroundIB = IndexBuffer::Builder()
            .indexCount(6)
            .bufferType(IndexBuffer::IndexType::USHORT)
            .build(*mEngine);

        mGroundIB->setBuffer(*mEngine,
            IndexBuffer::BufferDescriptor(groundIndices, sizeof(groundIndices)));

        mGroundMI = shadowMat->createInstance();
        mGroundMI->setParameter("shadowStrength", 0.5f);

        EntityManager& em = EntityManager::get();
        mGroundEntity = em.create();

        RenderableManager::Builder(1)
            .boundingBox({{-HALF_SIZE, -0.1f, -HALF_SIZE}, {HALF_SIZE, 0.1f, HALF_SIZE}})
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                      mGroundVB, mGroundIB, 0, 6)
            .material(0, mGroundMI)
            .culling(false)
            .receiveShadows(true)
            .castShadows(false)
            .build(*mEngine, mGroundEntity);

        mScene->addEntity(mGroundEntity);

        auto& tcm = mEngine->getTransformManager();
        tcm.create(mGroundEntity);
        auto ti = tcm.getInstance(mGroundEntity);
        tcm.setTransform(ti, mat4f::translation(float3{0.0f, GROUND_Y, 0.0f}));

        std::cout << "Ground plane added at Y=" << GROUND_Y << std::endl;
    }
    }
}

void FilamentApp::updateConeTransform() {
    auto& tcm = mEngine->getTransformManager();
    auto ti = tcm.getInstance(mConeEntity);
    if (!ti) return;
    const float HALF_PI = 3.14159265358979f * 0.5f;
    mat4f transform = mat4f::translation(mConePos) *
                      mat4f::rotation(mConeRot[1] * HALF_PI, float3{0, 1, 0}) *
                      mat4f::rotation(mConeRot[0] * HALF_PI, float3{1, 0, 0}) *
                      mat4f::rotation(mConeRot[2] * HALF_PI, float3{0, 0, 1}) *
                      mat4f::scaling(float3{mConeScale, mConeScale, mConeScale});
    tcm.setTransform(ti, transform);
}

void FilamentApp::update(float deltaTime, bool newWebcamFrame) {
    bool trackingActive = false;
    {
        std::lock_guard<std::mutex> lock(mTrackingMutex);
        trackingActive = mTrackingData.valid;
    }

    if (trackingActive) {
        updateSceneFromTracking(newWebcamFrame);
    } else {
        if (mAutoOrbit && !mMouseDragging) {
            mCameraYaw += deltaTime * 0.6f;
        }

        float camX = mCameraTarget.x + mCameraDistance * std::cos(mCameraPitch) * std::sin(mCameraYaw);
        float camY = mCameraTarget.y + mCameraDistance * std::sin(mCameraPitch);
        float camZ = mCameraTarget.z + mCameraDistance * std::cos(mCameraPitch) * std::cos(mCameraYaw);

        mCamera->lookAt(
            {camX, camY, camZ},
            mCameraTarget,
            {0.0f, 1.0f, 0.0f}
        );
    }

    // Update light direction
    if (mSunLight) {
        float lightDirX = std::cos(mLightPitch) * std::sin(mLightYaw);
        float lightDirY = std::sin(mLightPitch);
        float lightDirZ = std::cos(mLightPitch) * std::cos(mLightYaw);

        auto& lm = mEngine->getLightManager();
        auto li = lm.getInstance(mSunLight);
        lm.setDirection(li, {lightDirX, lightDirY, lightDirZ});
    }
}

bool FilamentApp::render() {
    // Double-buffered offscreen render for recording
    if (mRecording && mRecordRT[0] && mRecordBgView && mRecordMainView) {
        int cur = mRecordFrameIndex & 1;
        mRecordBgView->setRenderTarget(mRecordRT[cur]);
        mRecordMainView->setRenderTarget(mRecordRT[cur]);
        mRenderer->renderStandaloneView(mRecordBgView);
        mRenderer->renderStandaloneView(mRecordMainView);
    }

    // When distortion is enabled, render the 3D scene to offscreen RT first
    if (mDistortionEnabled && mDistortionSetup && mDistortionRT && mDistortionOffscreenView) {
        // Temporarily enable clear so the RT starts at transparent black (0,0,0,0).
        Renderer::ClearOptions prevClear;
        prevClear.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
        prevClear.clear = false;
        prevClear.discard = true;

        Renderer::ClearOptions distClear;
        distClear.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
        distClear.clear = true;
        distClear.discard = true;
        mRenderer->setClearOptions(distClear);

        // Temporarily widen camera projection to render oversized FOV.
        // The UV map was remapped so [0,1] covers the overscan range.
        // The projection matrix is scaled/shifted so NDC [-1,1] maps to [minU,maxU] x [minV,maxV].
        auto origProj = mCamera->getProjectionMatrix();
        double camNear = mCamera->getNear();
        double camFar = mCamera->getCullingFar();

        double sx = mDistortionOverscanMaxU - mDistortionOverscanMinU;
        double sy = mDistortionOverscanMaxV - mDistortionOverscanMinV;
        double ox = mDistortionOverscanMinU + mDistortionOverscanMaxU - 1.0;
        double oy = mDistortionOverscanMinV + mDistortionOverscanMaxV - 1.0;

        // S maps original NDC to oversized NDC:
        // ndc_new = (ndc_old - offset) / scale
        filament::math::mat4 S(
            filament::math::double4(1.0/sx, 0, 0, 0),
            filament::math::double4(0, 1.0/sy, 0, 0),
            filament::math::double4(0, 0, 1, 0),
            filament::math::double4(-ox/sx, -oy/sy, 0, 1));

        mCamera->setCustomProjection(S * origProj, camNear, camFar);

        mRenderer->renderStandaloneView(mDistortionOffscreenView);

        // Restore camera projection and clear options
        mCamera->setCustomProjection(origProj, camNear, camFar);
        mRenderer->setClearOptions(prevClear);
    }

    if (mRenderer->beginFrame(mSwapChain)) {
        if (mBackgroundView) {
            mRenderer->render(mBackgroundView);
        }

        if (mDistortionEnabled && mDistortionSetup && mDistortionView) {
            // Render distortion quad (samples offscreen RT via UV map) instead of mView
            mRenderer->render(mDistortionView);
        } else {
            mRenderer->render(mView);
        }

        // Read from PREVIOUS frame's offscreen RT
        if (mRecording && mRecordFrameIndex > 0) {
            auto now = std::chrono::steady_clock::now();
            if (now >= mRecordNextCapture) {
                int prev = 1 - (mRecordFrameIndex & 1);
                size_t bufSize = (size_t)mRecordWidth * mRecordHeight * 4;
                uint8_t* buf = new uint8_t[bufSize];
                auto pbd = backend::PixelBufferDescriptor::make(
                    buf, bufSize,
                    backend::PixelBufferDescriptor::PixelDataFormat::RGBA,
                    backend::PixelBufferDescriptor::PixelDataType::UBYTE,
                    [this](void* buffer, size_t) {
                        std::lock_guard<std::mutex> lock(mRecordMutex);
                        mRecordQueue.push(static_cast<uint8_t*>(buffer));
                        mRecordCV.notify_one();
                    });
                mRenderer->readPixels(mRecordRT[prev],
                    0, 0, mRecordWidth, mRecordHeight, std::move(pbd));
                int fps = mWebcamFps > 0 ? mWebcamFps : 30;
                auto interval = std::chrono::microseconds(1000000 / fps);
                mRecordNextCapture += interval;
                if (mRecordNextCapture < now) {
                    mRecordNextCapture = now + interval;
                }
            }
        }

        mRenderer->endFrame();
        if (mRecording) mRecordFrameIndex++;
        return true;
    }
    return false;
}

void FilamentApp::run() {
    Uint64 lastTime = SDL_GetPerformanceCounter();
    Uint64 frequency = SDL_GetPerformanceFrequency();

    std::cout << "Controls:" << std::endl;
    std::cout << "  Left mouse drag: Orbit camera" << std::endl;
    std::cout << "  Right mouse drag: Move light source" << std::endl;
    std::cout << "  Mouse wheel: Zoom in/out" << std::endl;
    std::cout << "  ESC: Exit" << std::endl;

    while (mRunning) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // Intercept Tab before ImGui to toggle sidebar
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_TAB) {
                mSidebarVisible = !mSidebarVisible;
                updateSidebarLayout();
                std::cout << "Sidebar: " << (mSidebarVisible ? "shown" : "hidden") << std::endl;
                continue;
            }

            ImGui_ImplSDL2_ProcessEvent(&event);

            switch (event.type) {
                case SDL_QUIT:
                    mRunning = false;
                    break;
                case SDL_KEYDOWN:
                    if (event.key.windowID != mMainWindowID) break;
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        mRunning = false;
                    } else if ((event.key.keysym.mod & KMOD_SHIFT) &&
                               event.key.keysym.sym == SDLK_r) {
                        if (!mGlbObjects.empty() && mGlbObjects[0].asset) {
                            auto& obj = mGlbObjects[0];
                            auto camMat = mCamera->getModelMatrix();
                            float3 camPos = {(float)camMat[3][0], (float)camMat[3][1], (float)camMat[3][2]};
                            float3 camFwd = {(float)-camMat[2][0], (float)-camMat[2][1], (float)-camMat[2][2]};
                            obj.position = camPos + camFwd * 2.0f;
                            float3 toCamera = -camFwd;
                            obj.yRotation = atan2f(toCamera.x, toCamera.z);
                            updateObjectTransform(obj);
                            std::cout << "Object reset to 2m in front: (" << obj.position.x << ", " << obj.position.y << ", " << obj.position.z << ")" << std::endl;
                        }
                    } else if (event.key.keysym.sym == SDLK_r) {
                        auto camMat = mCamera->getModelMatrix();
                        float3 camPos = {(float)camMat[3][0], (float)camMat[3][1], (float)camMat[3][2]};
                        float3 camFwd = {(float)-camMat[2][0], (float)-camMat[2][1], (float)-camMat[2][2]};
                        mConePos = camPos + camFwd * 1.0f;
                        mConePos.y += 1.0f;
                        mConeRot[0] = 2; mConeRot[1] = 0; mConeRot[2] = 0;
                        updateConeTransform();
                        std::cout << "Cone reset to 1m in front: (" << mConePos.x << ", " << mConePos.y << ", " << mConePos.z << ")" << std::endl;
                    } else if (event.key.keysym.sym == SDLK_LEFTBRACKET ||
                               event.key.keysym.sym == SDLK_RIGHTBRACKET) {
                        float delta = (event.key.keysym.sym == SDLK_RIGHTBRACKET) ? 2.5f : -2.5f;
                        mTrackingDelayMs = std::max(0.0f, mTrackingDelayMs + delta);
                        std::cout << "Tracking delay: " << mTrackingDelayMs << "ms" << std::endl;
                    } else if (event.key.keysym.sym == SDLK_SPACE) {
                        mAutoOrbit = !mAutoOrbit;
                        std::cout << "Auto orbit: " << (mAutoOrbit ? "ON" : "OFF") << std::endl;
                    } else if (event.key.keysym.sym == SDLK_F5) {
                        if (mRecording) stopRecording();
                        else startRecording();
                    } else if (event.key.keysym.sym == SDLK_DELETE) {
                        for (auto& marker : mTrailMarkers) {
                            mEngine->getRenderableManager().destroy(marker);
                            EntityManager::get().destroy(marker);
                        }
                        mTrailMarkers.clear();
                        std::cout << "Trail markers cleared" << std::endl;
                    } else if (event.key.keysym.sym == SDLK_g) {
                        float tipOffset = mConeScale;
                        float oldY = mConePos.y;
                        mConePos.y = mGroundWorldY + tipOffset;
                        updateConeTransform();
                        std::cout << "G: cone Y " << oldY << " -> " << mConePos.y
                                  << " (tip at Y=" << mGroundWorldY
                                  << ", ground Y=" << mGroundWorldY
                                  << ", scale=" << mConeScale << ")" << std::endl;
                    } else if ((event.key.keysym.mod & KMOD_SHIFT) &&
                               (event.key.keysym.sym == SDLK_COMMA ||
                                event.key.keysym.sym == SDLK_PERIOD)) {
                        if (!mGlbObjects.empty() && mGlbObjects[0].asset) {
                            auto& obj = mGlbObjects[0];
                            const float PI = 3.14159265358979f;
                            const float STEP = 15.0f * PI / 180.0f;
                            if (event.key.keysym.sym == SDLK_COMMA)  obj.yRotation -= STEP;
                            if (event.key.keysym.sym == SDLK_PERIOD) obj.yRotation += STEP;
                            updateObjectTransform(obj);
                            std::cout << "Object rot: " << (obj.yRotation * 180.0f / PI) << " deg" << std::endl;
                        }
                    } else if ((event.key.keysym.mod & KMOD_SHIFT) &&
                               (event.key.keysym.sym == SDLK_UP ||
                                event.key.keysym.sym == SDLK_DOWN ||
                                event.key.keysym.sym == SDLK_LEFT ||
                                event.key.keysym.sym == SDLK_RIGHT)) {
                        if (!mGlbObjects.empty() && mGlbObjects[0].asset) {
                            auto& obj = mGlbObjects[0];
                            auto camMat = mCamera->getModelMatrix();
                            float3 camFwd = normalize(float3{(float)-camMat[2][0], 0.0f, (float)-camMat[2][2]});
                            float3 camRight = normalize(float3{(float)camMat[0][0], 0.0f, (float)camMat[0][2]});
                            float3 delta = {0, 0, 0};
                            if (event.key.keysym.sym == SDLK_UP)    delta = camFwd * 1.0f;
                            if (event.key.keysym.sym == SDLK_DOWN)  delta = camFwd * -1.0f;
                            if (event.key.keysym.sym == SDLK_LEFT)  delta = camRight * -1.0f;
                            if (event.key.keysym.sym == SDLK_RIGHT) delta = camRight * 1.0f;
                            obj.position.x += delta.x;
                            obj.position.y += delta.y;
                            obj.position.z += delta.z;
                            updateObjectTransform(obj);
                            std::cout << "Object pos: (" << obj.position.x << ", " << obj.position.y << ", " << obj.position.z << ")" << std::endl;
                        }
                    } else if ((event.key.keysym.mod & KMOD_SHIFT) &&
                               (event.key.keysym.sym == SDLK_a ||
                                event.key.keysym.sym == SDLK_z)) {
                        if (!mGlbObjects.empty() && mGlbObjects[0].asset) {
                            auto& obj = mGlbObjects[0];
                            if (event.key.keysym.sym == SDLK_a) obj.position.y += 1.0f;
                            if (event.key.keysym.sym == SDLK_z) obj.position.y -= 1.0f;
                            updateObjectTransform(obj);
                            std::cout << "Object pos: (" << obj.position.x << ", " << obj.position.y << ", " << obj.position.z << ")" << std::endl;
                        }
                    } else if (event.key.keysym.sym == SDLK_a ||
                               event.key.keysym.sym == SDLK_z) {
                        if (event.key.keysym.sym == SDLK_a) mConePos.y += 0.1f;
                        if (event.key.keysym.sym == SDLK_z) mConePos.y -= 0.1f;
                        updateConeTransform();
                        std::cout << "Cone pos: (" << mConePos.x << ", " << mConePos.y << ", " << mConePos.z << ")" << std::endl;
                    } else if (event.key.keysym.sym == SDLK_UP ||
                               event.key.keysym.sym == SDLK_DOWN ||
                               event.key.keysym.sym == SDLK_LEFT ||
                               event.key.keysym.sym == SDLK_RIGHT) {
                        auto camMat = mCamera->getModelMatrix();
                        float3 camFwd = normalize(float3{(float)-camMat[2][0], 0.0f, (float)-camMat[2][2]});
                        float3 camRight = normalize(float3{(float)camMat[0][0], 0.0f, (float)camMat[0][2]});
                        float3 delta = {0, 0, 0};
                        if (event.key.keysym.sym == SDLK_UP)    delta = camFwd * 1.0f;
                        if (event.key.keysym.sym == SDLK_DOWN)  delta = camFwd * -1.0f;
                        if (event.key.keysym.sym == SDLK_LEFT)  delta = camRight * -1.0f;
                        if (event.key.keysym.sym == SDLK_RIGHT) delta = camRight * 1.0f;
                        mConePos.x += delta.x;
                        mConePos.y += delta.y;
                        mConePos.z += delta.z;
                        updateConeTransform();
                        std::cout << "Cone pos: (" << mConePos.x << ", " << mConePos.y << ", " << mConePos.z << ")" << std::endl;
                    } else if ((event.key.keysym.mod & KMOD_SHIFT) &&
                               (event.key.keysym.sym == SDLK_x ||
                                event.key.keysym.sym == SDLK_c)) {
                        if (!mGlbObjects.empty() && mGlbObjects[0].asset) {
                            auto& obj = mGlbObjects[0];
                            if (event.key.keysym.sym == SDLK_x) obj.scale *= 0.75f;
                            if (event.key.keysym.sym == SDLK_c) obj.scale *= 1.25f;
                            updateObjectTransform(obj);
                            std::cout << "Object scale: " << obj.scale << std::endl;
                        }
                    } else if (event.key.keysym.sym == SDLK_x ||
                               event.key.keysym.sym == SDLK_c) {
                        if (event.key.keysym.sym == SDLK_x) mConeScale *= 0.75f;
                        if (event.key.keysym.sym == SDLK_c) mConeScale *= 1.25f;
                        updateConeTransform();
                        std::cout << "Cone scale: " << mConeScale << std::endl;
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (event.button.windowID != mMainWindowID) break;
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        mMouseDragging = true;
                        mLastMouseX = event.button.x;
                        mLastMouseY = event.button.y;
                    } else if (event.button.button == SDL_BUTTON_RIGHT) {
                        mLightDragging = true;
                        mLastMouseX = event.button.x;
                        mLastMouseY = event.button.y;
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (event.button.windowID != mMainWindowID) break;
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        mMouseDragging = false;
                    } else if (event.button.button == SDL_BUTTON_RIGHT) {
                        mLightDragging = false;
                    }
                    break;
                case SDL_MOUSEMOTION:
                    if (event.motion.windowID != mMainWindowID) break;
                    if (mMouseDragging || mLightDragging) {
                        int dx = event.motion.x - mLastMouseX;
                        int dy = event.motion.y - mLastMouseY;
                        mLastMouseX = event.motion.x;
                        mLastMouseY = event.motion.y;

                        const float PI = 3.14159265358979f;

                        if (mMouseDragging) {
                            mCameraYaw -= dx * 0.005f;
                            mCameraPitch += dy * 0.005f;
                            if (mCameraPitch > PI * 0.4944f) mCameraPitch = PI * 0.4944f;
                            if (mCameraPitch < -PI * 0.4944f) mCameraPitch = -PI * 0.4944f;
                        }

                        if (mLightDragging) {
                            mLightYaw -= dx * 0.005f;
                            mLightPitch -= dy * 0.005f;
                            if (mLightPitch > -0.1f) mLightPitch = -0.1f;
                            if (mLightPitch < -PI * 0.45f) mLightPitch = -PI * 0.45f;
                        }
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    if (event.wheel.windowID != mMainWindowID) break;
                    mCameraDistance -= event.wheel.y * 0.5f;
                    if (mCameraDistance < 2.0f) mCameraDistance = 2.0f;
                    if (mCameraDistance > 20.0f) mCameraDistance = 20.0f;
                    break;
                case SDL_WINDOWEVENT:
                    if (event.window.windowID == mMainWindowID) {
                        if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                            updateSidebarLayout();
                        } else if (event.window.event == SDL_WINDOWEVENT_MOVED &&
                                   mSidebarDocked) {
                            updateSidebarLayout();
                        }
                    }
                    break;
            }
        }

        Uint64 currentTime = SDL_GetPerformanceCounter();
        float deltaTime = (float)(currentTime - lastTime) / (float)frequency;
        lastTime = currentTime;

        bool newWebcamFrame = updateWebcamFrame();

        // Build and render ImGui sidebar
        if (mSidebarVisible) {
            SDL_GL_MakeCurrent(mControlWindow, mGLContext);
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            buildImGuiPanel();
            ImGui::Render();
            int glW, glH;
            SDL_GL_GetDrawableSize(mControlWindow, &glW, &glH);
            glViewport(0, 0, glW, glH);
            glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            SDL_GL_SwapWindow(mControlWindow);
        }

        update(deltaTime, newWebcamFrame);
        bool rendered = render();

        // Log frame timing
        if (mCsvFile) {
            auto now = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(now - mCsvStartTime).count();
            fprintf(mCsvFile, "%.2f,%d,%d,%.1f,%.4f,%.4f,%.6f,%.4f,%.4f,%.6f,%.6f,%.4f,%.6f,%.6f,%.4f\n",
                    ms, newWebcamFrame ? 1 : 0, rendered ? 1 : 0, deltaTime * 1000.0f,
                    mDbgRawYaw, mDbgSmoothedYaw, mDbgYawFil,
                    mDbgRawPitch, mDbgSmoothedPitch,
                    mDbgRawX, mDbgRawY, mDbgRawZ,
                    mDbgSmoothedX, mDbgSmoothedY, mDbgSmoothedZ);
        }

        // Update FPS counter
        mFpsAccumulator += deltaTime;
        if (rendered) mFrameCount++;
        if (mFpsAccumulator >= 0.5f) {
            mCurrentFps = mFrameCount / mFpsAccumulator;
            float serialHz = mSerialMsgCount / mFpsAccumulator;
            float webcamHz = mWebcamCBCount / mFpsAccumulator;
            mFrameCount = 0;
            mSerialMsgCount = 0;
            mWebcamCBCount = 0;
            mFpsAccumulator = 0.0f;

            char title[256];
            const auto& track = mTrackingDisplay;
            if (track.valid) {
                float cx = (float)track.x;
                float cy = track.z;
                float cz = -(float)track.y;
                float dx = cx - mAnchorWorldPos.x;
                float dy = cy - mAnchorWorldPos.y;
                float dz = cz - mAnchorWorldPos.z;
                float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                const char* rtkStr = "No Fix";
                switch (track.solutionType) {
                    case 1: rtkStr = "Single"; break;
                    case 2: rtkStr = "DGPS"; break;
                    case 3: rtkStr = "RTK Float"; break;
                    case 4: rtkStr = "RTK Fixed"; break;
                }
                float delay = mTrackingDelayMs;
                int pos = snprintf(title, sizeof(title),
                    "%.0f FPS | Dist:%.2fm | Cam:(%.2f,%.2f,%.2f) | R:%.1f P:%.1f Y:%.1f | %s(%d)",
                    mCurrentFps, dist, cx, cy, cz,
                    track.roll, track.pitch, track.yaw,
                    rtkStr, track.beacons);
                if (delay > 0.0f) {
                    snprintf(title + pos, sizeof(title) - pos, " | Delay:%.1fms", delay);
                }
            } else {
                snprintf(title, sizeof(title), "%.0f FPS | Cam:%.0fHz (No tracking)", mCurrentFps, webcamHz);
            }
            if (mRecording) {
                size_t pos = strlen(title);
                snprintf(title + pos, sizeof(title) - pos, " | REC %d frames", mRecordFrameCount);
            }
            SDL_SetWindowTitle(mWindow, title);
        }

        // Sync render loop to webcam frame delivery
        if (mWebcamFrameEvent && mWebcamEnabled) {
            MsgWaitForMultipleObjects(1, &mWebcamFrameEvent, FALSE, 50, QS_ALLINPUT);
        } else {
            double targetMS = 33.3;
            for (;;) {
                Uint64 now = SDL_GetPerformanceCounter();
                double elapsed = (double)(now - currentTime) / (double)frequency * 1000.0;
                double remaining = targetMS - elapsed;
                if (remaining <= 0) break;
                if (remaining > 2.0) {
                    SDL_Delay((Uint32)(remaining - 2.0));
                }
            }
        }
    }
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
#endif

    FilamentApp app;

    if (!app.init()) {
        std::cerr << "Failed to initialize application" << std::endl;
        return 1;
    }

    std::cout << "Filament Red Cone Demo" << std::endl;
    std::cout << "Press ESC to exit" << std::endl;

    app.run();

#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return 0;
}
