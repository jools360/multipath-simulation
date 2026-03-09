#include "GNSSApp.h"

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
#include <sstream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Simple vertex with position + tangents (required by Filament)
struct LineVertex {
    float position[3];
    int16_t tangents[4];
};

#ifdef _WIN32
static std::string openFileDialogGNSS(HWND owner, const char* title, const char* filter, const char* ext) {
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

GNSSApp::GNSSApp() = default;

GNSSApp::~GNSSApp() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (mGLContext) SDL_GL_DeleteContext(mGLContext);
    if (mControlWindow) SDL_DestroyWindow(mControlWindow);

    if (mEngine) {
        destroySignalLines();
        destroyTrajectoryLine();

        // Receiver marker
        if (!mReceiverEntity.isNull()) {
            mEngine->getRenderableManager().destroy(mReceiverEntity);
            EntityManager::get().destroy(mReceiverEntity);
        }
        if (mReceiverMI) mEngine->destroy(mReceiverMI);
        if (mReceiverVB) mEngine->destroy(mReceiverVB);
        if (mReceiverIB) mEngine->destroy(mReceiverIB);

        // Ground plane
        if (!mGroundEntity.isNull()) {
            mEngine->getRenderableManager().destroy(mGroundEntity);
            EntityManager::get().destroy(mGroundEntity);
        }
        if (mGroundMI) mEngine->destroy(mGroundMI);
        if (mGroundVB) mEngine->destroy(mGroundVB);
        if (mGroundIB) mEngine->destroy(mGroundIB);

        // Material instances
        if (mLOSMI) mEngine->destroy(mLOSMI);
        if (mBlockedMI) mEngine->destroy(mBlockedMI);
        if (mReflectedMI) mEngine->destroy(mReflectedMI);
        if (mLineMaterial) mEngine->destroy(mLineMaterial);

        // Building model
        if (mBuildingAsset) {
            mScene->removeEntities(mBuildingAsset->getEntities(), mBuildingAsset->getEntityCount());
            mAssetLoader->destroyAsset(mBuildingAsset);
        }
        if (mResourceLoader) delete mResourceLoader;
        if (mMaterialProvider) { mMaterialProvider->destroyMaterials(); delete mMaterialProvider; }
        if (mStbDecoder) delete mStbDecoder;
        if (mAssetLoader) filament::gltfio::AssetLoader::destroy(&mAssetLoader);

        // Core
        mEngine->destroyCameraComponent(mCameraEntity);
        EntityManager::get().destroy(mCameraEntity);
        EntityManager::get().destroy(mSunLight);
        if (mIndirectLight) mEngine->destroy(mIndirectLight);
        if (mSkybox) mEngine->destroy(mSkybox);

        mEngine->destroy(mView);
        mEngine->destroy(mScene);
        mEngine->destroy(mRenderer);
        mEngine->destroy(mSwapChain);
        Engine::destroy(&mEngine);
    }

    if (mWindow) SDL_DestroyWindow(mWindow);
    SDL_Quit();
}

Material* GNSSApp::loadMaterial(const std::string& name) {
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

bool GNSSApp::init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << std::endl;
        return false;
    }

    mWindow = SDL_CreateWindow("GNSS Multipath Simulation",
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
    fro.interval = 1; // vsync
    mRenderer->setFrameRateOptions(fro);

    Renderer::ClearOptions co;
    co.clearColor = {0.05f, 0.05f, 0.15f, 1.0f}; // dark blue
    co.clear = true;
    mRenderer->setClearOptions(co);

    mScene = mEngine->createScene();
    mView = mEngine->createView();
    mView->setScene(mScene);
    mView->setViewport({0, 0, (uint32_t)mWinWidth, (uint32_t)mWinHeight});
    mView->setShadowingEnabled(true);
    mView->setPostProcessingEnabled(true);

    // Skybox (solid color)
    mSkybox = Skybox::Builder().color({0.05f, 0.05f, 0.15f, 1.0f}).build(*mEngine);
    mScene->setSkybox(mSkybox);

    // Initialize gltfio for GLB loading
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

    // Load line material (reuse existing line.filamat from GhostCar)
    mLineMaterial = loadMaterial("line.filamat");
    if (mLineMaterial) {
        mLOSMI = mLineMaterial->createInstance();
        mLOSMI->setParameter("color", float3{0.1f, 1.0f, 0.2f}); // green
        mLOSMI->setParameter("alpha", 0.9f);

        mBlockedMI = mLineMaterial->createInstance();
        mBlockedMI->setParameter("color", float3{1.0f, 0.15f, 0.1f}); // red
        mBlockedMI->setParameter("alpha", 0.9f);

        mReflectedMI = mLineMaterial->createInstance();
        mReflectedMI->setParameter("color", float3{1.0f, 0.6f, 0.0f}); // orange
        mReflectedMI->setParameter("alpha", 0.9f);

        mReceiverMI = mLineMaterial->createInstance();
        mReceiverMI->setParameter("color", float3{1.0f, 0.0f, 0.0f}); // red
        mReceiverMI->setParameter("alpha", 1.0f);

        mGroundMI = mLineMaterial->createInstance();
        mGroundMI->setParameter("color", float3{0.25f, 0.25f, 0.25f}); // dark grey
        mGroundMI->setParameter("alpha", 0.4f);
    }

    // Create ground plane (large flat quad)
    {
        float sz = 600.0f;
        static LineVertex gVerts[4] = {
            {{-sz, 0, -sz}, {0,0,0,32767}},
            {{ sz, 0, -sz}, {0,0,0,32767}},
            {{ sz, 0,  sz}, {0,0,0,32767}},
            {{-sz, 0,  sz}, {0,0,0,32767}},
        };
        static uint16_t gIdx[6] = {0,2,1, 0,3,2};

        mGroundVB = VertexBuffer::Builder()
            .vertexCount(4).bufferCount(1)
            .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3,
                       offsetof(LineVertex, position), sizeof(LineVertex))
            .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::SHORT4,
                       offsetof(LineVertex, tangents), sizeof(LineVertex))
            .normalized(VertexAttribute::TANGENTS)
            .build(*mEngine);
        mGroundVB->setBufferAt(*mEngine, 0, VertexBuffer::BufferDescriptor(gVerts, sizeof(gVerts)));

        mGroundIB = IndexBuffer::Builder()
            .indexCount(6).bufferType(IndexBuffer::IndexType::USHORT).build(*mEngine);
        mGroundIB->setBuffer(*mEngine, IndexBuffer::BufferDescriptor(gIdx, sizeof(gIdx)));

        mGroundEntity = EntityManager::get().create();
        RenderableManager::Builder(1)
            .boundingBox({{-sz,-1,-sz},{sz,1,sz}})
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, mGroundVB, mGroundIB, 0, 6)
            .material(0, mGroundMI)
            .culling(false).castShadows(false).receiveShadows(false)
            .build(*mEngine, mGroundEntity);
        mScene->addEntity(mGroundEntity);
    }

    // Create receiver marker (red cube)
    {
        float s = 2.0f; // half-size of cube
        // 8 corners of the cube
        // Tangents encode normals — use a simple up-facing tangent for all verts
        // (unlit material so it doesn't matter for shading)
        static LineVertex rVerts[8] = {
            {{-s, -s, -s}, {0,0,0,32767}},  // 0: left  bottom back
            {{ s, -s, -s}, {0,0,0,32767}},  // 1: right bottom back
            {{ s, -s,  s}, {0,0,0,32767}},  // 2: right bottom front
            {{-s, -s,  s}, {0,0,0,32767}},  // 3: left  bottom front
            {{-s,  s, -s}, {0,0,0,32767}},  // 4: left  top    back
            {{ s,  s, -s}, {0,0,0,32767}},  // 5: right top    back
            {{ s,  s,  s}, {0,0,0,32767}},  // 6: right top    front
            {{-s,  s,  s}, {0,0,0,32767}},  // 7: left  top    front
        };
        // 12 triangles (6 faces x 2)
        static uint16_t rIdx[36] = {
            0,2,1, 0,3,2,  // bottom
            4,5,6, 4,6,7,  // top
            0,1,5, 0,5,4,  // back
            2,3,7, 2,7,6,  // front
            0,4,7, 0,7,3,  // left
            1,2,6, 1,6,5,  // right
        };

        mReceiverVB = VertexBuffer::Builder()
            .vertexCount(8).bufferCount(1)
            .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3,
                       offsetof(LineVertex, position), sizeof(LineVertex))
            .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::SHORT4,
                       offsetof(LineVertex, tangents), sizeof(LineVertex))
            .normalized(VertexAttribute::TANGENTS)
            .build(*mEngine);
        mReceiverVB->setBufferAt(*mEngine, 0, VertexBuffer::BufferDescriptor(rVerts, sizeof(rVerts)));

        mReceiverIB = IndexBuffer::Builder()
            .indexCount(36).bufferType(IndexBuffer::IndexType::USHORT).build(*mEngine);
        mReceiverIB->setBuffer(*mEngine, IndexBuffer::BufferDescriptor(rIdx, sizeof(rIdx)));

        mReceiverEntity = EntityManager::get().create();
        auto& tcm = mEngine->getTransformManager();
        tcm.create(mReceiverEntity);

        RenderableManager::Builder(1)
            .boundingBox({{-s,-s,-s},{s,s,s}})
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, mReceiverVB, mReceiverIB, 0, 36)
            .material(0, mReceiverMI)
            .culling(false).castShadows(false).receiveShadows(false)
            .build(*mEngine, mReceiverEntity);
        mScene->addEntity(mReceiverEntity);
    }

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
    mSidebarWidth = (int)(400 * dpiScale);

    int mainX, mainY;
    SDL_GetWindowPosition(mWindow, &mainX, &mainY);
    mControlWindow = SDL_CreateWindow("GNSS Controls",
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

    loadSettings();

    // Try to auto-load building model
    if (mBuildingModelPath[0]) {
        loadBuildingModel(mBuildingModelPath);
    }

    return true;
}

void GNSSApp::createCamera() {
    EntityManager& em = EntityManager::get();
    mCameraEntity = em.create();
    auto& tcm = mEngine->getTransformManager();
    tcm.create(mCameraEntity);
    mCamera = mEngine->createCamera(mCameraEntity);
    mView->setCamera(mCamera);

    float aspect = (float)mWinWidth / (float)mWinHeight;
    mCamera->setProjection(60.0f, aspect, 0.5f, 5000.0f);
}

void GNSSApp::createLighting() {
    EntityManager& em = EntityManager::get();
    mSunLight = em.create();

    LightManager::Builder(LightManager::Type::SUN)
        .color({1.0f, 0.95f, 0.9f})
        .intensity(80000.0f)
        .direction({-0.3f, -1.0f, -0.4f})
        .castShadows(true)
        .build(*mEngine, mSunLight);
    mScene->addEntity(mSunLight);

    // Simple ambient IBL
    static float3 sh[9] = {};
    sh[0] = {0.8f, 0.85f, 0.9f};

    mIndirectLight = IndirectLight::Builder()
        .irradiance(3, sh)
        .intensity(25000.0f)
        .build(*mEngine);
    mScene->setIndirectLight(mIndirectLight);
}

void GNSSApp::loadBuildingModel(const std::string& path) {
    // Try multiple paths
    std::vector<std::string> paths = {path};
#ifdef _WIN32
    char ep[MAX_PATH];
    GetModuleFileNameA(NULL, ep, MAX_PATH);
    std::string ed(ep);
    size_t s = ed.find_last_of("\\/");
    if (s != std::string::npos) {
        std::string exeDir = ed.substr(0, s + 1);
        paths.insert(paths.begin(), exeDir + path);
        // Also check parent directories (build/Release/../..)
        paths.push_back(exeDir + "..\\..\\" + path);
        paths.push_back(exeDir + "..\\" + path);
    }
#endif

    std::ifstream file;
    std::string foundPath;
    for (const auto& p : paths) {
        file.open(p, std::ios::binary);
        if (file) { foundPath = p; break; }
    }
    if (!file) {
        std::cerr << "Cannot find building model: " << path << std::endl;
        return;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    file.close();
    std::cout << "Loading building model: " << foundPath << " (" << data.size() << " bytes)" << std::endl;

    // Remove old asset if any
    if (mBuildingAsset) {
        mScene->removeEntities(mBuildingAsset->getEntities(), mBuildingAsset->getEntityCount());
        mAssetLoader->destroyAsset(mBuildingAsset);
        mBuildingAsset = nullptr;
    }

    // Load for rendering via Filament gltfio
    mBuildingAsset = mAssetLoader->createAsset(data.data(), (uint32_t)data.size());
    if (!mBuildingAsset) {
        std::cerr << "Failed to parse building GLB for rendering" << std::endl;
        return;
    }
    mResourceLoader->loadResources(mBuildingAsset);
    mBuildingAsset->releaseSourceData();
    mScene->addEntities(mBuildingAsset->getEntities(), mBuildingAsset->getEntityCount());

    auto aabb = mBuildingAsset->getBoundingBox();
    std::cout << "Building AABB: (" << aabb.min.x << "," << aabb.min.y << "," << aabb.min.z
              << ") to (" << aabb.max.x << "," << aabb.max.y << "," << aabb.max.z << ")" << std::endl;

    // Set camera target to model center
    float3 center = (aabb.min + aabb.max) * 0.5f;
    mModelCenter = {center.x, 0, center.z};
    mCameraTarget = mModelCenter;
    mReceiverPos = {center.x, mAntennaHeight, center.z};

    // Extract mesh for ray tracing (separate parse of same GLB data)
    std::vector<gnss::Triangle> triangles;
    if (gnss::extractMeshFromGLB(data.data(), data.size(), triangles)) {
        mBVH.build(triangles);
        mMeshLoaded = true;
        mNeedsRecompute = true;
        std::cout << "Mesh ready for ray tracing: " << triangles.size() << " triangles" << std::endl;
    } else {
        std::cerr << "Failed to extract mesh from GLB for ray tracing" << std::endl;
    }
}

void GNSSApp::analyzeSignals() {
    mSignals.clear();
    mLOSCount = 0;
    mBlockedCount = 0;
    mReflectedCount = 0;

    if (!mMeshLoaded || mSatPositions.empty()) return;

    for (const auto& sat : mSatPositions) {
        if (!sat.visible || !sat.healthy) continue;

        // Satellite direction in Filament coords (ENU → Filament: E→X, N→-Z, U→Y)
        float el = (float)sat.elevationRad;
        float az = (float)sat.azimuthRad;

        // Apply model north rotation
        float rotRad = mModelNorthRotation * (float)M_PI / 180.0f;
        float azRotated = az - rotRad;

        float3 satDir = {
            cosf(el) * sinf(azRotated),
            sinf(el),
            -cosf(el) * cosf(azRotated)
        };

        // LOS check: trace ray from receiver toward satellite
        auto losHit = mBVH.trace(mReceiverPos, satDir, 2000.0f);

        if (!losHit.hit) {
            // Clear line of sight
            SignalResult result;
            result.prn = sat.prn;
            result.type = SignalResult::LOS;
            result.satDirection = satDir;
            result.elevationDeg = (float)sat.elevationDeg;
            result.azimuthDeg = (float)sat.azimuthDeg;
            mSignals.push_back(result);
            mLOSCount++;
        } else {
            // Blocked
            SignalResult result;
            result.prn = sat.prn;
            result.type = SignalResult::BLOCKED;
            result.satDirection = satDir;
            result.hitPoint = losHit.point;
            result.elevationDeg = (float)sat.elevationDeg;
            result.azimuthDeg = (float)sat.azimuthDeg;
            mSignals.push_back(result);
            mBlockedCount++;
        }

        // Always check for reflections (both LOS and NLOS satellites can cause multipath)
        findReflections(satDir, sat.prn, (float)sat.elevationDeg, (float)sat.azimuthDeg);
    }
}

void GNSSApp::findReflections(const float3& satDir, int prn, float elDeg, float azDeg) {
    // For each triangle near the receiver, check if a single-bounce reflection
    // from the satellite direction reaches the receiver
    const auto& triangles = mBVH.triangles();
    float maxRange2 = mMaxReflectionRange * mMaxReflectionRange;

    for (int ti = 0; ti < (int)triangles.size(); ti++) {
        const auto& tri = triangles[ti];

        // Quick distance check (centroid to receiver)
        float3 centroid = (tri.v0 + tri.v1 + tri.v2) * (1.0f / 3.0f);
        float3 toReceiver = mReceiverPos - centroid;
        float dist2 = dot(toReceiver, toReceiver);
        if (dist2 > maxRange2) continue;

        // Triangle must face the satellite (normal has component toward satellite)
        float nDotSat = dot(tri.normal, satDir);
        if (nDotSat < 0.01f) continue;

        // Triangle must face the receiver
        float nDotRec = dot(tri.normal, normalize(toReceiver));
        if (nDotRec < 0.01f) continue;

        // Mirror the receiver about the triangle's plane
        // Plane: dot(x - v0, normal) = 0
        float d = dot(mReceiverPos - tri.v0, tri.normal);
        float3 mirrorPos = mReceiverPos - tri.normal * (2.0f * d);

        // Cast ray from mirror position in direction -satDir (incoming from satellite)
        // to find intersection with the triangle plane
        float denom = dot(satDir, tri.normal); // should be positive (checked above)
        if (fabsf(denom) < 1e-6f) continue;

        float t = dot(tri.v0 - mirrorPos, tri.normal) / (-denom);
        // The reflection point is where a ray from mirrorPos in direction -satDir hits the plane
        // But we want the ray from satellite direction, so use: P = mirrorPos + t * (-satDir)
        float3 P = mirrorPos - satDir * t;

        // Check if P is inside the triangle (barycentric test)
        float3 e0 = tri.v1 - tri.v0;
        float3 e1 = tri.v2 - tri.v0;
        float3 ep = P - tri.v0;

        float d00 = dot(e0, e0);
        float d01 = dot(e0, e1);
        float d11 = dot(e1, e1);
        float d20 = dot(ep, e0);
        float d21 = dot(ep, e1);
        float denom2 = d00 * d11 - d01 * d01;
        if (fabsf(denom2) < 1e-8f) continue;

        float baryV = (d11 * d20 - d01 * d21) / denom2;
        float baryW = (d00 * d21 - d01 * d20) / denom2;
        float baryU = 1.0f - baryV - baryW;

        if (baryU < -0.001f || baryV < -0.001f || baryW < -0.001f) continue;

        // Valid reflection point! Now verify paths are unobstructed

        // Path: reflection point → receiver
        float3 toRec = mReceiverPos - P;
        float distToRec = length(toRec);
        if (distToRec < 0.01f) continue;
        float3 dirToRec = toRec / distToRec;

        // Offset origin slightly off the surface to avoid self-intersection
        float3 offsetP = P + tri.normal * 0.05f;

        auto hitToRec = mBVH.trace(offsetP, dirToRec, distToRec - 0.1f);
        if (hitToRec.hit) continue; // path to receiver is blocked

        // Path: satellite → reflection point (check from P upward)
        auto hitFromSat = mBVH.trace(offsetP, satDir, 2000.0f);
        if (hitFromSat.hit) continue; // incoming path is blocked

        // Valid single-bounce reflection!
        SignalResult result;
        result.prn = prn;
        result.type = SignalResult::REFLECTED;
        result.satDirection = satDir;
        result.hitPoint = P; // reflection point
        result.reflectionNormal = tri.normal;
        result.elevationDeg = elDeg;
        result.azimuthDeg = azDeg;

        // Sky endpoint for visualization (where incoming ray enters from above)
        result.reflEndpoint = P + satDir * mDomeRadius;

        // Extra path length: |P-R| - dot(P-R, satDir)
        float3 v = P - mReceiverPos;
        result.extraPathM = length(v) - dot(v, satDir);

        mSignals.push_back(result);
        mReflectedCount++;
    }
}

void GNSSApp::destroySignalLines() {
    auto cleanup = [&](Entity& e, VertexBuffer*& vb, IndexBuffer*& ib) {
        if (!e.isNull()) {
            mScene->remove(e);
            mEngine->getRenderableManager().destroy(e);
            EntityManager::get().destroy(e);
            e = {};
        }
        if (vb) { mEngine->destroy(vb); vb = nullptr; }
        if (ib) { mEngine->destroy(ib); ib = nullptr; }
    };
    cleanup(mLOSEntity, mLOSVB, mLOSIB);
    cleanup(mBlockedEntity, mBlockedVB, mBlockedIB);
    cleanup(mReflectedEntity, mReflectedVB, mReflectedIB);
    mLOSVertexCount = mBlockedVertexCount = mReflectedVertexCount = 0;
}

// Build two perpendicular quads for a line segment so it's visible from any angle.
// Each segment produces 8 vertices and 4 triangles (12 indices).
static void buildSegmentQuads(float3 A, float3 B, float halfW,
                               std::vector<LineVertex>& outVerts,
                               std::vector<uint32_t>& outIdx) {
    float3 dir = B - A;
    float len = length(dir);
    if (len < 1e-6f) return;
    dir = dir / len;

    // First perpendicular: cross with up. If dir is nearly vertical, use right instead.
    float3 up = {0, 1, 0};
    float3 side1 = cross(dir, up);
    if (length(side1) < 0.01f) {
        side1 = cross(dir, float3{1, 0, 0});
    }
    side1 = normalize(side1) * halfW;

    // Second perpendicular (orthogonal to both dir and side1)
    float3 side2 = normalize(cross(dir, side1)) * halfW;

    uint32_t base = (uint32_t)outVerts.size();

    auto pushVert = [&](float3 p) {
        LineVertex v;
        v.position[0] = p.x; v.position[1] = p.y; v.position[2] = p.z;
        v.tangents[0] = 0; v.tangents[1] = 0; v.tangents[2] = 0; v.tangents[3] = 32767;
        outVerts.push_back(v);
    };

    // Quad 1 (using side1)
    pushVert(A - side1);  // base+0
    pushVert(A + side1);  // base+1
    pushVert(B + side1);  // base+2
    pushVert(B - side1);  // base+3

    // Quad 2 (using side2)
    pushVert(A - side2);  // base+4
    pushVert(A + side2);  // base+5
    pushVert(B + side2);  // base+6
    pushVert(B - side2);  // base+7

    // Two triangles per quad, double-sided material handles back faces
    outIdx.push_back(base+0); outIdx.push_back(base+1); outIdx.push_back(base+2);
    outIdx.push_back(base+0); outIdx.push_back(base+2); outIdx.push_back(base+3);
    outIdx.push_back(base+4); outIdx.push_back(base+5); outIdx.push_back(base+6);
    outIdx.push_back(base+4); outIdx.push_back(base+6); outIdx.push_back(base+7);
}

void GNSSApp::updateSignalLines() {
    destroySignalLines();
    if (mSignals.empty() || !mLineMaterial) return;

    float halfW = mLineWidth * 0.5f;

    // Collect segments by type: each segment is a pair (A, B)
    struct Segment { float3 a, b; };
    std::vector<Segment> losSegs, blockedSegs, reflectedSegs;

    for (const auto& sig : mSignals) {
        if (sig.type == SignalResult::LOS && mShowLOS) {
            losSegs.push_back({mReceiverPos, mReceiverPos + sig.satDirection * mDomeRadius});
        }
        else if (sig.type == SignalResult::BLOCKED && mShowBlocked) {
            blockedSegs.push_back({mReceiverPos, sig.hitPoint});
        }
        else if (sig.type == SignalResult::REFLECTED && mShowReflected) {
            reflectedSegs.push_back({sig.reflEndpoint, sig.hitPoint});
            reflectedSegs.push_back({sig.hitPoint, mReceiverPos});
        }
    }

    // Helper to create a triangle-based renderable from segments
    auto createLineRenderable = [&](const std::vector<Segment>& segs, MaterialInstance* mi,
                                     VertexBuffer*& vb, IndexBuffer*& ib, Entity& entity, int& vertCount) {
        if (segs.empty()) return;

        std::vector<LineVertex> verts;
        std::vector<uint32_t> indices;
        verts.reserve(segs.size() * 8);
        indices.reserve(segs.size() * 12);

        for (const auto& seg : segs) {
            buildSegmentQuads(seg.a, seg.b, halfW, verts, indices);
        }

        vertCount = (int)verts.size();
        int idxCount = (int)indices.size();
        if (vertCount == 0) return;

        auto* vertData = new LineVertex[vertCount];
        memcpy(vertData, verts.data(), vertCount * sizeof(LineVertex));

        auto* idxData = new uint32_t[idxCount];
        memcpy(idxData, indices.data(), idxCount * sizeof(uint32_t));

        vb = VertexBuffer::Builder()
            .vertexCount(vertCount).bufferCount(1)
            .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3,
                       offsetof(LineVertex, position), sizeof(LineVertex))
            .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::SHORT4,
                       offsetof(LineVertex, tangents), sizeof(LineVertex))
            .normalized(VertexAttribute::TANGENTS)
            .build(*mEngine);
        vb->setBufferAt(*mEngine, 0, VertexBuffer::BufferDescriptor(
            vertData, vertCount * sizeof(LineVertex),
            [](void* buf, size_t, void*) { delete[] static_cast<LineVertex*>(buf); }
        ));

        ib = IndexBuffer::Builder()
            .indexCount(idxCount).bufferType(IndexBuffer::IndexType::UINT).build(*mEngine);
        ib->setBuffer(*mEngine, IndexBuffer::BufferDescriptor(
            idxData, idxCount * sizeof(uint32_t),
            [](void* buf, size_t, void*) { delete[] static_cast<uint32_t*>(buf); }
        ));

        entity = EntityManager::get().create();
        RenderableManager::Builder(1)
            .boundingBox({{-2000,-500,-2000},{2000,500,2000}})
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, vb, ib, 0, idxCount)
            .material(0, mi)
            .culling(false).castShadows(false).receiveShadows(false)
            .build(*mEngine, entity);
        mScene->addEntity(entity);
    };

    createLineRenderable(losSegs, mLOSMI, mLOSVB, mLOSIB, mLOSEntity, mLOSVertexCount);
    createLineRenderable(blockedSegs, mBlockedMI, mBlockedVB, mBlockedIB, mBlockedEntity, mBlockedVertexCount);
    createLineRenderable(reflectedSegs, mReflectedMI, mReflectedVB, mReflectedIB, mReflectedEntity, mReflectedVertexCount);
}

// ============================================================
// Trajectory: KML loading, coordinate conversion, rendering
// ============================================================

float3 GNSSApp::latLonToModel(double lat, double lon) {
    constexpr double DEG_TO_M = M_PI / 180.0 * 6378137.0;
    double dLat = lat - mModelOriginLat;
    double dLon = lon - mModelOriginLon;

    float northM = (float)(dLat * DEG_TO_M);
    float eastM  = (float)(dLon * DEG_TO_M * cos(mModelOriginLat * M_PI / 180.0));

    // Apply north rotation (rotation around Y axis)
    float rotRad = mModelNorthRotation * (float)M_PI / 180.0f;
    float cosR = cosf(rotRad);
    float sinR = sinf(rotRad);

    // Default: east → +X, north → -Z (Filament convention)
    float modelX = eastM * cosR + northM * sinR;
    float modelZ = eastM * sinR - northM * cosR;

    return {modelX + mModelCenter.x, mAntennaHeight + mModelOriginHeight, modelZ + mModelCenter.z};
}

bool GNSSApp::loadKML(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Cannot open KML file: " << path << std::endl;
        return false;
    }

    // Read entire file
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    // Find <coordinates> ... </coordinates>
    size_t startTag = content.find("<coordinates>");
    size_t endTag = content.find("</coordinates>");
    if (startTag == std::string::npos || endTag == std::string::npos) {
        std::cerr << "No <coordinates> found in KML" << std::endl;
        return false;
    }
    startTag += 13; // skip "<coordinates>"

    std::string coordStr = content.substr(startTag, endTag - startTag);

    // Parse lon,lat,alt triples separated by whitespace
    mTrajectory.clear();
    std::istringstream iss(coordStr);
    std::string token;
    while (iss >> token) {
        // token is "lon,lat,alt"
        double lon = 0, lat = 0, alt = 0;
        size_t c1 = token.find(',');
        if (c1 == std::string::npos) continue;
        size_t c2 = token.find(',', c1 + 1);

        try {
            lon = std::stod(token.substr(0, c1));
            lat = std::stod(token.substr(c1 + 1, c2 != std::string::npos ? c2 - c1 - 1 : std::string::npos));
        } catch (...) { continue; }

        TrajectoryPoint pt;
        pt.lat = lat;
        pt.lon = lon;
        pt.modelPos = latLonToModel(lat, lon);
        pt.cumDist = 0;
        mTrajectory.push_back(pt);
    }

    if (mTrajectory.size() < 2) {
        std::cerr << "KML has fewer than 2 coordinate points" << std::endl;
        mTrajectory.clear();
        return false;
    }

    // Compute cumulative distances
    mTrajectory[0].cumDist = 0;
    for (size_t i = 1; i < mTrajectory.size(); i++) {
        float3 delta = mTrajectory[i].modelPos - mTrajectory[i-1].modelPos;
        delta.y = 0; // horizontal distance only
        mTrajectory[i].cumDist = mTrajectory[i-1].cumDist + length(delta);
    }
    mTrajectoryTotalDist = mTrajectory.back().cumDist;

    // Also update receiver lat/lon to the path centre for satellite computation
    double avgLat = 0, avgLon = 0;
    for (const auto& pt : mTrajectory) { avgLat += pt.lat; avgLon += pt.lon; }
    avgLat /= mTrajectory.size();
    avgLon /= mTrajectory.size();
    mReceiverLat = avgLat;
    mReceiverLon = avgLon;

    mTrajectoryLoaded = true;
    mPlaybackDist = 0;
    mPlaying = false;
    mPlaybackPaused = false;
    mNeedsRecompute = true;

    // Move receiver to start of trajectory
    mReceiverPos = mTrajectory[0].modelPos;

    std::cout << "Loaded KML trajectory: " << mTrajectory.size() << " points, "
              << mTrajectoryTotalDist << " m total" << std::endl;

    buildTrajectoryLine();
    return true;
}

void GNSSApp::destroyTrajectoryLine() {
    if (!mTrajectoryEntity.isNull()) {
        mScene->remove(mTrajectoryEntity);
        mEngine->getRenderableManager().destroy(mTrajectoryEntity);
        EntityManager::get().destroy(mTrajectoryEntity);
        mTrajectoryEntity = {};
    }
    if (mTrajectoryMI) { mEngine->destroy(mTrajectoryMI); mTrajectoryMI = nullptr; }
    if (mTrajectoryVB) { mEngine->destroy(mTrajectoryVB); mTrajectoryVB = nullptr; }
    if (mTrajectoryIB) { mEngine->destroy(mTrajectoryIB); mTrajectoryIB = nullptr; }
    mTrajectoryVertCount = 0;
}

void GNSSApp::buildTrajectoryLine() {
    destroyTrajectoryLine();
    if (mTrajectory.size() < 2 || !mLineMaterial) return;

    // Build quad geometry for each segment of the trajectory
    float halfW = mLineWidth * 0.5f;
    std::vector<LineVertex> verts;
    std::vector<uint32_t> indices;

    for (size_t i = 0; i + 1 < mTrajectory.size(); i++) {
        float3 a = mTrajectory[i].modelPos;
        float3 b = mTrajectory[i+1].modelPos;
        // Draw trajectory just above ground, offset by model origin height
        a.y = mModelOriginHeight + 0.1f;
        b.y = mModelOriginHeight + 0.1f;
        buildSegmentQuads(a, b, halfW, verts, indices);
    }

    mTrajectoryVertCount = (int)verts.size();
    int idxCount = (int)indices.size();
    if (mTrajectoryVertCount == 0) return;

    auto* vertData = new LineVertex[mTrajectoryVertCount];
    memcpy(vertData, verts.data(), mTrajectoryVertCount * sizeof(LineVertex));
    auto* idxData = new uint32_t[idxCount];
    memcpy(idxData, indices.data(), idxCount * sizeof(uint32_t));

    mTrajectoryVB = VertexBuffer::Builder()
        .vertexCount(mTrajectoryVertCount).bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3,
                   offsetof(LineVertex, position), sizeof(LineVertex))
        .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::SHORT4,
                   offsetof(LineVertex, tangents), sizeof(LineVertex))
        .normalized(VertexAttribute::TANGENTS)
        .build(*mEngine);
    mTrajectoryVB->setBufferAt(*mEngine, 0, VertexBuffer::BufferDescriptor(
        vertData, mTrajectoryVertCount * sizeof(LineVertex),
        [](void* buf, size_t, void*) { delete[] static_cast<LineVertex*>(buf); }
    ));

    mTrajectoryIB = IndexBuffer::Builder()
        .indexCount(idxCount).bufferType(IndexBuffer::IndexType::UINT).build(*mEngine);
    mTrajectoryIB->setBuffer(*mEngine, IndexBuffer::BufferDescriptor(
        idxData, idxCount * sizeof(uint32_t),
        [](void* buf, size_t, void*) { delete[] static_cast<uint32_t*>(buf); }
    ));

    mTrajectoryMI = mLineMaterial->createInstance();
    mTrajectoryMI->setParameter("color", float3{0.0f, 0.8f, 1.0f}); // cyan
    mTrajectoryMI->setParameter("alpha", 0.8f);

    mTrajectoryEntity = EntityManager::get().create();
    RenderableManager::Builder(1)
        .boundingBox({{-2000, -10, -2000}, {2000, 10, 2000}})
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, mTrajectoryVB, mTrajectoryIB, 0, idxCount)
        .material(0, mTrajectoryMI)
        .culling(false).castShadows(false).receiveShadows(false)
        .build(*mEngine, mTrajectoryEntity);
    mScene->addEntity(mTrajectoryEntity);
}

void GNSSApp::updateSidebarLayout() {
    int mainX, mainY;
    SDL_GetWindowPosition(mWindow, &mainX, &mainY);
    int di = SDL_GetWindowDisplayIndex(mWindow);
    SDL_Rect bounds;
    int sidebarHeight = mWinHeight;
    if (SDL_GetDisplayUsableBounds(di, &bounds) == 0) {
        sidebarHeight = bounds.h;
    }
    SDL_SetWindowPosition(mControlWindow, mainX - mSidebarWidth, bounds.y);
    SDL_SetWindowSize(mControlWindow, mSidebarWidth, sidebarHeight);
}

static std::string getSettingsPathGNSS() {
#ifdef _WIN32
    char ep[MAX_PATH];
    GetModuleFileNameA(NULL, ep, MAX_PATH);
    std::string d(ep);
    size_t s = d.find_last_of("\\/");
    if (s != std::string::npos) return d.substr(0, s + 1) + "gnss_settings.ini";
#endif
    return "gnss_settings.ini";
}

void GNSSApp::saveSettings() {
    std::ofstream f(getSettingsPathGNSS());
    if (!f) return;
    f << "almanacPath=" << mAlmanacPath << "\n";
    f << "buildingModelPath=" << mBuildingModelPath << "\n";
    f << "receiverLat=" << std::fixed << mReceiverLat << "\n";
    f << "receiverLon=" << std::fixed << mReceiverLon << "\n";
    f << "receiverAlt=" << mGroundElevation << "\n";
    f << "receiverHeight=" << mAntennaHeight << "\n";
    f << "year=" << mYear << "\n";
    f << "month=" << mMonth << "\n";
    f << "day=" << mDay << "\n";
    f << "hour=" << mHour << "\n";
    f << "minute=" << mMinute << "\n";
    f << "elevationMask=" << mElevationMask << "\n";
    f << "modelNorthRotation=" << mModelNorthRotation << "\n";
    f << "domeRadius=" << mDomeRadius << "\n";
    f << "maxReflectionRange=" << mMaxReflectionRange << "\n";
    f << "moveSpeed=" << mMoveSpeed << "\n";
    f << "lineWidth=" << mLineWidth << "\n";
    f << "kmlPath=" << mKmlPath << "\n";
    f << "modelOriginLat=" << std::fixed << mModelOriginLat << "\n";
    f << "modelOriginLon=" << std::fixed << mModelOriginLon << "\n";
    f << "modelOriginHeight=" << mModelOriginHeight << "\n";
    f << "playbackSpeed=" << mPlaybackSpeed << "\n";
    f << "followCamera=" << (mFollowCamera ? 1 : 0) << "\n";
    f << "showLOS=" << (mShowLOS ? 1 : 0) << "\n";
    f << "showBlocked=" << (mShowBlocked ? 1 : 0) << "\n";
    f << "showReflected=" << (mShowReflected ? 1 : 0) << "\n";
    std::cout << "Settings saved" << std::endl;
}

void GNSSApp::loadSettings() {
    std::ifstream f(getSettingsPathGNSS());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        try {
            if (key == "almanacPath") strncpy(mAlmanacPath, val.c_str(), sizeof(mAlmanacPath) - 1);
            else if (key == "buildingModelPath") strncpy(mBuildingModelPath, val.c_str(), sizeof(mBuildingModelPath) - 1);
            else if (key == "receiverLat") mReceiverLat = std::stod(val);
            else if (key == "receiverLon") mReceiverLon = std::stod(val);
            else if (key == "receiverAlt") mGroundElevation = std::stod(val);
            else if (key == "receiverHeight") mAntennaHeight = std::stof(val);
            else if (key == "year") mYear = std::stoi(val);
            else if (key == "month") mMonth = std::stoi(val);
            else if (key == "day") mDay = std::stoi(val);
            else if (key == "hour") mHour = std::stoi(val);
            else if (key == "minute") mMinute = std::stoi(val);
            else if (key == "elevationMask") mElevationMask = std::stof(val);
            else if (key == "modelNorthRotation") mModelNorthRotation = std::stof(val);
            else if (key == "domeRadius") mDomeRadius = std::stof(val);
            else if (key == "maxReflectionRange") mMaxReflectionRange = std::stof(val);
            else if (key == "moveSpeed") mMoveSpeed = std::stof(val);
            else if (key == "lineWidth") mLineWidth = std::stof(val);
            else if (key == "kmlPath") strncpy(mKmlPath, val.c_str(), sizeof(mKmlPath) - 1);
            else if (key == "modelOriginLat") mModelOriginLat = std::stod(val);
            else if (key == "modelOriginLon") mModelOriginLon = std::stod(val);
            else if (key == "modelOriginHeight") mModelOriginHeight = std::stof(val);
            else if (key == "playbackSpeed") mPlaybackSpeed = std::stof(val);
            else if (key == "followCamera") mFollowCamera = (val == "1");
            else if (key == "showLOS") mShowLOS = (val == "1");
            else if (key == "showBlocked") mShowBlocked = (val == "1");
            else if (key == "showReflected") mShowReflected = (val == "1");
        } catch (...) {}
    }
}

void GNSSApp::drawSkyPlot() {
    // Polar plot of satellite positions within the ImGui window
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float plotSize = std::min(avail.x - 10.0f, 280.0f);
    if (plotSize < 100) return;

    float radius = plotSize * 0.45f;
    ImVec2 center = ImGui::GetCursorScreenPos();
    center.x += plotSize * 0.5f;
    center.y += plotSize * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background circle
    dl->AddCircleFilled(center, radius, IM_COL32(20, 20, 40, 255), 64);

    // Elevation rings (30, 60 degrees)
    dl->AddCircle(center, radius * (1.0f - 30.0f/90.0f), IM_COL32(60,60,80,255), 64);
    dl->AddCircle(center, radius * (1.0f - 60.0f/90.0f), IM_COL32(60,60,80,255), 64);
    dl->AddCircle(center, radius, IM_COL32(100,100,120,255), 64);

    // Cross lines (N-S, E-W)
    dl->AddLine({center.x, center.y - radius}, {center.x, center.y + radius}, IM_COL32(60,60,80,255));
    dl->AddLine({center.x - radius, center.y}, {center.x + radius, center.y}, IM_COL32(60,60,80,255));

    // Cardinal labels
    dl->AddText({center.x - 3, center.y - radius - 16}, IM_COL32(200,200,200,255), "N");
    dl->AddText({center.x - 3, center.y + radius + 2}, IM_COL32(200,200,200,255), "S");
    dl->AddText({center.x + radius + 4, center.y - 7}, IM_COL32(200,200,200,255), "E");
    dl->AddText({center.x - radius - 12, center.y - 7}, IM_COL32(200,200,200,255), "W");

    // Elevation mask circle
    float maskRadius = radius * (1.0f - mElevationMask / 90.0f);
    dl->AddCircle(center, maskRadius, IM_COL32(100, 50, 50, 128), 64);

    // Plot satellites
    for (const auto& sat : mSatPositions) {
        if (!sat.healthy) continue;

        float r = radius * (1.0f - (float)sat.elevationDeg / 90.0f);
        float azRad = (float)(sat.azimuthDeg * M_PI / 180.0);
        // In the plot: up = North, right = East
        float px = center.x + r * sinf(azRad);
        float py = center.y - r * cosf(azRad);

        // Color based on signal status
        ImU32 col = IM_COL32(128, 128, 128, 255); // default grey (not visible)
        if (sat.visible) {
            // Find this satellite in signal results
            bool isLOS = false, isBlocked = false, isReflected = false;
            for (const auto& sig : mSignals) {
                if (sig.prn == sat.prn) {
                    if (sig.type == SignalResult::LOS) isLOS = true;
                    else if (sig.type == SignalResult::BLOCKED) isBlocked = true;
                    else if (sig.type == SignalResult::REFLECTED) isReflected = true;
                }
            }
            if (isLOS) col = IM_COL32(50, 255, 80, 255);
            else if (isBlocked) col = IM_COL32(255, 50, 50, 255);
            if (isReflected) {
                // Draw a small ring for reflected
                dl->AddCircle({px, py}, 8, IM_COL32(255, 160, 0, 255), 12, 2.0f);
            }
        }

        dl->AddCircleFilled({px, py}, 5, col, 12);

        // PRN label
        char label[8];
        snprintf(label, sizeof(label), "%d", sat.prn);
        dl->AddText({px + 7, py - 7}, IM_COL32(200,200,200,255), label);
    }

    // Reserve space for the plot
    ImGui::Dummy(ImVec2(plotSize, plotSize));
}

void GNSSApp::buildImGuiPanel() {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    int cw, ch;
    SDL_GetWindowSize(mControlWindow, &cw, &ch);
    ImGui::SetNextWindowSize(ImVec2((float)cw, (float)ch));
    ImGui::Begin("GNSS Multipath", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // --- Files ---
    if (ImGui::CollapsingHeader("Files", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Building Model:");
        ImGui::InputText("##model", mBuildingModelPath, sizeof(mBuildingModelPath));
        ImGui::SameLine();
        if (ImGui::Button("Browse##model")) {
#ifdef _WIN32
            std::string f = openFileDialogGNSS(NULL, "Open Building Model",
                "GLB Files (*.glb)\0*.glb\0All Files\0*.*\0", "glb");
            if (!f.empty()) {
                strncpy(mBuildingModelPath, f.c_str(), sizeof(mBuildingModelPath) - 1);
                loadBuildingModel(mBuildingModelPath);
            }
#endif
        }
        if (mBuildingAsset) {
            ImGui::TextColored(ImVec4(0,1,0,1), "Model loaded (%d triangles)", mBVH.triangleCount());
        }

        ImGui::Separator();
        ImGui::Text("YUMA Almanac:");
        ImGui::InputText("##alm", mAlmanacPath, sizeof(mAlmanacPath));
        ImGui::SameLine();
        if (ImGui::Button("Browse##alm")) {
#ifdef _WIN32
            std::string f = openFileDialogGNSS(NULL, "Open YUMA Almanac",
                "Almanac Files (*.alm;*.txt;*.yuma)\0*.alm;*.txt;*.yuma\0All Files\0*.*\0", "alm");
            if (!f.empty()) {
                strncpy(mAlmanacPath, f.c_str(), sizeof(mAlmanacPath) - 1);
                mAlmanac.clear();
                mAlmanacLoaded = parseYumaAlmanac(mAlmanacPath, mAlmanac);
                mNeedsRecompute = true;
            }
#endif
        }
        if (ImGui::Button("Load Almanac") && mAlmanacPath[0]) {
            mAlmanac.clear();
            mAlmanacLoaded = parseYumaAlmanac(mAlmanacPath, mAlmanac);
            mNeedsRecompute = true;
        }
        if (mAlmanacLoaded) {
            ImGui::TextColored(ImVec4(0,1,0,1), "%d satellites in almanac", (int)mAlmanac.size());
        }
    }

    // --- Receiver Position ---
    if (ImGui::CollapsingHeader("Receiver", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool changed = false;
        changed |= ImGui::InputDouble("Latitude", &mReceiverLat, 0.0001, 0.01, "%.6f");
        changed |= ImGui::InputDouble("Longitude", &mReceiverLon, 0.0001, 0.01, "%.6f");
        if (ImGui::Button("-##ah")) { mAntennaHeight = std::max(0.0f, mAntennaHeight - 1.0f); changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("+##ah")) { mAntennaHeight += 1.0f; changed = true; }
        ImGui::SameLine();
        changed |= ImGui::DragFloat("Antenna Height (m)", &mAntennaHeight, 0.5f, 0.0f, 500.0f, "%.1f");
        ImGui::SetItemTooltip("Height of antenna in the 3D scene. Use A/Z keys to move up/down.");
        changed |= ImGui::InputDouble("Ground Elevation (m)", &mGroundElevation, 1.0, 10.0, "%.1f");
        ImGui::SetItemTooltip("Ground height above WGS-84 ellipsoid (has negligible effect on satellite positions)");

        ImGui::Separator();
        ImGui::Text("Model Position: (%.1f, %.1f, %.1f)", mReceiverPos.x, mReceiverPos.y, mReceiverPos.z);
        ImGui::SliderFloat("Move Speed (m)", &mMoveSpeed, 0.5f, 50.0f);
        ImGui::Text("Arrow keys: move horizontally");
        ImGui::Text("A/Z: move up/down");

        if (changed) mNeedsRecompute = true;
    }

    // --- Time ---
    if (ImGui::CollapsingHeader("Date & Time (UTC)", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool changed = false;
        changed |= ImGui::InputInt("Year", &mYear);
        changed |= ImGui::InputInt("Month", &mMonth);
        changed |= ImGui::InputInt("Day", &mDay);
        changed |= ImGui::InputInt("Hour", &mHour);
        changed |= ImGui::InputInt("Minute", &mMinute);

        // Clamp values
        mMonth = std::clamp(mMonth, 1, 12);
        mDay = std::clamp(mDay, 1, 31);
        mHour = std::clamp(mHour, 0, 23);
        mMinute = std::clamp(mMinute, 0, 59);

        // Show GPS time
        GPSTime gps = utcToGPSTime(mYear, mMonth, mDay, mHour, mMinute, mSecond);
        ImGui::Text("GPS Week: %d  TOW: %.0f s", gps.week, gps.tow);

        changed |= ImGui::SliderFloat("Elevation Mask", &mElevationMask, 0, 30, "%.0f deg");

        if (changed) mNeedsRecompute = true;
    }

    // --- Model Settings ---
    if (ImGui::CollapsingHeader("Model Settings")) {
        bool changed = false;
        changed |= ImGui::SliderFloat("North Rotation", &mModelNorthRotation, -180, 180, "%.1f deg");
        changed |= ImGui::SliderFloat("Dome Radius (m)", &mDomeRadius, 50, 1000, "%.0f");
        changed |= ImGui::SliderFloat("Max Reflection Range (m)", &mMaxReflectionRange, 10, 500, "%.0f");
        if (changed) mNeedsRecompute = true;
    }

    // --- Trajectory ---
    if (ImGui::CollapsingHeader("Trajectory", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText("##kml", mKmlPath, sizeof(mKmlPath));
        ImGui::SameLine();
        if (ImGui::Button("Browse##kml")) {
#ifdef _WIN32
            std::string f = openFileDialogGNSS(NULL, "Open KML File",
                "KML Files (*.kml)\0*.kml\0All Files\0*.*\0", "kml");
            if (!f.empty()) {
                strncpy(mKmlPath, f.c_str(), sizeof(mKmlPath) - 1);
            }
#endif
        }
        if (mKmlPath[0] && ImGui::Button("Load KML")) {
            loadKML(mKmlPath);
        }

        if (mTrajectoryLoaded) {
            ImGui::Text("%d points, %.0f m total", (int)mTrajectory.size(), mTrajectoryTotalDist);

            ImGui::Separator();

            // Model origin alignment
            bool originChanged = false;
            originChanged |= ImGui::InputDouble("Model Origin Lat", &mModelOriginLat, 0.0001, 0.001, "%.6f");
            originChanged |= ImGui::InputDouble("Model Origin Lon", &mModelOriginLon, 0.0001, 0.001, "%.6f");
            if (ImGui::Button("-##moh")) { mModelOriginHeight -= 1.0f; originChanged = true; }
            ImGui::SameLine();
            if (ImGui::Button("+##moh")) { mModelOriginHeight += 1.0f; originChanged = true; }
            ImGui::SameLine();
            originChanged |= ImGui::DragFloat("Model Origin Height", &mModelOriginHeight, 0.1f, -500.0f, 500.0f, "%.1f m");
            if (originChanged) {
                // Recompute trajectory model positions
                for (auto& pt : mTrajectory) {
                    pt.modelPos = latLonToModel(pt.lat, pt.lon);
                }
                // Recompute cumulative distances
                mTrajectory[0].cumDist = 0;
                for (size_t i = 1; i < mTrajectory.size(); i++) {
                    float3 delta = mTrajectory[i].modelPos - mTrajectory[i-1].modelPos;
                    delta.y = 0;
                    mTrajectory[i].cumDist = mTrajectory[i-1].cumDist + length(delta);
                }
                mTrajectoryTotalDist = mTrajectory.back().cumDist;
                buildTrajectoryLine();
                mNeedsRecompute = true;
            }

            ImGui::Separator();

            // Playback controls
            ImGui::SliderFloat("Speed (m/s)", &mPlaybackSpeed, 0.5f, 50.0f, "%.1f");
            ImGui::Text("%.0f km/h", mPlaybackSpeed * 3.6f);

            float progress = mTrajectoryTotalDist > 0 ? mPlaybackDist / mTrajectoryTotalDist : 0;
            if (ImGui::SliderFloat("Progress", &progress, 0, 1, "%.2f")) {
                mPlaybackDist = progress * mTrajectoryTotalDist;
                // Interpolate position
                for (size_t i = 1; i < mTrajectory.size(); i++) {
                    if (mPlaybackDist <= mTrajectory[i].cumDist) {
                        float segLen = mTrajectory[i].cumDist - mTrajectory[i-1].cumDist;
                        float t = segLen > 0 ? (mPlaybackDist - mTrajectory[i-1].cumDist) / segLen : 0;
                        mReceiverPos = mTrajectory[i-1].modelPos + (mTrajectory[i].modelPos - mTrajectory[i-1].modelPos) * t;
                        mReceiverPos.y = mAntennaHeight + mModelOriginHeight;
                        break;
                    }
                }
                mNeedsRecompute = true;
            }

            ImGui::Text("Distance: %.0f / %.0f m", mPlaybackDist, mTrajectoryTotalDist);

            // Play/Pause/Stop buttons
            if (!mPlaying) {
                if (ImGui::Button("Play", ImVec2(80, 0))) {
                    mPlaying = true;
                    mPlaybackPaused = false;
                    mLastPlaybackTime = 0;
                    if (mPlaybackDist >= mTrajectoryTotalDist) mPlaybackDist = 0;
                }
            } else {
                if (mPlaybackPaused) {
                    if (ImGui::Button("Resume", ImVec2(80, 0))) {
                        mPlaybackPaused = false;
                        mLastPlaybackTime = 0;
                    }
                } else {
                    if (ImGui::Button("Pause", ImVec2(80, 0))) {
                        mPlaybackPaused = true;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop", ImVec2(80, 0))) {
                mPlaying = false;
                mPlaybackPaused = false;
                mPlaybackDist = 0;
                mReceiverPos = mTrajectory[0].modelPos;
                mReceiverPos.y = mAntennaHeight + mModelOriginHeight;
                mNeedsRecompute = true;
            }
            ImGui::SameLine();
            ImGui::Checkbox("Follow Camera", &mFollowCamera);
        }
    }

    // --- Recompute button ---
    if (ImGui::Button("Recompute Signals", ImVec2(-1, 30))) {
        mNeedsRecompute = true;
    }

    // --- Display options ---
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool changed = false;
        changed |= ImGui::Checkbox("Show LOS (green)", &mShowLOS);
        changed |= ImGui::Checkbox("Show Blocked (red)", &mShowBlocked);
        changed |= ImGui::Checkbox("Show Reflected (orange)", &mShowReflected);
        changed |= ImGui::SliderFloat("Line Width (m)", &mLineWidth, 0.1f, 10.0f, "%.1f");
        if (changed) updateSignalLines();
    }

    // --- Statistics ---
    if (ImGui::CollapsingHeader("Signal Analysis", ImGuiTreeNodeFlags_DefaultOpen)) {
        int visCount = 0;
        for (const auto& s : mSatPositions) if (s.visible && s.healthy) visCount++;
        ImGui::Text("Visible satellites: %d / %d", visCount, (int)mSatPositions.size());
        ImGui::TextColored(ImVec4(0.1f,1,0.2f,1), "LOS: %d", mLOSCount);
        ImGui::TextColored(ImVec4(1,0.15f,0.1f,1), "Blocked: %d", mBlockedCount);
        ImGui::TextColored(ImVec4(1,0.6f,0,1), "Reflected: %d", mReflectedCount);

        ImGui::Separator();
        // Satellite table
        if (ImGui::BeginTable("sats", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY, ImVec2(0, 150))) {
            ImGui::TableSetupColumn("PRN", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Az", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("El", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Delay", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            for (const auto& sig : mSignals) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", sig.prn);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", sig.azimuthDeg);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", sig.elevationDeg);
                ImGui::TableNextColumn();
                if (sig.type == SignalResult::LOS)
                    ImGui::TextColored(ImVec4(0.1f,1,0.2f,1), "LOS");
                else if (sig.type == SignalResult::BLOCKED)
                    ImGui::TextColored(ImVec4(1,0.15f,0.1f,1), "NLOS");
                else
                    ImGui::TextColored(ImVec4(1,0.6f,0,1), "REFL");
                ImGui::TableNextColumn();
                if (sig.type == SignalResult::REFLECTED)
                    ImGui::Text("%.1fm", sig.extraPathM);
                else
                    ImGui::Text("-");
            }
            ImGui::EndTable();
        }
    }

    // --- Sky Plot ---
    if (ImGui::CollapsingHeader("Sky Plot", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawSkyPlot();
    }

    // --- Save/Load ---
    ImGui::Separator();
    if (ImGui::Button("Save Settings")) saveSettings();
    ImGui::SameLine();
    if (ImGui::Button("Load Settings")) { loadSettings(); mNeedsRecompute = true; }

    ImGui::End();
}

void GNSSApp::run() {
    while (mRunning) {
        // --- Event handling ---
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // Route to ImGui if for sidebar
            if (event.type == SDL_WINDOWEVENT || event.type == SDL_MOUSEMOTION ||
                event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP ||
                event.type == SDL_MOUSEWHEEL || event.type == SDL_TEXTINPUT ||
                event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                Uint32 winID = 0;
                if (event.type == SDL_WINDOWEVENT) winID = event.window.windowID;
                else if (event.type == SDL_MOUSEMOTION) winID = event.motion.windowID;
                else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP)
                    winID = event.button.windowID;
                else if (event.type == SDL_MOUSEWHEEL) winID = event.wheel.windowID;
                else if (event.type == SDL_TEXTINPUT) winID = event.text.windowID;
                else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
                    winID = event.key.windowID;

                if (winID == mControlWindowID) {
                    ImGui_ImplSDL2_ProcessEvent(&event);
                    continue;
                }
            }

            if (event.type == SDL_QUIT) {
                mRunning = false;
            }
            else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    mRunning = false;
                }
                else if (event.window.event == SDL_WINDOWEVENT_RESIZED &&
                         event.window.windowID == mMainWindowID) {
                    mWinWidth = event.window.data1;
                    mWinHeight = event.window.data2;
                    mView->setViewport({0, 0, (uint32_t)mWinWidth, (uint32_t)mWinHeight});
                    float aspect = (float)mWinWidth / (float)mWinHeight;
                    mCamera->setProjection(60.0f, aspect, 0.5f, 5000.0f);
                }
                else if (event.window.event == SDL_WINDOWEVENT_MOVED &&
                         event.window.windowID == mMainWindowID) {
                    updateSidebarLayout();
                }
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.windowID == mMainWindowID) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    mDragging = true;
                    mLastMouseX = event.button.x;
                    mLastMouseY = event.button.y;
                }
                else if (event.button.button == SDL_BUTTON_MIDDLE) {
                    mPanning = true;
                    mLastMouseX = event.button.x;
                    mLastMouseY = event.button.y;
                }
            }
            else if (event.type == SDL_MOUSEBUTTONUP) {
                if (event.button.button == SDL_BUTTON_LEFT) mDragging = false;
                if (event.button.button == SDL_BUTTON_MIDDLE) mPanning = false;
            }
            else if (event.type == SDL_MOUSEMOTION && event.motion.windowID == mMainWindowID) {
                int dx = event.motion.x - mLastMouseX;
                int dy = event.motion.y - mLastMouseY;
                mLastMouseX = event.motion.x;
                mLastMouseY = event.motion.y;

                if (mDragging) {
                    mCameraYaw -= dx * 0.005f;
                    mCameraPitch -= dy * 0.005f;
                    mCameraPitch = std::clamp(mCameraPitch, -1.55f, -0.05f);
                }
                if (mPanning) {
                    // Pan camera target
                    float panScale = mCameraDistance * 0.001f;
                    float cosYaw = cosf(mCameraYaw);
                    float sinYaw = sinf(mCameraYaw);
                    mCameraTarget.x += (cosYaw * dx + sinYaw * dy) * panScale;
                    mCameraTarget.z += (-sinYaw * dx + cosYaw * dy) * panScale;
                }
            }
            else if (event.type == SDL_MOUSEWHEEL && event.wheel.windowID == mMainWindowID) {
                mCameraDistance *= (event.wheel.y > 0) ? 0.9f : 1.1f;
                mCameraDistance = std::clamp(mCameraDistance, 10.0f, 3000.0f);
            }
            else if (event.type == SDL_KEYDOWN && event.key.windowID == mMainWindowID) {
                auto key = event.key.keysym.sym;
                bool moved = false;

                // Receiver movement with arrow keys
                float cosYaw = cosf(mCameraYaw);
                float sinYaw = sinf(mCameraYaw);

                if (key == SDLK_LEFT)  { mReceiverPos.x -= mMoveSpeed * cosYaw; mReceiverPos.z += mMoveSpeed * sinYaw; moved = true; }
                if (key == SDLK_RIGHT) { mReceiverPos.x += mMoveSpeed * cosYaw; mReceiverPos.z -= mMoveSpeed * sinYaw; moved = true; }
                if (key == SDLK_UP)    { mReceiverPos.x -= mMoveSpeed * sinYaw; mReceiverPos.z -= mMoveSpeed * cosYaw; moved = true; }
                if (key == SDLK_DOWN)  { mReceiverPos.x += mMoveSpeed * sinYaw; mReceiverPos.z += mMoveSpeed * cosYaw; moved = true; }
                if (key == SDLK_a)     { mAntennaHeight += mMoveSpeed; moved = true; }
                if (key == SDLK_z)     { mAntennaHeight -= mMoveSpeed; if (mAntennaHeight < 0) mAntennaHeight = 0; moved = true; }
                if (key == SDLK_ESCAPE) mRunning = false;
                if (key == SDLK_TAB) {
                    // Toggle sidebar
                    if (SDL_GetWindowFlags(mControlWindow) & SDL_WINDOW_SHOWN)
                        SDL_HideWindow(mControlWindow);
                    else
                        SDL_ShowWindow(mControlWindow);
                }

                if (moved) {
                    mReceiverPos.y = mAntennaHeight + mModelOriginHeight;
                    mNeedsRecompute = true;
                }
            }
        }

        // --- Trajectory playback advancement ---
        if (mPlaying && !mPlaybackPaused && mTrajectoryLoaded && mTrajectory.size() >= 2) {
            double now = SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
            if (mLastPlaybackTime > 0) {
                double dt = now - mLastPlaybackTime;
                mPlaybackDist += mPlaybackSpeed * (float)dt;

                // Clamp / wrap
                if (mPlaybackDist >= mTrajectoryTotalDist) {
                    mPlaybackDist = mTrajectoryTotalDist;
                    mPlaying = false;
                }

                // Interpolate position along trajectory
                for (size_t i = 1; i < mTrajectory.size(); ++i) {
                    if (mPlaybackDist <= mTrajectory[i].cumDist || i == mTrajectory.size() - 1) {
                        float segLen = mTrajectory[i].cumDist - mTrajectory[i-1].cumDist;
                        float t = (segLen > 0.001f) ? (mPlaybackDist - mTrajectory[i-1].cumDist) / segLen : 0.0f;
                        t = std::clamp(t, 0.0f, 1.0f);
                        float3 p0 = mTrajectory[i-1].modelPos;
                        float3 p1 = mTrajectory[i].modelPos;
                        mReceiverPos.x = p0.x + t * (p1.x - p0.x);
                        mReceiverPos.z = p0.z + t * (p1.z - p0.z);
                        mReceiverPos.y = mAntennaHeight;

                        // Interpolate lat/lon for satellite computation
                        mReceiverLat = mTrajectory[i-1].lat + t * (mTrajectory[i].lat - mTrajectory[i-1].lat);
                        mReceiverLon = mTrajectory[i-1].lon + t * (mTrajectory[i].lon - mTrajectory[i-1].lon);
                        break;
                    }
                }

                // Follow camera
                if (mFollowCamera) {
                    mCameraTarget = mReceiverPos;
                }

                mNeedsRecompute = true;
            }
            mLastPlaybackTime = now;
        }

        // --- Recompute signals if needed ---
        if (mNeedsRecompute && mAlmanacLoaded && mMeshLoaded) {
            GPSTime gps = utcToGPSTime(mYear, mMonth, mDay, mHour, mMinute, mSecond);
            computeSatellitePositions(mAlmanac, gps, mReceiverLat, mReceiverLon,
                                     mGroundElevation + mAntennaHeight,
                                     mElevationMask, mSatPositions);
            analyzeSignals();
            updateSignalLines();
            mNeedsRecompute = false;
        }

        // --- Update receiver marker transform ---
        {
            auto& tcm = mEngine->getTransformManager();
            auto ti = tcm.getInstance(mReceiverEntity);
            if (ti) {
                tcm.setTransform(ti, mat4f::translation(mReceiverPos));
            }
        }

        // --- Update camera ---
        {
            float3 eye;
            eye.x = mCameraTarget.x + mCameraDistance * cosf(mCameraPitch) * sinf(mCameraYaw);
            eye.y = mCameraTarget.y - mCameraDistance * sinf(mCameraPitch);
            eye.z = mCameraTarget.z + mCameraDistance * cosf(mCameraPitch) * cosf(mCameraYaw);
            mCamera->lookAt(eye, mCameraTarget, {0, 1, 0});
        }

        // --- Render 3D scene ---
        if (mRenderer->beginFrame(mSwapChain)) {
            mRenderer->render(mView);
            mRenderer->endFrame();
        }

        // --- Render ImGui sidebar ---
        SDL_GL_MakeCurrent(mControlWindow, mGLContext);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        buildImGuiPanel();
        ImGui::Render();

        int fbW, fbH;
        SDL_GL_GetDrawableSize(mControlWindow, &fbW, &fbH);
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(mControlWindow);
    }
}
