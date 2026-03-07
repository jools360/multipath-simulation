#include "FilamentApp.h"
#include "SceneManager.h"
#include "LensDistortion.h"

#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/LightManager.h>

// nlohmann/json uses a method called assert_invariant() which conflicts with
// Filament's macro of the same name (from utils/debug.h). Temporarily undefine it.
#ifdef assert_invariant
#define FILAMENT_ASSERT_INVARIANT_SAVED
#undef assert_invariant
#endif
#include <nlohmann/json.hpp>
#ifdef FILAMENT_ASSERT_INVARIANT_SAVED
#include <utils/debug.h>
#undef FILAMENT_ASSERT_INVARIANT_SAVED
#endif

#ifdef _WIN32
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#endif

#include <fstream>
#include <iostream>

std::string scene_exeDirPath(const std::string& filename) {
#ifdef _WIN32
    // If already an absolute path, return as-is
    if (filename.size() >= 2 && filename[1] == ':') return filename;
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    size_t lastSlash = dir.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        return dir.substr(0, lastSlash + 1) + filename;
    }
#endif
    return filename;
}

std::string scene_resolveFilePath(const std::string& filename) {
    std::string exePath = scene_exeDirPath(filename);
    if (std::ifstream(exePath).good()) return exePath;
    if (std::ifstream(filename).good()) return filename;
    if (std::ifstream("./" + filename).good()) return "./" + filename;
    return "";
}

#ifdef _WIN32
std::string openFileDialog(HWND owner, const char* title, const char* filter, const char* defaultExt) {
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defaultExt;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return std::string(filename);
    return "";
}

std::string saveFileDialog(HWND owner, const char* title, const char* filter, const char* defaultExt) {
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defaultExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) return std::string(filename);
    return "";
}
#endif

filament::gltfio::FilamentAsset* scene_loadGlbModel(FilamentApp* app, const std::string& filename) {
    std::vector<std::string> searchPaths = {
        filename,
        "./" + filename
    };

#ifdef _WIN32
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir(exePath);
    size_t lastSlash = exeDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        exeDir = exeDir.substr(0, lastSlash + 1);
        searchPaths.insert(searchPaths.begin(), exeDir + filename);
    }
#endif

    std::ifstream file;
    for (const auto& path : searchPaths) {
        file.open(path, std::ios::binary);
        if (file) {
            std::cout << "Found GLB at: " << path << std::endl;
            break;
        }
    }

    if (!file) {
        std::cerr << "Could not find GLB file: " << filename << std::endl;
        return nullptr;
    }

    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>()
    );
    std::cout << "GLB size: " << data.size() << " bytes" << std::endl;

    auto* asset = app->mAssetLoader->createAsset(data.data(), static_cast<uint32_t>(data.size()));
    if (!asset) {
        std::cerr << "Failed to parse GLB: " << filename << std::endl;
        return nullptr;
    }

    app->mResourceLoader->loadResources(asset);
    asset->releaseSourceData();

    auto aabb = asset->getBoundingBox();
    std::cout << "  AABB min: (" << aabb.min.x << ", " << aabb.min.y << ", " << aabb.min.z << ")" << std::endl;
    std::cout << "  AABB max: (" << aabb.max.x << ", " << aabb.max.y << ", " << aabb.max.z << ")" << std::endl;
    float3 extent = aabb.max - aabb.min;
    std::cout << "  Size: (" << extent.x << ", " << extent.y << ", " << extent.z << ")" << std::endl;

    app->mScene->addEntities(asset->getEntities(), asset->getEntityCount());

    std::cout << "Loaded GLB model: " << filename << " (" << asset->getEntityCount() << " entities)" << std::endl;
    return asset;
}

void scene_updateObjectTransform(FilamentApp* app, SceneObject& obj) {
    if (!obj.asset) return;
    auto& tcm = app->mEngine->getTransformManager();
    auto ti = tcm.getInstance(obj.asset->getRoot());
    if (!ti) return;
    const float HALF_PI = 3.14159265358979f * 0.5f;
    mat4f transform = mat4f::translation(obj.position) *
                      mat4f::rotation(obj.yRotation, float3{0, 1, 0}) *
                      mat4f::rotation(obj.rot90[1] * HALF_PI, float3{0, 1, 0}) *
                      mat4f::rotation(obj.rot90[0] * HALF_PI, float3{1, 0, 0}) *
                      mat4f::rotation(obj.rot90[2] * HALF_PI, float3{0, 0, 1}) *
                      mat4f::scaling(float3{obj.scale, obj.scale, obj.scale}) *
                      mat4f::translation(-obj.aabbCenterOffset);
    tcm.setTransform(ti, transform);
}

void scene_loadGlbObject(FilamentApp* app, SceneObject& obj) {
    obj.asset = scene_loadGlbModel(app, obj.glbFilename);
    if (obj.asset) {
        scene_updateObjectTransform(app, obj);
        if (!obj.visible) {
            auto& rm = app->mEngine->getRenderableManager();
            const Entity* entities = obj.asset->getEntities();
            size_t count = obj.asset->getEntityCount();
            for (size_t i = 0; i < count; i++) {
                auto ri = rm.getInstance(entities[i]);
                if (ri) {
                    rm.setLayerMask(ri, 0x1, 0x0);
                }
            }
        }
    }
}

void scene_destroyAllGlbObjects(FilamentApp* app) {
    for (auto& obj : app->mGlbObjects) {
        if (obj.asset) {
            app->mScene->removeEntities(obj.asset->getEntities(), obj.asset->getEntityCount());
            app->mAssetLoader->destroyAsset(obj.asset);
            obj.asset = nullptr;
        }
    }
    app->mGlbObjects.clear();
}

void scene_save(FilamentApp* app, const std::string& filename) {
    using json = nlohmann::json;
    json j;
    j["version"] = 1;

    j["camera"] = {
        {"distance", app->mCameraDistance},
        {"yaw", app->mCameraYaw},
        {"pitch", app->mCameraPitch},
        {"target", {app->mCameraTarget.x, app->mCameraTarget.y, app->mCameraTarget.z}},
        {"autoOrbit", app->mAutoOrbit}
    };

    j["lighting"] = {
        {"sunIntensity", app->mSunIntensity},
        {"sunYaw", app->mLightYaw},
        {"sunPitch", app->mLightPitch},
        {"iblIntensity", app->mIBLIntensity},
        {"shadowStrength", app->mShadowStrength},
        {"exposure", app->mExposure}
    };

    j["groundPlane"] = {
        {"worldY", app->mGroundWorldY},
        {"visible", app->mShowGroundPlane}
    };

    j["cone"] = {
        {"position", {app->mConePos.x, app->mConePos.y, app->mConePos.z}},
        {"scale", app->mConeScale},
        {"rot90", {app->mConeRot[0], app->mConeRot[1], app->mConeRot[2]}},
        {"visible", app->mShowCone}
    };

    j["cube"] = {{"visible", app->mShowCube}};

    j["trailMarkers"] = {{"visible", app->mShowTrailMarkers}};

    json glbArr = json::array();
    for (auto& obj : app->mGlbObjects) {
        json o;
        o["name"] = obj.name;
        o["filename"] = obj.glbFilename;
        o["position"] = {obj.position.x, obj.position.y, obj.position.z};
        o["scale"] = obj.scale;
        o["yRotation"] = obj.yRotation;
        o["rot90"] = {obj.rot90[0], obj.rot90[1], obj.rot90[2]};
        o["aabbCenterOffset"] = {obj.aabbCenterOffset.x, obj.aabbCenterOffset.y, obj.aabbCenterOffset.z};
        o["visible"] = obj.visible;
        glbArr.push_back(o);
    }
    j["glbObjects"] = glbArr;

    j["environment"] = {
        {"groundBrightness", app->mEnvGroundBrightness},
        {"skyBrightness", app->mEnvSkyBrightness},
        {"skyBlueness", app->mEnvSkyBlueness},
        {"horizonBrightness", app->mEnvHorizonBrightness},
        {"lightIntensity", app->mEnvLightIntensity},
        {"hdrFile", app->mUseHdrEnv ? app->mHdrEnvPath : ""}
    };

    j["tracking"] = {
        {"protocol", (int)app->mTrackingProtocol},
        {"transport", (int)app->mTrackingTransport},
        {"udpPort", app->mUdpPort}
    };

    j["background"] = {
        {"useGradient", app->mUseGradientBg}
    };

    if (app->mDistortionCalibLoaded) {
        j["distortion"] = {
            {"enabled", app->mDistortionEnabled},
            {"calibFile", app->mDistortionCalibFile}
        };
    }

    // If filename is already an absolute path, use it directly; otherwise resolve relative to exe dir
    bool isAbsolute = false;
#ifdef _WIN32
    isAbsolute = (filename.size() >= 2 && filename[1] == ':');
#endif
    std::string path = isAbsolute ? filename : scene_exeDirPath(filename);
    std::ofstream out(path);
    if (out) {
        out << j.dump(2);
        std::cout << "Scene saved to " << path
                  << " (cone visible=" << app->mShowCone
                  << ", " << app->mGlbObjects.size() << " GLB objects)" << std::endl;
    } else {
        std::cerr << "Failed to save scene to " << path << std::endl;
    }
}

void scene_load(FilamentApp* app, const std::string& filename) {
    using json = nlohmann::json;
    std::string path = scene_resolveFilePath(filename);
    if (path.empty()) {
        std::cerr << "Scene file not found: " << filename << std::endl;
        return;
    }
    std::ifstream in(path);
    if (!in) {
        std::cerr << "Failed to open scene file: " << path << std::endl;
        return;
    }

    json j;
    try {
        in >> j;
    } catch (const json::parse_error& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
        return;
    }

    if (j.contains("camera")) {
        auto& cam = j["camera"];
        if (cam.contains("distance")) app->mCameraDistance = cam["distance"];
        if (cam.contains("yaw")) app->mCameraYaw = cam["yaw"];
        if (cam.contains("pitch")) app->mCameraPitch = cam["pitch"];
        if (cam.contains("target")) {
            auto& t = cam["target"];
            app->mCameraTarget = float3(t[0].get<float>(), t[1].get<float>(), t[2].get<float>());
        }
        if (cam.contains("autoOrbit")) app->mAutoOrbit = cam["autoOrbit"];
    }

    if (j.contains("lighting")) {
        auto& lit = j["lighting"];
        if (lit.contains("sunIntensity")) {
            app->mSunIntensity = lit["sunIntensity"];
            auto& lm = app->mEngine->getLightManager();
            auto li = lm.getInstance(app->mSunLight);
            if (li) lm.setIntensity(li, app->mSunIntensity);
        }
        if (lit.contains("sunYaw")) app->mLightYaw = lit["sunYaw"];
        if (lit.contains("sunPitch")) app->mLightPitch = lit["sunPitch"];
        if (lit.contains("iblIntensity")) {
            app->mIBLIntensity = lit["iblIntensity"];
            if (app->mIndirectLight) app->mIndirectLight->setIntensity(app->mIBLIntensity);
        }
        if (lit.contains("shadowStrength")) {
            app->mShadowStrength = lit["shadowStrength"];
            if (app->mGroundMI) app->mGroundMI->setParameter("shadowStrength", app->mShadowStrength);
        }
        if (lit.contains("exposure")) {
            app->mExposure = lit["exposure"];
            float shutterSpeed = 256.0f / std::pow(2.0f, app->mExposure);
            app->mCamera->setExposure(16.0f, shutterSpeed, 100.0f);
        }
    }

    if (j.contains("groundPlane")) {
        auto& gp = j["groundPlane"];
        if (gp.contains("worldY")) {
            app->mGroundWorldY = gp["worldY"];
            auto& tcm = app->mEngine->getTransformManager();
            auto ti = tcm.getInstance(app->mGroundEntity);
            if (ti) tcm.setTransform(ti, mat4f::translation(float3{0.0f, app->mGroundWorldY, 0.0f}));
        }
        if (gp.contains("visible")) {
            app->mShowGroundPlane = gp["visible"];
            auto& rm = app->mEngine->getRenderableManager();
            auto ri = rm.getInstance(app->mGroundEntity);
            if (ri) rm.setLayerMask(ri, 0x1, app->mShowGroundPlane ? 0x1 : 0x0);
        }
    }

    if (j.contains("cone")) {
        auto& cone = j["cone"];
        if (cone.contains("position")) {
            auto& p = cone["position"];
            app->mConePos = float3(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
        }
        if (cone.contains("scale")) app->mConeScale = cone["scale"];
        if (cone.contains("rot90")) {
            auto& r = cone["rot90"];
            app->mConeRot[0] = r[0]; app->mConeRot[1] = r[1]; app->mConeRot[2] = r[2];
        }
        if (cone.contains("visible")) {
            app->mShowCone = cone["visible"];
            auto& rm = app->mEngine->getRenderableManager();
            auto ri = rm.getInstance(app->mConeEntity);
            if (ri) rm.setLayerMask(ri, 0x1, app->mShowCone ? 0x1 : 0x0);
        }
        app->updateConeTransform();
    }

    if (j.contains("cube")) {
        auto& cube = j["cube"];
        if (cube.contains("visible")) {
            app->mShowCube = cube["visible"];
            if (app->mShowCube) app->mScene->addEntity(app->mCubeEntity);
            else app->mScene->remove(app->mCubeEntity);
        }
    }

    if (j.contains("trailMarkers")) {
        auto& tm = j["trailMarkers"];
        if (tm.contains("visible")) {
            app->mShowTrailMarkers = tm["visible"];
            auto& rm = app->mEngine->getRenderableManager();
            for (auto& marker : app->mTrailMarkers) {
                auto ri = rm.getInstance(marker);
                if (ri) rm.setLayerMask(ri, 0x1, app->mShowTrailMarkers ? 0x1 : 0x0);
            }
        }
    }

    if (j.contains("glbObjects")) {
        scene_destroyAllGlbObjects(app);
        for (auto& item : j["glbObjects"]) {
            SceneObject obj;
            obj.name = item.value("name", "Unnamed");
            obj.glbFilename = item.value("filename", "");
            if (item.contains("position")) {
                auto& p = item["position"];
                obj.position = float3(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
            }
            obj.scale = item.value("scale", 1.0f);
            obj.yRotation = item.value("yRotation", 0.0f);
            if (item.contains("rot90")) {
                auto& r = item["rot90"];
                obj.rot90[0] = r[0]; obj.rot90[1] = r[1]; obj.rot90[2] = r[2];
            }
            if (item.contains("aabbCenterOffset")) {
                auto& a = item["aabbCenterOffset"];
                obj.aabbCenterOffset = float3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
            }
            obj.visible = item.value("visible", true);
            scene_loadGlbObject(app, obj);
            app->mGlbObjects.push_back(std::move(obj));
        }
    }

    if (j.contains("environment")) {
        auto& env = j["environment"];
        bool envChanged = false;
        if (env.contains("groundBrightness")) { app->mEnvGroundBrightness = env["groundBrightness"]; envChanged = true; }
        if (env.contains("skyBrightness")) { app->mEnvSkyBrightness = env["skyBrightness"]; envChanged = true; }
        if (env.contains("skyBlueness")) { app->mEnvSkyBlueness = env["skyBlueness"]; envChanged = true; }
        if (env.contains("horizonBrightness")) { app->mEnvHorizonBrightness = env["horizonBrightness"]; envChanged = true; }
        if (env.contains("lightIntensity")) { app->mEnvLightIntensity = env["lightIntensity"]; envChanged = true; }
        if (env.contains("hdrFile")) {
            std::string hdrFile = env["hdrFile"].get<std::string>();
            if (!hdrFile.empty()) {
                app->loadHdrEnvironment(hdrFile);
                envChanged = false;  // HDR overrides procedural
            } else {
                app->mUseHdrEnv = false;
                app->mHdrEnvPath.clear();
            }
        }
        if (envChanged) app->regenerateCubemap();
    }

    if (j.contains("tracking")) {
        auto& trk = j["tracking"];
        if (trk.contains("protocol")) app->mTrackingProtocol = (TrackingProtocol)trk["protocol"].get<int>();
        if (trk.contains("transport")) app->mTrackingTransport = (TrackingTransport)trk["transport"].get<int>();
        if (trk.contains("udpPort")) {
            app->mUdpPort = trk["udpPort"];
            snprintf(app->mUdpPortBuf, sizeof(app->mUdpPortBuf), "%d", app->mUdpPort);
        }
    }

    if (j.contains("background")) {
        auto& bg = j["background"];
        if (bg.contains("useGradient")) {
            bool wantGradient = bg["useGradient"];
            if (wantGradient) {
                if (app->mWebcamEnabled) {
                    app->shutdownWebcam();
                }
                app->setupGradientBackground();
            } else {
                app->mUseGradientBg = false;
            }
        }
    }

    if (j.contains("distortion")) {
        auto& dist = j["distortion"];
        if (dist.contains("calibFile")) {
            std::string calibFile = dist["calibFile"].get<std::string>();
            if (!calibFile.empty()) {
                // Teardown existing distortion if any
                if (app->mDistortionSetup) {
                    distortion_teardown(app);
                }
                distortion_loadCalibration(app, calibFile);
                bool enabled = dist.value("enabled", false);
                app->mDistortionEnabled = enabled;
                if (enabled && app->mDistortionCalibLoaded) {
                    distortion_setup(app);
                }
            }
        }
    }

    std::cout << "Scene loaded from " << path
              << " (cone visible=" << app->mShowCone
              << ", " << app->mGlbObjects.size() << " GLB objects)" << std::endl;
}
