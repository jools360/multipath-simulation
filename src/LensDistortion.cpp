#include "LensDistortion.h"
#include "FilamentApp.h"
#include "SceneManager.h"

#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/RenderTarget.h>
#include <filament/View.h>
#include <filament/Scene.h>
#include <filament/Camera.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/RenderableManager.h>

#include <utils/EntityManager.h>

#include <opencv2/opencv.hpp>

// nlohmann/json workaround for Filament macro collision
#ifdef assert_invariant
#define FILAMENT_ASSERT_INVARIANT_SAVED
#undef assert_invariant
#endif
#include <nlohmann/json.hpp>
#ifdef FILAMENT_ASSERT_INVARIANT_SAVED
#include <utils/debug.h>
#undef FILAMENT_ASSERT_INVARIANT_SAVED
#endif

#include <fstream>
#include <iostream>
#include <sstream>

using namespace filament;
using namespace filament::math;
using namespace utils;

void distortion_loadCalibration(FilamentApp* app, const std::string& path) {
    using json = nlohmann::json;

    // Reset coefficients
    app->mDistK1 = app->mDistK2 = app->mDistK3 = 0.0;
    app->mDistP1 = app->mDistP2 = 0.0;
    app->mDistFx = app->mDistFy = 0.0;
    app->mDistCx = app->mDistCy = 0.0;
    app->mDistortionImageW = 1920;
    app->mDistortionImageH = 1080;
    app->mDistortionCameraName = "";

    // Detect format by extension
    std::string ext = path.substr(path.find_last_of('.') + 1);
    for (auto& c : ext) c = (char)tolower(c);

    std::ifstream in(path);
    if (!in) {
        std::cerr << "Failed to open calibration file: " << path << std::endl;
        return;
    }
    json j;
    try { in >> j; }
    catch (const json::parse_error& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
        return;
    }

    if (ext == "ulens") {
        // .ulens (Unreal Engine lens file)
        if (j.contains("ImageDimensions")) {
            app->mDistortionImageW = j["ImageDimensions"]["Width"].get<int>();
            app->mDistortionImageH = j["ImageDimensions"]["Height"].get<int>();
        }
        if (j.contains("Metadata") && j["Metadata"].contains("Name")) {
            app->mDistortionCameraName = j["Metadata"]["Name"].get<std::string>();
        }
        if (j.contains("CameraParameterTables")) {
            for (auto& table : j["CameraParameterTables"]) {
                std::string paramName = table["ParameterName"].get<std::string>();
                std::string data = table["Data"].get<std::string>();
                std::istringstream ss(data);
                std::vector<double> vals;
                std::string token;
                while (std::getline(ss, token, ',')) {
                    vals.push_back(std::stod(token));
                }
                if (paramName == "DistortionTable" && vals.size() >= 7) {
                    app->mDistK1 = vals[2];
                    app->mDistK2 = vals[3];
                    app->mDistK3 = vals[4];
                    app->mDistP1 = vals[5];
                    app->mDistP2 = vals[6];
                } else if (paramName == "ImageCenterTable" && vals.size() >= 4) {
                    app->mDistCx = vals[2];
                    app->mDistCy = vals[3];
                } else if (paramName == "FocalLengthTable" && vals.size() >= 4) {
                    if (app->mDistFx == 0.0) {
                        app->mDistFx = vals[2];
                        app->mDistFy = vals[3];
                    }
                }
            }
        }
    } else if (ext == "json") {
        // OpenCV calibration JSON
        if (j.contains("image_width")) app->mDistortionImageW = j["image_width"].get<int>();
        if (j.contains("image_height")) app->mDistortionImageH = j["image_height"].get<int>();
        if (j.contains("camera_name")) app->mDistortionCameraName = j["camera_name"].get<std::string>();
        double w = (double)app->mDistortionImageW;
        double h = (double)app->mDistortionImageH;
        if (j.contains("camera_matrix")) {
            auto& cm = j["camera_matrix"];
            if (cm.is_array() && cm.size() >= 9) {
                app->mDistFx = cm[0].get<double>() / w;
                app->mDistFy = cm[4].get<double>() / h;
                app->mDistCx = cm[2].get<double>() / w;
                app->mDistCy = cm[5].get<double>() / h;
            }
        }
        if (j.contains("dist_coeffs")) {
            auto& dc = j["dist_coeffs"];
            if (dc.is_array() && dc.size() >= 5) {
                app->mDistK1 = dc[0].get<double>();
                app->mDistK2 = dc[1].get<double>();
                app->mDistP1 = dc[2].get<double>();
                app->mDistP2 = dc[3].get<double>();
                app->mDistK3 = dc[4].get<double>();
            }
        }
    } else {
        std::cerr << "Unknown calibration format: " << ext << std::endl;
        return;
    }

    std::cout << "Loaded calibration: " << app->mDistortionCameraName
              << " K1=" << app->mDistK1 << " K2=" << app->mDistK2
              << " K3=" << app->mDistK3 << " P1=" << app->mDistP1
              << " P2=" << app->mDistP2
              << " Fx=" << app->mDistFx << " Fy=" << app->mDistFy
              << " Cx=" << app->mDistCx << " Cy=" << app->mDistCy
              << " " << app->mDistortionImageW << "x" << app->mDistortionImageH << std::endl;

    app->mDistortionCalibFile = path;
    app->mDistortionCalibLoaded = true;

    // Generate UV map from the loaded coefficients
    distortion_generateUVMap(app);
}

void distortion_generateUVMap(FilamentApp* app) {
    std::cout << "[DISTORTION] generateUVMap start" << std::endl;
    // Get the viewport dimensions for the UV map
    int w, h;
    SDL_GetWindowSize(app->mWindow, &w, &h);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    double imgW = (double)app->mDistortionImageW;
    double imgH = (double)app->mDistortionImageH;

    // Build OpenCV camera matrix from normalized parameters
    double fx = app->mDistFx * imgW;
    double fy = app->mDistFy * imgH;
    double cx = app->mDistCx * imgW;
    double cy = app->mDistCy * imgH;

    double k1 = app->mDistK1, k2 = app->mDistK2, k3 = app->mDistK3;
    double p1 = app->mDistP1, p2 = app->mDistP2;

    // Pass 1: Apply forward distortion model to every output pixel to find source UVs
    // and compute the bounding box of all source UVs (for overscan calculation).
    std::vector<float> rawU(w * h);
    std::vector<float> rawV(w * h);
    float minU = 1e9f, maxU = -1e9f, minV = 1e9f, maxV = -1e9f;

    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            double pixX = (double)px / (double)w * imgW;
            double pixY = (double)py / (double)h * imgH;
            double xn = (pixX - cx) / fx;
            double yn = (pixY - cy) / fy;

            double r2 = xn * xn + yn * yn;
            double r4 = r2 * r2;
            double r6 = r4 * r2;
            double radial = 1.0 + k1 * r2 + k2 * r4 + k3 * r6;
            double xd = xn * radial + 2.0 * p1 * xn * yn + p2 * (r2 + 2.0 * xn * xn);
            double yd = yn * radial + p1 * (r2 + 2.0 * yn * yn) + 2.0 * p2 * xn * yn;

            double srcX = xd * fx + cx;
            double srcY = yd * fy + cy;
            float u = (float)(srcX / imgW);
            float v = (float)(srcY / imgH);

            int i = py * w + px;
            rawU[i] = u;
            rawV[i] = v;
            if (u < minU) minU = u;
            if (u > maxU) maxU = u;
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
        }
    }

    // Store overscan bounds — the camera projection will be widened to cover this range
    app->mDistortionOverscanMinU = minU;
    app->mDistortionOverscanMaxU = maxU;
    app->mDistortionOverscanMinV = minV;
    app->mDistortionOverscanMaxV = maxV;

    float rangeU = maxU - minU;
    float rangeV = maxV - minV;

    std::cout << "  Distortion UV bounds: U=[" << minU << ", " << maxU
              << "] V=[" << minV << ", " << maxV << "]"
              << " overscan: " << rangeU << " x " << rangeV << std::endl;

    // Pass 2: Remap source UVs into [0,1] of the oversized render.
    // The camera projection will be widened to cover [minU,maxU] x [minV,maxV],
    // so the RT's [0,1] maps to this extended range.
    std::vector<float> uvData(w * h * 4);
    for (int i = 0; i < w * h; i++) {
        float u = (rawU[i] - minU) / rangeU;
        float v = (rawV[i] - minV) / rangeV;

        int idx = i * 4;
        uvData[idx + 0] = u;
        uvData[idx + 1] = v;
        uvData[idx + 2] = 1.0f;  // all pixels are now valid (RT covers full range)
        uvData[idx + 3] = 1.0f;
    }

    // Create or recreate the UV map texture
    if (app->mDistortionUVMap) {
        app->mEngine->destroy(app->mDistortionUVMap);
        app->mDistortionUVMap = nullptr;
    }

    app->mDistortionUVMap = Texture::Builder()
        .width(w)
        .height(h)
        .levels(1)
        .format(Texture::InternalFormat::RGBA32F)
        .usage(Texture::Usage::DEFAULT)
        .build(*app->mEngine);

    size_t dataSize = w * h * 4 * sizeof(float);
    float* buf = new float[w * h * 4];
    memcpy(buf, uvData.data(), dataSize);

    Texture::PixelBufferDescriptor pbd(
        buf, dataSize,
        Texture::Format::RGBA, Texture::Type::FLOAT,
        [](void* buffer, size_t, void*) { delete[] static_cast<float*>(buffer); });
    app->mDistortionUVMap->setImage(*app->mEngine, 0, std::move(pbd));

    app->mDistortionUVMapW = w;
    app->mDistortionUVMapH = h;

    std::cout << "Generated UV distortion map: " << w << "x" << h << std::endl;
}

void distortion_setup(FilamentApp* app) {
    std::cout << "[DISTORTION] setup start" << std::endl;
    // Get viewport dimensions
    int winW, winH;
    SDL_GetWindowSize(app->mWindow, &winW, &winH);
    int viewW = winW;
    int viewH = winH;
    if (viewW < 1) viewW = 1;

    // Create offscreen RT for the 3D scene
    app->mDistortionColor = Texture::Builder()
        .width(viewW)
        .height(viewH)
        .levels(1)
        .format(Texture::InternalFormat::RGBA8)
        .usage(Texture::Usage::COLOR_ATTACHMENT | Texture::Usage::SAMPLEABLE)
        .build(*app->mEngine);

    app->mDistortionDepth = Texture::Builder()
        .width(viewW)
        .height(viewH)
        .levels(1)
        .format(Texture::InternalFormat::DEPTH24)
        .usage(Texture::Usage::DEPTH_ATTACHMENT)
        .build(*app->mEngine);

    app->mDistortionRT = RenderTarget::Builder()
        .texture(RenderTarget::AttachmentPoint::COLOR, app->mDistortionColor)
        .texture(RenderTarget::AttachmentPoint::DEPTH, app->mDistortionDepth)
        .build(*app->mEngine);
    std::cout << "[DISTORTION] RT created" << std::endl;

    // Create dedicated offscreen view for rendering 3D scene to RT.
    // Post-processing ON, TRANSLUCENT blend. RT is cleared via Renderer::setClearOptions
    // before renderStandaloneView (set in render loop).
    app->mDistortionOffscreenView = app->mEngine->createView();
    app->mDistortionOffscreenView->setScene(app->mScene);
    app->mDistortionOffscreenView->setCamera(app->mCamera);
    app->mDistortionOffscreenView->setViewport({0, 0, (uint32_t)viewW, (uint32_t)viewH});
    app->mDistortionOffscreenView->setPostProcessingEnabled(true);
    app->mDistortionOffscreenView->setShadowingEnabled(true);
    app->mDistortionOffscreenView->setShadowType(View::ShadowType::PCF);
    app->mDistortionOffscreenView->setBlendMode(View::BlendMode::TRANSLUCENT);
    app->mDistortionOffscreenView->setRenderTarget(app->mDistortionRT);
    std::cout << "[DISTORTION] views created" << std::endl;

    // Create the distortion quad (full-screen)
    static TexturedVertex distVerts[4] = {
        {{-1.0f,  1.0f, 0.0f}, {0.0f, 1.0f}, {0, 0, 0, 32767}},
        {{ 1.0f,  1.0f, 0.0f}, {1.0f, 1.0f}, {0, 0, 0, 32767}},
        {{ 1.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {0, 0, 0, 32767}},
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {0, 0, 0, 32767}},
    };
    static uint16_t distIndices[6] = {0, 3, 2, 0, 2, 1};

    EntityManager& em = EntityManager::get();

    app->mDistortionVB = VertexBuffer::Builder()
        .vertexCount(4)
        .bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3,
                   offsetof(TexturedVertex, position), sizeof(TexturedVertex))
        .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2,
                   offsetof(TexturedVertex, uv), sizeof(TexturedVertex))
        .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::SHORT4,
                   offsetof(TexturedVertex, tangents), sizeof(TexturedVertex))
        .normalized(VertexAttribute::TANGENTS)
        .build(*app->mEngine);

    app->mDistortionVB->setBufferAt(*app->mEngine, 0,
        VertexBuffer::BufferDescriptor(distVerts, sizeof(distVerts)));

    app->mDistortionIB = IndexBuffer::Builder()
        .indexCount(6)
        .bufferType(IndexBuffer::IndexType::USHORT)
        .build(*app->mEngine);

    app->mDistortionIB->setBuffer(*app->mEngine,
        IndexBuffer::BufferDescriptor(distIndices, sizeof(distIndices)));

    // Load distortion material
    app->mDistortionMaterial = app->loadMaterial("lens_distortion.filamat");
    if (!app->mDistortionMaterial) {
        std::cerr << "ERROR: Failed to load lens_distortion material" << std::endl;
        return;
    }
    app->mDistortionMI = app->mDistortionMaterial->createInstance();

    // Set textures on the material instance
    TextureSampler sceneSampler;
    sceneSampler.setMagFilter(TextureSampler::MagFilter::LINEAR);
    sceneSampler.setMinFilter(TextureSampler::MinFilter::LINEAR);
    app->mDistortionMI->setParameter("sceneTexture", app->mDistortionColor, sceneSampler);

    TextureSampler uvSampler;
    uvSampler.setMagFilter(TextureSampler::MagFilter::NEAREST);
    uvSampler.setMinFilter(TextureSampler::MinFilter::NEAREST);
    app->mDistortionMI->setParameter("uvMap", app->mDistortionUVMap, uvSampler);
    app->mDistortionMI->setParameter("showGrid", app->mDistortionShowGrid ? 1.0f : 0.0f);
    std::cout << "[DISTORTION] material and textures set" << std::endl;

    // Create renderable entity
    app->mDistortionEntity = em.create();
    RenderableManager::Builder(1)
        .boundingBox({{-1, -1, -1}, {1, 1, 1}})
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                  app->mDistortionVB, app->mDistortionIB, 0, 6)
        .material(0, app->mDistortionMI)
        .culling(false)
        .receiveShadows(false)
        .castShadows(false)
        .build(*app->mEngine, app->mDistortionEntity);

    // Create distortion output scene and view (renders distortion quad to swap chain)
    app->mDistortionScene = app->mEngine->createScene();
    app->mDistortionScene->addEntity(app->mDistortionEntity);

    app->mDistortionCameraEntity = em.create();
    app->mDistortionCamera = app->mEngine->createCamera(app->mDistortionCameraEntity);
    app->mDistortionCamera->setProjection(Camera::Projection::ORTHO, -1, 1, -1, 1, 0, 10);
    app->mDistortionCamera->lookAt({0, 0, 1}, {0, 0, 0}, {0, 1, 0});

    app->mDistortionView = app->mEngine->createView();
    app->mDistortionView->setScene(app->mDistortionScene);
    app->mDistortionView->setCamera(app->mDistortionCamera);
    app->mDistortionView->setPostProcessingEnabled(false);
    app->mDistortionView->setShadowingEnabled(false);
    app->mDistortionView->setBlendMode(View::BlendMode::TRANSLUCENT);

    // Set viewport to full window (sidebar overlays on top, doesn't affect layout)
    app->mDistortionView->setViewport({0, 0, (uint32_t)viewW, (uint32_t)viewH});

    app->mDistortionSetup = true;
    std::cout << "Distortion pipeline set up: " << viewW << "x" << viewH << std::endl;
}

void distortion_teardown(FilamentApp* app) {
    if (!app->mDistortionSetup) return;


    if (app->mDistortionView) { app->mEngine->destroy(app->mDistortionView); app->mDistortionView = nullptr; }
    if (app->mDistortionScene) {
        app->mDistortionScene->remove(app->mDistortionEntity);
        app->mEngine->destroy(app->mDistortionScene);
        app->mDistortionScene = nullptr;
    }
    if (app->mDistortionCamera) {
        app->mEngine->destroyCameraComponent(app->mDistortionCameraEntity);
        EntityManager::get().destroy(app->mDistortionCameraEntity);
        app->mDistortionCamera = nullptr;
    }
    if (app->mDistortionEntity) {
        app->mEngine->getRenderableManager().destroy(app->mDistortionEntity);
        EntityManager::get().destroy(app->mDistortionEntity);
    }
    if (app->mDistortionMI) { app->mEngine->destroy(app->mDistortionMI); app->mDistortionMI = nullptr; }
    if (app->mDistortionMaterial) { app->mEngine->destroy(app->mDistortionMaterial); app->mDistortionMaterial = nullptr; }
    if (app->mDistortionVB) { app->mEngine->destroy(app->mDistortionVB); app->mDistortionVB = nullptr; }
    if (app->mDistortionIB) { app->mEngine->destroy(app->mDistortionIB); app->mDistortionIB = nullptr; }

    if (app->mDistortionOffscreenView) { app->mEngine->destroy(app->mDistortionOffscreenView); app->mDistortionOffscreenView = nullptr; }

    if (app->mDistortionRT) { app->mEngine->destroy(app->mDistortionRT); app->mDistortionRT = nullptr; }
    if (app->mDistortionColor) { app->mEngine->destroy(app->mDistortionColor); app->mDistortionColor = nullptr; }
    if (app->mDistortionDepth) { app->mEngine->destroy(app->mDistortionDepth); app->mDistortionDepth = nullptr; }

    if (app->mDistortionUVMap) { app->mEngine->destroy(app->mDistortionUVMap); app->mDistortionUVMap = nullptr; }

    app->mDistortionSetup = false;
    std::cout << "Distortion pipeline torn down" << std::endl;
}
