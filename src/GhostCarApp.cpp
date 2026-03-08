#include "GhostCarApp.h"

#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/LightManager.h>
#include <filament/TextureSampler.h>

#include <gltfio/materials/uberarchive.h>

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <SDL_opengl.h>

#ifdef _WIN32
#include <SDL_syswm.h>
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#endif

#include <fstream>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <filesystem>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// File dialog helper
#ifdef _WIN32
static std::string openFileDialogGC(HWND owner, const char* title, const char* filter, const char* ext) {
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = ext;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return std::string(filename);
    return "";
}
#endif

GhostCarApp::GhostCarApp() = default;

GhostCarApp::~GhostCarApp() {
    if (mVideoCapture.isOpened()) mVideoCapture.release();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (mGLContext) SDL_GL_DeleteContext(mGLContext);
    if (mControlWindow) SDL_DestroyWindow(mControlWindow);

    if (mEngine) {
        if (mBackgroundView) mEngine->destroy(mBackgroundView);
        if (mBackgroundScene) mEngine->destroy(mBackgroundScene);
        if (mBackgroundCamera) {
            mEngine->destroyCameraComponent(mBackgroundCameraEntity);
            EntityManager::get().destroy(mBackgroundCameraEntity);
        }
        if (mBackgroundEntity.isNull() == false) {
            mEngine->getRenderableManager().destroy(mBackgroundEntity);
            EntityManager::get().destroy(mBackgroundEntity);
        }
        if (mBackgroundMI) mEngine->destroy(mBackgroundMI);
        if (mBackgroundMaterial) mEngine->destroy(mBackgroundMaterial);
        if (mBackgroundVB) mEngine->destroy(mBackgroundVB);
        if (mBackgroundIB) mEngine->destroy(mBackgroundIB);
        if (mVideoTexture) mEngine->destroy(mVideoTexture);

        destroyDrivingLine();

        for (auto* mi : mGhostMIs) mEngine->destroy(mi);
        mGhostMIs.clear();
        if (mGhostMaterial) mEngine->destroy(mGhostMaterial);
        if (mLineMaterial) mEngine->destroy(mLineMaterial);
        if (mGhostAsset) {
            mScene->removeEntities(mGhostAsset->getEntities(), mGhostAsset->getEntityCount());
            mAssetLoader->destroyAsset(mGhostAsset);
        }

        if (mResourceLoader) delete mResourceLoader;
        if (mMaterialProvider) { mMaterialProvider->destroyMaterials(); delete mMaterialProvider; }
        if (mStbDecoder) delete mStbDecoder;
        if (mAssetLoader) filament::gltfio::AssetLoader::destroy(&mAssetLoader);

        mEngine->destroyCameraComponent(mCameraEntity);
        EntityManager::get().destroy(mCameraEntity);
        EntityManager::get().destroy(mSunLight);
        if (mIndirectLight) mEngine->destroy(mIndirectLight);
        if (mReflectionsCubemap) mEngine->destroy(mReflectionsCubemap);

        mEngine->destroy(mView);
        mEngine->destroy(mScene);
        mEngine->destroy(mRenderer);
        mEngine->destroy(mSwapChain);
        Engine::destroy(&mEngine);
    }

    if (mWindow) SDL_DestroyWindow(mWindow);
    SDL_Quit();
}

Material* GhostCarApp::loadMaterial(const std::string& name) {
    std::vector<std::string> paths = { name, "./" + name, "materials/" + name };
#ifdef _WIN32
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir(exePath);
    size_t slash = exeDir.find_last_of("\\/");
    if (slash != std::string::npos) {
        paths.insert(paths.begin(), exeDir.substr(0, slash + 1) + name);
    }
#endif
    std::ifstream matFile;
    for (const auto& p : paths) {
        matFile.open(p, std::ios::binary);
        if (matFile) break;
    }
    if (!matFile) {
        std::cerr << "Cannot find material: " << name << std::endl;
        return nullptr;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(matFile)),
                               std::istreambuf_iterator<char>());
    return Material::Builder().package(data.data(), data.size()).build(*mEngine);
}

bool GhostCarApp::init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << std::endl;
        return false;
    }

    mWindow = SDL_CreateWindow("GhostCar",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        mWinWidth, mWinHeight,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!mWindow) return false;

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
    if (!mEngine) { std::cerr << "Filament engine failed" << std::endl; return false; }

    mSwapChain = mEngine->createSwapChain(nativeWindow);
    mRenderer = mEngine->createRenderer();

    Renderer::FrameRateOptions fro;
    fro.interval = 0;
    mRenderer->setFrameRateOptions(fro);

    Renderer::ClearOptions co;
    co.clearColor = {0, 0, 0, 0};
    co.clear = false;
    co.discard = true;
    mRenderer->setClearOptions(co);

    mScene = mEngine->createScene();
    mView = mEngine->createView();
    mView->setScene(mScene);
    mView->setViewport({0, 0, (uint32_t)mWinWidth, (uint32_t)mWinHeight});
    mView->setShadowingEnabled(false);
    mView->setPostProcessingEnabled(true);
    mView->setBlendMode(View::BlendMode::TRANSLUCENT);

    // Initialize gltfio
    mMaterialProvider = filament::gltfio::createUbershaderProvider(
        mEngine, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);

    filament::gltfio::AssetConfiguration ac = {};
    ac.engine = mEngine;
    ac.materials = mMaterialProvider;
    mAssetLoader = filament::gltfio::AssetLoader::create(ac);

    std::string resPath = ".";
#ifdef _WIN32
    {
        char ep[MAX_PATH];
        GetModuleFileNameA(NULL, ep, MAX_PATH);
        std::string d(ep);
        size_t s = d.find_last_of("\\/");
        if (s != std::string::npos) resPath = d.substr(0, s + 1);
    }
#endif
    mStbDecoder = filament::gltfio::createStbProvider(mEngine);
    mResourceLoader = new filament::gltfio::ResourceLoader({mEngine, resPath.c_str(), true});
    mResourceLoader->addTextureProvider("image/png", mStbDecoder);
    mResourceLoader->addTextureProvider("image/jpeg", mStbDecoder);

    createCamera();
    createLighting();

    // Create video texture (will be resized when video opens)
    mVideoTexture = Texture::Builder()
        .width(mVideoWidth).height(mVideoHeight).levels(1)
        .format(Texture::InternalFormat::RGB8)
        .sampler(Texture::Sampler::SAMPLER_2D)
        .build(*mEngine);

    createBackgroundQuad();

    // Load materials
    mGhostMaterial = loadMaterial("ghost_car.filamat");
    mLineMaterial = loadMaterial("line.filamat");

    // Load ghost car model
    loadGhostCar();

    // --- ImGui sidebar setup ---
    mMainWindowID = SDL_GetWindowID(mWindow);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    float dpiScale = 1.0f;
    {
        int di = SDL_GetWindowDisplayIndex(mWindow);
        float ddpi = 0, hdpi = 0, vdpi = 0;
        if (SDL_GetDisplayDPI(di, &ddpi, &hdpi, &vdpi) == 0 && ddpi > 0) {
            dpiScale = ddpi / 96.0f;
            if (dpiScale < 1.0f) dpiScale = 1.0f;
        }
    }
    mSidebarWidth = (int)(350 * dpiScale);

    int mainX, mainY;
    SDL_GetWindowPosition(mWindow, &mainX, &mainY);
    mControlWindow = SDL_CreateWindow("Ghost Car Controls",
        mainX, mainY, mSidebarWidth, mWinHeight,
        SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!mControlWindow) return false;

    mGLContext = SDL_GL_CreateContext(mControlWindow);
    if (!mGLContext) return false;
    SDL_GL_SetSwapInterval(0);
    mControlWindowID = SDL_GetWindowID(mControlWindow);

#ifdef _WIN32
    {
        SDL_SysWMinfo cw, mw;
        SDL_VERSION(&cw.version); SDL_VERSION(&mw.version);
        SDL_GetWindowWMInfo(mControlWindow, &cw);
        SDL_GetWindowWMInfo(mWindow, &mw);
        SetWindowLongPtr(cw.info.win.window, GWLP_HWNDPARENT, (LONG_PTR)mw.info.win.window);
    }
#endif

    updateSidebarLayout();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    float fontSize = 18.0f * dpiScale;
    const char* fontPaths[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
    };
    bool fontLoaded = false;
    for (const char* fp : fontPaths) {
        std::ifstream ff(fp, std::ios::binary | std::ios::ate);
        if (!ff) continue;
        size_t sz = ff.tellg();
        ff.seekg(0);
        void* fd = IM_ALLOC(sz);
        ff.read((char*)fd, sz);
        ff.close();
        if (io.Fonts->AddFontFromMemoryTTF(fd, (int)sz, fontSize)) { fontLoaded = true; break; }
    }
    if (!fontLoaded) io.Fonts->AddFontDefault();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpiScale);
    style.WindowRounding = 6.0f;
    style.FrameRounding = 3.0f;

    ImGui_ImplSDL2_InitForOpenGL(mControlWindow, mGLContext);
    ImGui_ImplOpenGL3_Init("#version 130");

    // Try to auto-load circuit database from exe directory
    std::string dbPath = resPath + "StartFinishDataBase.xml";
    if (!std::filesystem::exists(dbPath)) {
        dbPath = "StartFinishDataBase.xml";
    }
    if (std::filesystem::exists(dbPath)) {
        loadCircuitDatabase(dbPath, mCircuitDb);
        strncpy(mDbPath, dbPath.c_str(), sizeof(mDbPath) - 1);
    }

    loadSettings();

    return true;
}

void GhostCarApp::createCamera() {
    EntityManager& em = EntityManager::get();
    mCameraEntity = em.create();
    auto& tcm = mEngine->getTransformManager();
    tcm.create(mCameraEntity);
    mCamera = mEngine->createCamera(mCameraEntity);
    mView->setCamera(mCamera);

    float aspect = (float)mWinWidth / (float)mWinHeight;
    mCamera->setProjection(mCameraFovH, aspect, 0.1f, 2000.0f, Camera::Fov::HORIZONTAL);
    mCamera->lookAt({0, mCameraHeight, 0}, {0, mCameraHeight, -100}, {0, 1, 0});
}

void GhostCarApp::createLighting() {
    EntityManager& em = EntityManager::get();
    mSunLight = em.create();

    LightManager::Builder(LightManager::Type::SUN)
        .color({1.0f, 0.95f, 0.9f})
        .intensity(100000.0f)
        .direction({-0.5f, -1.0f, -0.5f})
        .castShadows(false)
        .build(*mEngine, mSunLight);

    mScene->addEntity(mSunLight);

    // Simple ambient IBL
    static float3 sh[9] = {};
    sh[0] = {1.0f, 1.0f, 1.0f};

    mIndirectLight = IndirectLight::Builder()
        .irradiance(3, sh)
        .intensity(30000.0f)
        .build(*mEngine);

    mScene->setIndirectLight(mIndirectLight);
}

void GhostCarApp::createBackgroundQuad() {
    static BgVertex verts[4] = {
        {{-1, 1, 0}, {0, 1}, {0, 0, 0, 32767}},
        {{ 1, 1, 0}, {1, 1}, {0, 0, 0, 32767}},
        {{ 1,-1, 0}, {1, 0}, {0, 0, 0, 32767}},
        {{-1,-1, 0}, {0, 0}, {0, 0, 0, 32767}},
    };
    static uint16_t indices[6] = {0, 3, 2, 0, 2, 1};

    mBackgroundVB = VertexBuffer::Builder()
        .vertexCount(4).bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3,
                   offsetof(BgVertex, position), sizeof(BgVertex))
        .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2,
                   offsetof(BgVertex, uv), sizeof(BgVertex))
        .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::SHORT4,
                   offsetof(BgVertex, tangents), sizeof(BgVertex))
        .normalized(VertexAttribute::TANGENTS)
        .build(*mEngine);

    mBackgroundVB->setBufferAt(*mEngine, 0,
        VertexBuffer::BufferDescriptor(verts, sizeof(verts)));

    mBackgroundIB = IndexBuffer::Builder()
        .indexCount(6)
        .bufferType(IndexBuffer::IndexType::USHORT)
        .build(*mEngine);
    mBackgroundIB->setBuffer(*mEngine,
        IndexBuffer::BufferDescriptor(indices, sizeof(indices)));

    mBackgroundMaterial = loadMaterial("background.filamat");
    if (mBackgroundMaterial) {
        mBackgroundMI = mBackgroundMaterial->createInstance();
        TextureSampler sampler;
        sampler.setMagFilter(TextureSampler::MagFilter::LINEAR);
        sampler.setMinFilter(TextureSampler::MinFilter::LINEAR);
        mBackgroundMI->setParameter("videoTexture", mVideoTexture, sampler);
    }

    EntityManager& em = EntityManager::get();
    mBackgroundEntity = em.create();

    RenderableManager::Builder(1)
        .boundingBox({{-1,-1,-1}, {1,1,1}})
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                  mBackgroundVB, mBackgroundIB, 0, 6)
        .material(0, mBackgroundMI)
        .culling(false)
        .receiveShadows(false)
        .castShadows(false)
        .build(*mEngine, mBackgroundEntity);

    mBackgroundScene = mEngine->createScene();
    mBackgroundScene->addEntity(mBackgroundEntity);

    mBackgroundCameraEntity = em.create();
    mBackgroundCamera = mEngine->createCamera(mBackgroundCameraEntity);
    mBackgroundCamera->setProjection(Camera::Projection::ORTHO, -1, 1, -1, 1, 0, 10);
    mBackgroundCamera->lookAt({0,0,1}, {0,0,0}, {0,1,0});

    mBackgroundView = mEngine->createView();
    mBackgroundView->setScene(mBackgroundScene);
    mBackgroundView->setCamera(mBackgroundCamera);
    mBackgroundView->setViewport({0, 0, (uint32_t)mWinWidth, (uint32_t)mWinHeight});
    mBackgroundView->setPostProcessingEnabled(false);
    mBackgroundView->setShadowingEnabled(false);
}

void GhostCarApp::loadGhostCar() {
    // Search for funcup.glb
    std::vector<std::string> paths = {"funcup.glb", "assets/funcup.glb", "./funcup.glb"};
#ifdef _WIN32
    char ep[MAX_PATH];
    GetModuleFileNameA(NULL, ep, MAX_PATH);
    std::string ed(ep);
    size_t s = ed.find_last_of("\\/");
    if (s != std::string::npos) {
        std::string dir = ed.substr(0, s + 1);
        paths.insert(paths.begin(), dir + "funcup.glb");
    }
#endif

    std::ifstream file;
    std::string foundPath;
    for (const auto& p : paths) {
        file.open(p, std::ios::binary);
        if (file) { foundPath = p; break; }
    }
    if (!file) {
        std::cerr << "Cannot find funcup.glb" << std::endl;
        return;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    std::cout << "Loading ghost car: " << foundPath << " (" << data.size() << " bytes)" << std::endl;

    mGhostAsset = mAssetLoader->createAsset(data.data(), (uint32_t)data.size());
    if (!mGhostAsset) {
        std::cerr << "Failed to parse funcup.glb" << std::endl;
        return;
    }

    mResourceLoader->loadResources(mGhostAsset);
    mGhostAsset->releaseSourceData();

    auto aabb = mGhostAsset->getBoundingBox();
    std::cout << "  Ghost car AABB: (" << aabb.min.x << "," << aabb.min.y << "," << aabb.min.z
              << ") to (" << aabb.max.x << "," << aabb.max.y << "," << aabb.max.z << ")" << std::endl;

    mScene->addEntities(mGhostAsset->getEntities(), mGhostAsset->getEntityCount());

    // Apply transparency
    applyGhostTransparency();

    // Initially hide the ghost car
    hideGhostCar();
}

void GhostCarApp::applyGhostTransparency() {
    if (!mGhostAsset || !mGhostMaterial) return;

    auto& rm = mEngine->getRenderableManager();
    const Entity* allEntities = mGhostAsset->getRenderableEntities();
    size_t count = mGhostAsset->getRenderableEntityCount();

    // Clear previous ghost MIs
    for (auto* mi : mGhostMIs) mEngine->destroy(mi);
    mGhostMIs.clear();

    for (size_t i = 0; i < count; i++) {
        auto ri = rm.getInstance(allEntities[i]);
        if (!ri) continue;

        size_t primCount = rm.getPrimitiveCount(ri);
        for (size_t p = 0; p < primCount; p++) {
            auto* mi = mGhostMaterial->createInstance();
            mi->setParameter("color", float3{0.85f, 0.85f, 0.85f});
            mi->setParameter("alpha", mGhostAlpha);
            rm.setMaterialInstanceAt(ri, p, mi);
            mGhostMIs.push_back(mi);
        }

        rm.setCastShadows(ri, false);
        rm.setReceiveShadows(ri, false);
    }
}

static std::string getSettingsPath() {
#ifdef _WIN32
    char ep[MAX_PATH];
    GetModuleFileNameA(NULL, ep, MAX_PATH);
    std::string d(ep);
    size_t s = d.find_last_of("\\/");
    if (s != std::string::npos) return d.substr(0, s + 1) + "ghostcar_settings.ini";
#endif
    return "ghostcar_settings.ini";
}

void GhostCarApp::saveSettings() {
    std::ofstream f(getSettingsPath());
    if (!f) return;
    f << "vboPath=" << mVboPath << "\n";
    f << "ghostAlpha=" << mGhostAlpha << "\n";
    f << "cameraFovH=" << mCameraFovH << "\n";
    f << "cameraHeight=" << mCameraHeight << "\n";
    f << "cameraPitchOffset=" << mCameraPitchOffset << "\n";
    f << "cameraPanOffset=" << mCameraPanOffset << "\n";
    f << "dataOffsetMs=" << mDataOffsetMs << "\n";
    f << "ghostYOffset=" << mGhostYOffset << "\n";
    f << "modelRotOffset=" << mModelRotOffset << "\n";
    f << "ghostScale=" << mGhostScale << "\n";
    f << "pitchCompensation=" << (mPitchCompensation ? 1 : 0) << "\n";
    f << "showDrivingLine=" << (mShowDrivingLine ? 1 : 0) << "\n";
    f << "lineWidth=" << mLineWidth << "\n";
    f << "playbackSpeed=" << mPlaybackSpeed << "\n";
    std::cout << "Settings saved to " << getSettingsPath() << std::endl;
}

void GhostCarApp::loadSettings() {
    std::ifstream f(getSettingsPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "vboPath") strncpy(mVboPath, val.c_str(), sizeof(mVboPath) - 1);
        else if (key == "ghostAlpha") mGhostAlpha = std::stof(val);
        else if (key == "cameraFovH") mCameraFovH = std::stof(val);
        else if (key == "cameraHeight") mCameraHeight = std::stof(val);
        else if (key == "cameraPitchOffset") mCameraPitchOffset = std::stof(val);
        else if (key == "cameraPanOffset") mCameraPanOffset = std::stof(val);
        else if (key == "dataOffsetMs") mDataOffsetMs = std::stof(val);
        else if (key == "ghostYOffset") mGhostYOffset = std::stof(val);
        else if (key == "modelRotOffset") mModelRotOffset = std::stof(val);
        else if (key == "ghostScale") mGhostScale = std::stof(val);
        else if (key == "pitchCompensation") mPitchCompensation = (val == "1");
        else if (key == "showDrivingLine") mShowDrivingLine = (val == "1");
        else if (key == "lineWidth") mLineWidth = std::stof(val);
        else if (key == "playbackSpeed") mPlaybackSpeed = std::stof(val);
    }
    // Apply loaded FOV
    if (mCamera) {
        float aspect = (float)mWinWidth / (float)mWinHeight;
        mCamera->setProjection(mCameraFovH, aspect, 0.1f, 2000.0f, Camera::Fov::HORIZONTAL);
    }
    // Apply loaded alpha to ghost material instances
    for (auto* mi : mGhostMIs) {
        mi->setParameter("alpha", mGhostAlpha);
    }
    std::cout << "Settings loaded from " << getSettingsPath() << std::endl;
}

float GhostCarApp::computePitch(const VboFile& vbo, int sampleIdx, int windowSamples) {
    // Look windowSamples ahead and behind to get a smoothed pitch angle
    int n = (int)vbo.samples.size();
    int idxBefore = std::max(0, sampleIdx - windowSamples);
    int idxAfter = std::min(n - 1, sampleIdx + windowSamples);

    if (idxBefore == idxAfter) return 0.0f;

    const auto& sBefore = vbo.samples[idxBefore];
    const auto& sAfter = vbo.samples[idxAfter];

    double heightDiff = sAfter.height - sBefore.height;

    // Compute horizontal distance between the two samples
    double eastB, northB, eastA, northA;
    gpsToLocalMeters(sBefore.latitude, sBefore.longitude,
                     sBefore.latitude, sBefore.longitude, eastB, northB);
    gpsToLocalMeters(sAfter.latitude, sAfter.longitude,
                     sBefore.latitude, sBefore.longitude, eastA, northA);

    double horizDist = sqrt(eastA * eastA + northA * northA);
    if (horizDist < 0.1) return 0.0f;  // too close, skip

    return (float)atan2(heightDiff, horizDist);
}

void GhostCarApp::createDrivingLine(const LapInfo& lap) {
    destroyDrivingLine();

    mLineSampleCount = lap.endIdx - lap.startIdx + 1;
    if (mLineSampleCount < 2) return;
    mLineVertexCount = mLineSampleCount * 2;  // left + right vertex per sample

    // Vertex buffer: positions updated each frame, tangents set once
    mLineVB = VertexBuffer::Builder()
        .vertexCount(mLineVertexCount).bufferCount(2)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3, 0, sizeof(float3))
        .attribute(VertexAttribute::TANGENTS, 1, VertexBuffer::AttributeType::SHORT4, 0, 4 * sizeof(int16_t))
        .normalized(VertexAttribute::TANGENTS)
        .build(*mEngine);

    // Set dummy tangents once (pointing up)
    int16_t* tangents = new int16_t[mLineVertexCount * 4];
    for (int i = 0; i < mLineVertexCount; i++) {
        tangents[i * 4 + 0] = 0;
        tangents[i * 4 + 1] = 0;
        tangents[i * 4 + 2] = 0;
        tangents[i * 4 + 3] = 32767;
    }
    mLineVB->setBufferAt(*mEngine, 1, VertexBuffer::BufferDescriptor(
        tangents, mLineVertexCount * 4 * sizeof(int16_t),
        [](void* buf, size_t, void*) { delete[] static_cast<int16_t*>(buf); }
    ));

    // Index buffer: sequential for triangle strip
    uint16_t* indices = new uint16_t[mLineVertexCount];
    for (int i = 0; i < mLineVertexCount; i++) indices[i] = (uint16_t)i;
    mLineIB = IndexBuffer::Builder()
        .indexCount(mLineVertexCount)
        .bufferType(IndexBuffer::IndexType::USHORT)
        .build(*mEngine);
    mLineIB->setBuffer(*mEngine, IndexBuffer::BufferDescriptor(
        indices, mLineVertexCount * sizeof(uint16_t),
        [](void* buf, size_t, void*) { delete[] static_cast<uint16_t*>(buf); }
    ));

    // Material instance - use line material (no depth write, so always visible)
    if (mLineMaterial) {
        mLineMI = mLineMaterial->createInstance();
        mLineMI->setParameter("color", float3{1.0f, 1.0f, 0.0f});
        mLineMI->setParameter("alpha", 0.8f);
    }

    EntityManager& em = EntityManager::get();
    mLineEntity = em.create();
    RenderableManager::Builder(1)
        .boundingBox({{-10000, -100, -10000}, {10000, 100, 10000}})
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLE_STRIP, mLineVB, mLineIB, 0, mLineVertexCount)
        .material(0, mLineMI)
        .culling(false)
        .castShadows(false)
        .receiveShadows(false)
        .build(*mEngine, mLineEntity);

    mScene->addEntity(mLineEntity);
}

void GhostCarApp::updateDrivingLine(int camIdx) {
    if (!mLineVB || mLineSampleCount < 2) return;

    const auto& camSample = mVbo.samples[camIdx];
    double headRad = camSample.heading * M_PI / 180.0;
    double cosH = cos(headRad);
    double sinH = sin(headRad);

    const auto& camLap = mLaps[mCameraLapIdx];
    float halfW = mLineWidth * 0.5f;
    float y = -mCameraHeight + mGhostYOffset;

    // First pass: compute centre positions in camera-local space with height
    double camHeight = camSample.height;
    std::vector<float3> centres(mLineSampleCount);
    for (int i = 0; i < mLineSampleCount; i++) {
        const auto& s = mVbo.samples[camLap.startIdx + i];
        double east, north;
        gpsToLocalMeters(s.latitude, s.longitude,
                         camSample.latitude, camSample.longitude, east, north);
        double relRight = east * cosH - north * sinH;
        double relForward = east * sinH + north * cosH;
        float heightDiff = (float)(s.height - camHeight);
        centres[i] = float3{(float)relRight, y + heightDiff, -(float)relForward};
    }

    // Second pass: generate ribbon (left/right offset perpendicular to direction)
    float3* verts = new float3[mLineVertexCount];
    for (int i = 0; i < mLineSampleCount; i++) {
        // Direction vector from previous to next sample
        int iPrev = std::max(0, i - 1);
        int iNext = std::min(mLineSampleCount - 1, i + 1);
        float dx = centres[iNext].x - centres[iPrev].x;
        float dz = centres[iNext].z - centres[iPrev].z;
        float len = sqrtf(dx * dx + dz * dz);
        if (len < 1e-6f) { dx = 0; dz = -1; len = 1; }
        // Perpendicular in XZ plane (rotate 90 degrees)
        float px = -dz / len * halfW;
        float pz =  dx / len * halfW;

        verts[i * 2 + 0] = float3{centres[i].x + px, centres[i].y, centres[i].z + pz};
        verts[i * 2 + 1] = float3{centres[i].x - px, centres[i].y, centres[i].z - pz};
    }

    mLineVB->setBufferAt(*mEngine, 0, VertexBuffer::BufferDescriptor(
        verts, mLineVertexCount * sizeof(float3),
        [](void* buf, size_t, void*) { delete[] static_cast<float3*>(buf); }
    ));
}

void GhostCarApp::destroyDrivingLine() {
    if (mLineEntity.isNull() == false) {
        mScene->remove(mLineEntity);
        mEngine->getRenderableManager().destroy(mLineEntity);
        EntityManager::get().destroy(mLineEntity);
        mLineEntity = {};
    }
    if (mLineMI) { mEngine->destroy(mLineMI); mLineMI = nullptr; }
    if (mLineVB) { mEngine->destroy(mLineVB); mLineVB = nullptr; }
    if (mLineIB) { mEngine->destroy(mLineIB); mLineIB = nullptr; }
    mLineVertexCount = 0;
}

void GhostCarApp::updateGhostCarTransform(float x, float y, float z, float yRotRad, float pitchRad) {
    if (!mGhostAsset) return;
    auto& tcm = mEngine->getTransformManager();
    auto ti = tcm.getInstance(mGhostAsset->getRoot());
    if (!ti) return;

    float rotOffset = mModelRotOffset * (float)M_PI / 180.0f;
    mat4f transform = mat4f::translation(float3{x, y, z}) *
                      mat4f::rotation(yRotRad + rotOffset, float3{0, 1, 0}) *
                      mat4f::rotation(pitchRad, float3{1, 0, 0}) *
                      mat4f::scaling(float3{mGhostScale, mGhostScale, mGhostScale});
    tcm.setTransform(ti, transform);
}

void GhostCarApp::hideGhostCar() {
    if (!mGhostAsset) return;
    auto& rm = mEngine->getRenderableManager();
    const Entity* ents = mGhostAsset->getEntities();
    for (size_t i = 0; i < mGhostAsset->getEntityCount(); i++) {
        auto ri = rm.getInstance(ents[i]);
        if (ri) rm.setLayerMask(ri, 0x1, 0x0);
    }
}

void GhostCarApp::showGhostCar() {
    if (!mGhostAsset) return;
    auto& rm = mEngine->getRenderableManager();
    const Entity* ents = mGhostAsset->getEntities();
    for (size_t i = 0; i < mGhostAsset->getEntityCount(); i++) {
        auto ri = rm.getInstance(ents[i]);
        if (ri) rm.setLayerMask(ri, 0x1, 0x1);
    }
}

bool GhostCarApp::uploadVideoFrame(const cv::Mat& frame) {
    if (frame.empty()) return false;

    int w = frame.cols;
    int h = frame.rows;

    // Recreate texture if size changed
    if (w != mVideoWidth || h != mVideoHeight) {
        mVideoWidth = w;
        mVideoHeight = h;
        if (mVideoTexture) mEngine->destroy(mVideoTexture);
        mVideoTexture = Texture::Builder()
            .width(w).height(h).levels(1)
            .format(Texture::InternalFormat::RGB8)
            .sampler(Texture::Sampler::SAMPLER_2D)
            .build(*mEngine);

        if (mBackgroundMI) {
            TextureSampler sampler;
            sampler.setMagFilter(TextureSampler::MagFilter::LINEAR);
            sampler.setMinFilter(TextureSampler::MinFilter::LINEAR);
            mBackgroundMI->setParameter("videoTexture", mVideoTexture, sampler);
        }
    }

    size_t dataSize = (size_t)w * h * 3;
    uint8_t* uploadData = new uint8_t[dataSize];
    memcpy(uploadData, frame.data, dataSize);

    Texture::PixelBufferDescriptor pbd(
        uploadData, dataSize,
        Texture::Format::RGB, Texture::Type::UBYTE,
        [](void* buf, size_t, void*) { delete[] static_cast<uint8_t*>(buf); }
    );
    mVideoTexture->setImage(*mEngine, 0, std::move(pbd));
    return true;
}

int GhostCarApp::findSampleAtAviTime(const VboFile& vbo, int startIdx, int endIdx, double aviTimeMs) {
    // Binary search for closest aviTime
    int lo = startIdx, hi = endIdx;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (vbo.samples[mid].aviTime < aviTimeMs) lo = mid + 1;
        else hi = mid;
    }
    if (lo > startIdx && lo < (int)vbo.samples.size()) {
        if (std::abs(vbo.samples[lo - 1].aviTime - aviTimeMs) <
            std::abs(vbo.samples[lo].aviTime - aviTimeMs)) {
            lo--;
        }
    }
    return std::clamp(lo, startIdx, endIdx);
}

int GhostCarApp::findSampleAtElapsed(const VboFile& vbo, int startIdx, int endIdx, double elapsedSec) {
    double startTime = vbo.samples[startIdx].timeSeconds;
    double targetTime = startTime + elapsedSec;

    int lo = startIdx, hi = endIdx;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (vbo.samples[mid].timeSeconds < targetTime) lo = mid + 1;
        else hi = mid;
    }
    if (lo > startIdx && lo < (int)vbo.samples.size()) {
        if (std::abs(vbo.samples[lo - 1].timeSeconds - targetTime) <
            std::abs(vbo.samples[lo].timeSeconds - targetTime)) {
            lo--;
        }
    }
    return std::clamp(lo, startIdx, endIdx);
}

void GhostCarApp::updateSidebarLayout() {
    int mainX, mainY;
    SDL_GetWindowPosition(mWindow, &mainX, &mainY);
    // Use full display height for the sidebar so all controls are visible
    int di = SDL_GetWindowDisplayIndex(mWindow);
    SDL_Rect bounds;
    int sidebarHeight = mWinHeight;
    if (SDL_GetDisplayUsableBounds(di, &bounds) == 0) {
        sidebarHeight = bounds.h;
    }
    SDL_SetWindowPosition(mControlWindow, mainX - mSidebarWidth, bounds.y);
    SDL_SetWindowSize(mControlWindow, mSidebarWidth, sidebarHeight);
}

void GhostCarApp::buildImGuiPanel() {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    int cw, ch;
    SDL_GetWindowSize(mControlWindow, &cw, &ch);
    ImGui::SetNextWindowSize(ImVec2((float)cw, (float)ch));
    ImGui::Begin("Ghost Car", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // --- File Selection ---
    if (ImGui::CollapsingHeader("Files", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("VBO File:");
        ImGui::InputText("##vbo", mVboPath, sizeof(mVboPath), ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##vbo")) {
#ifdef _WIN32
            std::string f = openFileDialogGC(NULL, "Open VBO File",
                "VBO Files (*.vbo)\0*.vbo\0All Files\0*.*\0", "vbo");
            if (!f.empty()) {
                strncpy(mVboPath, f.c_str(), sizeof(mVboPath) - 1);
                mVboLoaded = false;
            }
#endif
        }

        if (mVboPath[0] && !mVboLoaded) {
            if (ImGui::Button("Load VBO")) {
                mVbo = VboFile{};
                mLaps.clear();
                mCircuitDetected = false;
                mCameraLapIdx = -1;
                mGhostLapIdx = -1;
                mPlaying = false;
                mFastestLapIdx = -1;

                if (parseVboFile(mVboPath, mVbo)) {
                    mVboLoaded = true;

                    // Detect circuit
                    if (!mCircuitDb.empty()) {
                        mCircuitDetected = detectCircuit(mVbo, mCircuitDb, mCircuit);
                    }

                    // Detect laps using database circuit or VBO laptiming
                    double lon1, lat1, lon2, lat2;
                    double gw = 25.0;
                    if (mCircuitDetected) {
                        lon1 = mCircuit.sfLon; lat1 = mCircuit.sfLat;
                        lon2 = mCircuit.sfOldLon; lat2 = mCircuit.sfOldLat;
                        gw = mCircuit.gateWidth;
                    } else if (mVbo.hasLaptiming) {
                        lon1 = mVbo.sfLon1; lat1 = mVbo.sfLat1;
                        lon2 = mVbo.sfLon2; lat2 = mVbo.sfLat2;
                    } else {
                        lon1 = lat1 = lon2 = lat2 = 0;
                    }

                    if (lon1 != 0 || lat1 != 0) {
                        mLaps = detectLaps(mVbo, lon1, lat1, lon2, lat2, gw);

                        // Find fastest lap
                        if (!mLaps.empty()) {
                            double bestTime = 1e18;
                            for (int i = 0; i < (int)mLaps.size(); i++) {
                                if (mLaps[i].lapTimeSeconds < bestTime) {
                                    bestTime = mLaps[i].lapTimeSeconds;
                                    mFastestLapIdx = i;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (mVboLoaded) {
            ImGui::Text("Samples: %d (%.0f Hz)", (int)mVbo.samples.size(), mVbo.sampleRate);
        }
    }

    // --- Circuit Info ---
    if (mVboLoaded && ImGui::CollapsingHeader("Circuit", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (mCircuitDetected) {
            ImGui::Text("Name: %s", mCircuit.name.c_str());
            ImGui::Text("Country: %s", mCircuit.country.c_str());
            if (mCircuit.length > 0)
                ImGui::Text("Length: %d m", mCircuit.length);
        } else if (mVbo.hasLaptiming) {
            ImGui::Text("Circuit: (from VBO laptiming)");
        } else {
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "No circuit detected");
        }
    }

    // --- Lap List ---
    if (mVboLoaded && !mLaps.empty() &&
        ImGui::CollapsingHeader("Laps", ImGuiTreeNodeFlags_DefaultOpen)) {

        ImGui::Text("Select Video Lap and Ghost Lap:");
        ImGui::Separator();

        for (int i = 0; i < (int)mLaps.size(); i++) {
            const auto& lap = mLaps[i];
            int mins = (int)(lap.lapTimeSeconds / 60.0);
            double secs = lap.lapTimeSeconds - mins * 60.0;

            bool isFastest = (i == mFastestLapIdx);
            if (isFastest) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 1, 0, 1));

            char label[128];
            snprintf(label, sizeof(label), "Lap %d: %d:%06.3f%s",
                     lap.lapNumber, mins, secs, isFastest ? " (FASTEST)" : "");

            ImGui::Text("%s", label);
            ImGui::SameLine();

            char btnV[32], btnG[32];
            snprintf(btnV, sizeof(btnV), "Video##%d", i);
            snprintf(btnG, sizeof(btnG), "Ghost##%d", i);

            bool isVideoLap = (mCameraLapIdx == i);
            bool isGhostLap = (mGhostLapIdx == i);

            if (isVideoLap) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1));
            if (ImGui::SmallButton(btnV)) mCameraLapIdx = i;
            if (isVideoLap) ImGui::PopStyleColor();

            ImGui::SameLine();

            if (isGhostLap) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.8f, 1));
            if (ImGui::SmallButton(btnG)) mGhostLapIdx = i;
            if (isGhostLap) ImGui::PopStyleColor();

            if (isFastest) ImGui::PopStyleColor();
        }

        ImGui::Separator();
        if (mCameraLapIdx >= 0) {
            ImGui::Text("Video: Lap %d", mLaps[mCameraLapIdx].lapNumber);
        }
        if (mGhostLapIdx >= 0) {
            ImGui::Text("Ghost: Lap %d", mLaps[mGhostLapIdx].lapNumber);
        }
    }

    // --- Playback Controls ---
    if (mVboLoaded && mCameraLapIdx >= 0 && mGhostLapIdx >= 0 &&
        ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {

        if (!mPlaying) {
            if (ImGui::Button("Play")) {
                // Open video file
                const auto& camLap = mLaps[mCameraLapIdx];
                int aviIdx = mVbo.samples[camLap.startIdx].aviFileIndex;
                std::string videoPath = buildVideoFilename(mVbo, aviIdx);

                if (!mVideoCapture.isOpened() || !mVideoOpen) {
                    mVideoCapture.open(videoPath);
                    if (mVideoCapture.isOpened()) {
                        mVideoFps = mVideoCapture.get(cv::CAP_PROP_FPS);
                        if (mVideoFps <= 0) mVideoFps = 25.0;
                        mVideoOpen = true;
                        std::cout << "Opened video: " << videoPath
                                  << " (" << mVideoFps << " fps)" << std::endl;
                    } else {
                        std::cerr << "Cannot open video: " << videoPath << std::endl;
                    }
                }

                if (mVideoOpen) {
                    // Seek video to start of camera lap
                    mVideoCapture.set(cv::CAP_PROP_POS_MSEC, camLap.startAviTime);
                    double now = SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
                    mPlayStartTime = now;
                    mPlayElapsedSec = 0;
                    mNextFrameTime = now;
                    mPlaying = true;
                    mPaused = false;
                    showGhostCar();
                    if (mShowDrivingLine)
                        createDrivingLine(camLap);
                }
            }
        } else {
            if (ImGui::Button(mPaused ? "Resume" : "Pause")) {
                mPaused = !mPaused;
                if (!mPaused) {
                    double now = SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
                    mPlayStartTime = now - mPlayElapsedSec / mPlaybackSpeed;
                    mNextFrameTime = now;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop")) {
                mPlaying = false;
                mPaused = false;
                hideGhostCar();
                destroyDrivingLine();
            }

            // Progress bar
            double lapDuration = mLaps[mCameraLapIdx].lapTimeSeconds;
            float progress = (lapDuration > 0) ? (float)(mPlayElapsedSec / lapDuration) : 0;
            progress = std::clamp(progress, 0.0f, 1.0f);
            ImGui::ProgressBar(progress);

            int elMin = (int)(mPlayElapsedSec / 60.0);
            double elSec = mPlayElapsedSec - elMin * 60.0;
            ImGui::Text("Elapsed: %d:%05.2f / %.1f s", elMin, elSec, lapDuration);
        }

        if (ImGui::SliderFloat("Speed", &mPlaybackSpeed, 0.1f, 5.0f, "%.1fx")) {
            // Recalculate start time so elapsed stays consistent at new speed
            if (mPlaying && !mPaused) {
                double now = SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
                mPlayStartTime = now - mPlayElapsedSec / mPlaybackSpeed;
                mNextFrameTime = now;
            }
        }
    }

    // --- Ghost Car Settings ---
    if (ImGui::CollapsingHeader("Ghost Car Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SliderFloat("Alpha", &mGhostAlpha, 0.05f, 1.0f)) {
            for (auto* mi : mGhostMIs) {
                mi->setParameter("alpha", mGhostAlpha);
            }
        }

        ImGui::SliderFloat("Scale", &mGhostScale, 0.1f, 10.0f);
        ImGui::SliderFloat("Y Offset", &mGhostYOffset, -3.0f, 3.0f);
        ImGui::SliderFloat("Rotation Offset", &mModelRotOffset, -180.0f, 180.0f, "%.0f deg");
        ImGui::Checkbox("Pitch Compensation", &mPitchCompensation);
        if (ImGui::Checkbox("Show Driving Line", &mShowDrivingLine)) {
            if (mShowDrivingLine && mPlaying && mCameraLapIdx >= 0) {
                createDrivingLine(mLaps[mCameraLapIdx]);
            } else if (!mShowDrivingLine) {
                destroyDrivingLine();
            }
        }
    }

    // --- Camera Settings ---
    if (ImGui::CollapsingHeader("Camera")) {
        if (ImGui::SliderFloat("FOV (H)", &mCameraFovH, 30.0f, 150.0f, "%.0f deg")) {
            float aspect = (float)mWinWidth / (float)mWinHeight;
            mCamera->setProjection(mCameraFovH, aspect, 0.1f, 2000.0f, Camera::Fov::HORIZONTAL);
        }
        ImGui::SliderFloat("Height", &mCameraHeight, 0.5f, 3.0f, "%.1f m");
        ImGui::SliderFloat("Pitch", &mCameraPitchOffset, -10.0f, 10.0f, "%.1f deg");
        ImGui::SliderFloat("Pan", &mCameraPanOffset, -10.0f, 10.0f, "%.1f deg");
        ImGui::SliderFloat("Data Offset", &mDataOffsetMs, -500.0f, 500.0f, "%.0f ms");
    }

    // --- Settings ---
    ImGui::Separator();
    if (ImGui::Button("Save Settings")) saveSettings();
    ImGui::SameLine();
    if (ImGui::Button("Load Settings")) loadSettings();

    ImGui::End();
}

void GhostCarApp::run() {
    Uint64 lastTime = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();

    while (mRunning) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);

            switch (event.type) {
                case SDL_QUIT:
                    mRunning = false;
                    break;
                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_ESCAPE) mRunning = false;
                    else if (event.key.keysym.sym == SDLK_SPACE && mPlaying) {
                        mPaused = !mPaused;
                        if (!mPaused) {
                            double now = SDL_GetPerformanceCounter() / (double)freq;
                            mPlayStartTime = now - mPlayElapsedSec / mPlaybackSpeed;
                            mNextFrameTime = now;
                        }
                    }
                    break;
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_MOVED &&
                        event.window.windowID == mMainWindowID) {
                        updateSidebarLayout();
                    }
                    if (event.window.event == SDL_WINDOWEVENT_RESIZED &&
                        event.window.windowID == mMainWindowID) {
                        mWinWidth = event.window.data1;
                        mWinHeight = event.window.data2;
                        mView->setViewport({0, 0, (uint32_t)mWinWidth, (uint32_t)mWinHeight});
                        if (mBackgroundView)
                            mBackgroundView->setViewport({0, 0, (uint32_t)mWinWidth, (uint32_t)mWinHeight});
                        float aspect = (float)mWinWidth / (float)mWinHeight;
                        mCamera->setProjection(mCameraFovH, aspect, 0.1f, 2000.0f, Camera::Fov::HORIZONTAL);
                        updateSidebarLayout();
                    }
                    break;
            }
        }

        // --- Video playback & ghost car update ---
        bool newFrame = false;
        if (mPlaying && !mPaused && mVideoOpen) {
            double now = SDL_GetPerformanceCounter() / (double)freq;
            mPlayElapsedSec = (now - mPlayStartTime) * mPlaybackSpeed;

            const auto& camLap = mLaps[mCameraLapIdx];
            double lapDuration = camLap.lapTimeSeconds;

            if (mPlayElapsedSec >= lapDuration) {
                mPlaying = false;
                hideGhostCar();
                destroyDrivingLine();
            } else {
                // Pace video reads to match playback speed
                double frameDuration = 1.0 / (mVideoFps * mPlaybackSpeed);
                while (now >= mNextFrameTime) {
                    cv::Mat frame;
                    if (mVideoCapture.read(frame) && !frame.empty()) {
                        uploadVideoFrame(frame);
                        newFrame = true;
                    }
                    mNextFrameTime += frameDuration;
                }

                // Find camera car GPS position at this elapsed time (shifted by data offset)
                double dataElapsed = mPlayElapsedSec + mDataOffsetMs / 1000.0;
                dataElapsed = std::clamp(dataElapsed, 0.0, camLap.lapTimeSeconds);
                int camIdx = findSampleAtElapsed(mVbo, camLap.startIdx, camLap.endIdx,
                                                 dataElapsed);

                // Find ghost car GPS position at same elapsed time
                const auto& ghostLap = mLaps[mGhostLapIdx];
                double ghostElapsed = std::min(dataElapsed, ghostLap.lapTimeSeconds);
                int ghostIdx = findSampleAtElapsed(mVbo, ghostLap.startIdx, ghostLap.endIdx,
                                                   ghostElapsed);

                const auto& camSample = mVbo.samples[camIdx];
                const auto& ghostSample = mVbo.samples[ghostIdx];

                // Convert GPS to local meters
                double camEast, camNorth, ghostEast, ghostNorth;
                double refLat = camSample.latitude;
                double refLon = camSample.longitude;
                gpsToLocalMeters(camSample.latitude, camSample.longitude,
                                 refLat, refLon, camEast, camNorth);
                gpsToLocalMeters(ghostSample.latitude, ghostSample.longitude,
                                 refLat, refLon, ghostEast, ghostNorth);

                double dEast = ghostEast - camEast;   // = ghostEast since camEast=0
                double dNorth = ghostNorth - camNorth; // = ghostNorth since camNorth=0

                // Transform to camera-local coordinates
                double headRad = camSample.heading * M_PI / 180.0;
                double cosH = cos(headRad);
                double sinH = sin(headRad);

                // Camera forward in world: (sin(h), cos(h)) [east, north]
                // Camera right in world: (cos(h), -sin(h))
                double relRight = dEast * cosH - dNorth * sinH;
                double relForward = dEast * sinH + dNorth * cosH;

                // Filament: X=right, Y=up, -Z=forward
                float fx = (float)relRight;
                float fy = -mCameraHeight + mGhostYOffset;
                float fz = -(float)relForward;

                // Ghost car heading relative to camera
                double headingDiff = ghostSample.heading - camSample.heading;
                // Normalize to [-180, 180]
                while (headingDiff > 180.0) headingDiff -= 360.0;
                while (headingDiff < -180.0) headingDiff += 360.0;
                float ghostYRot = -(float)(headingDiff * M_PI / 180.0);

                // Compute ghost car pitch from height gradient
                float ghostPitch = mPitchCompensation ? computePitch(mVbo, ghostIdx) : 0.0f;

                if (mGhostVisible && relForward > 0) {
                    showGhostCar();
                    updateGhostCarTransform(fx, fy, fz, ghostYRot, ghostPitch);
                } else {
                    hideGhostCar();
                }

                // Pitch the 3D camera to match the camera car's road angle
                // so ghost car moves up/down in frame correctly on hills
                float camPitch = mPitchCompensation ? computePitch(mVbo, camIdx) : 0.0f;
                camPitch += mCameraPitchOffset * (float)M_PI / 180.0f;
                float camPan = mCameraPanOffset * (float)M_PI / 180.0f;
                float lookX = 100.0f * sinf(camPan) * cosf(camPitch);
                float lookY = mCameraHeight + 100.0f * sinf(camPitch);
                float lookZ = -100.0f * cosf(camPan) * cosf(camPitch);
                mCamera->lookAt(
                    {0, mCameraHeight, 0},
                    {lookX, lookY, lookZ},
                    {0, 1, 0});

                // Update driving line overlay
                if (mShowDrivingLine && mLineVB)
                    updateDrivingLine(camIdx);

                // Update title bar
                char title[512];
                snprintf(title, sizeof(title),
                    "GhostCar - Lap %d vs %d | %.1fs | dist=%.1fm | camPitch=%.1f° ghostPitch=%.1f°",
                    mLaps[mCameraLapIdx].lapNumber, mLaps[mGhostLapIdx].lapNumber,
                    mPlayElapsedSec,
                    sqrt(dEast * dEast + dNorth * dNorth),
                    camPitch * 180.0f / (float)M_PI,
                    ghostPitch * 180.0f / (float)M_PI);
                SDL_SetWindowTitle(mWindow, title);
            }
        }

        // --- Update overlays when paused (so settings changes are visible) ---
        if (mPlaying && mPaused && mVideoOpen && mCameraLapIdx >= 0 && mGhostLapIdx >= 0) {
            const auto& camLap = mLaps[mCameraLapIdx];
            double dataElapsed = mPlayElapsedSec + mDataOffsetMs / 1000.0;
            dataElapsed = std::clamp(dataElapsed, 0.0, camLap.lapTimeSeconds);
            int camIdx = findSampleAtElapsed(mVbo, camLap.startIdx, camLap.endIdx, dataElapsed);

            const auto& ghostLap = mLaps[mGhostLapIdx];
            double ghostElapsed = std::min(dataElapsed, ghostLap.lapTimeSeconds);
            int ghostIdx = findSampleAtElapsed(mVbo, ghostLap.startIdx, ghostLap.endIdx, ghostElapsed);

            const auto& camSample = mVbo.samples[camIdx];
            const auto& ghostSample = mVbo.samples[ghostIdx];

            double ghostEast, ghostNorth;
            gpsToLocalMeters(ghostSample.latitude, ghostSample.longitude,
                             camSample.latitude, camSample.longitude, ghostEast, ghostNorth);

            double headRad = camSample.heading * M_PI / 180.0;
            double cosH = cos(headRad);
            double sinH = sin(headRad);
            double relRight = ghostEast * cosH - ghostNorth * sinH;
            double relForward = ghostEast * sinH + ghostNorth * cosH;

            float fx = (float)relRight;
            float fy = -mCameraHeight + mGhostYOffset;
            float fz = -(float)relForward;

            double headingDiff = ghostSample.heading - camSample.heading;
            while (headingDiff > 180.0) headingDiff -= 360.0;
            while (headingDiff < -180.0) headingDiff += 360.0;
            float ghostYRot = -(float)(headingDiff * M_PI / 180.0);
            float ghostPitch = mPitchCompensation ? computePitch(mVbo, ghostIdx) : 0.0f;

            if (mGhostVisible && relForward > 0) {
                showGhostCar();
                updateGhostCarTransform(fx, fy, fz, ghostYRot, ghostPitch);
            } else {
                hideGhostCar();
            }

            float camPitch = mPitchCompensation ? computePitch(mVbo, camIdx) : 0.0f;
            camPitch += mCameraPitchOffset * (float)M_PI / 180.0f;
            float camPan = mCameraPanOffset * (float)M_PI / 180.0f;
            float lookX = 100.0f * sinf(camPan) * cosf(camPitch);
            float lookY = mCameraHeight + 100.0f * sinf(camPitch);
            float lookZ = -100.0f * cosf(camPan) * cosf(camPitch);
            mCamera->lookAt({0, mCameraHeight, 0}, {lookX, lookY, lookZ}, {0, 1, 0});

            if (mShowDrivingLine && mLineVB)
                updateDrivingLine(camIdx);
        }

        // --- Render ---
        if (mRenderer->beginFrame(mSwapChain)) {
            if (mBackgroundView) mRenderer->render(mBackgroundView);
            mRenderer->render(mView);
            mRenderer->endFrame();
        }

        // --- Render ImGui ---
        SDL_GL_MakeCurrent(mControlWindow, mGLContext);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        buildImGuiPanel();

        ImGui::Render();
        int dw, dh;
        SDL_GL_GetDrawableSize(mControlWindow, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(mControlWindow);

        // Frame pacing (~30fps when not playing video)
        if (!mPlaying || mPaused) {
            SDL_Delay(16);
        }
    }
}
