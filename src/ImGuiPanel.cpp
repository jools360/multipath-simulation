#include "ImGuiPanel.h"
#include "FilamentApp.h"
#include "SceneManager.h"
#include "LensDistortion.h"

#include <filament/RenderableManager.h>
#include <filament/LightManager.h>
#include <filament/TransformManager.h>
#include <filament/IndirectLight.h>

#include <imgui.h>

#ifdef _WIN32
#include <SDL_syswm.h>
#endif

#include <string>
#include <cstring>
#include <cmath>

void imgui_toggleEntityVisibility(FilamentApp* app, Entity entity, bool visible) {
    auto& rm = app->mEngine->getRenderableManager();
    auto ri = rm.getInstance(entity);
    if (!ri) return;
    rm.setLayerMask(ri, 0x1, visible ? 0x1 : 0x0);
}

void imgui_toggleGlbVisibility(FilamentApp* app, filament::gltfio::FilamentAsset* asset, bool visible) {
    if (!asset) return;
    auto& rm = app->mEngine->getRenderableManager();
    const Entity* entities = asset->getEntities();
    size_t count = asset->getEntityCount();
    for (size_t i = 0; i < count; i++) {
        auto ri = rm.getInstance(entities[i]);
        if (ri) {
            rm.setLayerMask(ri, 0x1, visible ? 0x1 : 0x0);
        }
    }
}

void imgui_buildPanel(FilamentApp* app) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Scene Controls", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // Dock / Float toggle
    if (app->mSidebarDocked) {
        if (ImGui::Button("Undock")) {
            app->mSidebarDocked = false;
            // Position floating window to the left of main window
            int mainX, mainY, mainW, mainH;
            SDL_GetWindowPosition(app->mWindow, &mainX, &mainY);
            SDL_GetWindowSize(app->mWindow, &mainW, &mainH);
            int ctrlX = mainX - app->mSidebarWidth - 8;
            if (ctrlX < 0) ctrlX = 0;
            SDL_SetWindowPosition(app->mControlWindow, ctrlX, mainY);
            SDL_SetWindowSize(app->mControlWindow, app->mSidebarWidth, mainH);
            app->updateSidebarLayout();
        }
    } else {
        if (ImGui::Button("Dock")) {
            app->mSidebarDocked = true;
            app->updateSidebarLayout();
        }
    }
    ImGui::SameLine();
    ImGui::Text("Tab: show/hide");
    ImGui::Separator();

    // Status
    ImGui::Text("FPS: %.0f", app->mCurrentFps);
    {
        std::lock_guard<std::mutex> lock(app->mTrackingMutex);
        if (app->mTrackingData.valid) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Tracking: Active");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Tracking: Inactive");
        }
    }

    // Tracking Input selection
    if (ImGui::CollapsingHeader("Tracking Input", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool isConnected = !app->mConnectedPortName.empty() || app->mUdpListening;

        // Transport radio buttons (disabled while connected)
        if (isConnected) ImGui::BeginDisabled();
        int transport = (int)app->mTrackingTransport;
        ImGui::Text("Transport:");
        ImGui::SameLine();
        if (ImGui::RadioButton("RS-232", &transport, 0)) app->mTrackingTransport = TrackingTransport::Serial;
        ImGui::SameLine();
        if (ImGui::RadioButton("UDP", &transport, 1)) app->mTrackingTransport = TrackingTransport::UDP;

        // Protocol radio buttons
        int protocol = (int)app->mTrackingProtocol;
        ImGui::Text("Protocol:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Auto-detect", &protocol, 0)) app->mTrackingProtocol = TrackingProtocol::AutoDetect;
        ImGui::SameLine();
        if (ImGui::RadioButton("VIPS", &protocol, 1)) app->mTrackingProtocol = TrackingProtocol::VIPS;
        ImGui::SameLine();
        if (ImGui::RadioButton("FreeD", &protocol, 2)) app->mTrackingProtocol = TrackingProtocol::FreeD;
        if (isConnected) ImGui::EndDisabled();

        ImGui::Separator();

        if (app->mTrackingTransport == TrackingTransport::Serial) {
            // RS-232 mode
            if (ImGui::Button("Refresh##comport")) {
                app->mComPorts = app->enumerateComPorts();
                app->mSelectedComPort = -1;
                if (!app->mConnectedPortName.empty()) {
                    for (int i = 0; i < (int)app->mComPorts.size(); i++) {
                        if (app->mComPorts[i] == app->mConnectedPortName) {
                            app->mSelectedComPort = i;
                            break;
                        }
                    }
                }
            }

            ImGui::SameLine();
            {
                const char* preview = (app->mSelectedComPort >= 0 && app->mSelectedComPort < (int)app->mComPorts.size())
                    ? app->mComPorts[app->mSelectedComPort].c_str() : "-- Select --";
                ImGui::SetNextItemWidth(120);
                if (ImGui::BeginCombo("##comport", preview)) {
                    for (int i = 0; i < (int)app->mComPorts.size(); i++) {
                        bool isSelected = (app->mSelectedComPort == i);
                        if (ImGui::Selectable(app->mComPorts[i].c_str(), isSelected)) {
                            app->mSelectedComPort = i;
                        }
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::SameLine();
            if (app->mConnectedPortName.empty()) {
                bool canConnect = (app->mSelectedComPort >= 0 && app->mSelectedComPort < (int)app->mComPorts.size());
                if (!canConnect) ImGui::BeginDisabled();
                if (ImGui::Button("Connect##comport")) {
                    const std::string& port = app->mComPorts[app->mSelectedComPort];
                    app->closeSerialPort();
                    app->mTrackingInitialized = false;
                    if (app->openSerialPort(port)) {
                        app->mConnectedPortName = port;
                    }
                }
                if (!canConnect) ImGui::EndDisabled();
            } else {
                if (ImGui::Button("Disconnect##comport")) {
                    app->closeSerialPort();
                    app->mConnectedPortName.clear();
                    {
                        std::lock_guard<std::mutex> lock2(app->mTrackingMutex);
                        app->mTrackingData.valid = false;
                    }
                    app->mTrackingInitialized = false;
                    app->mOrientationSmoothed = false;
                }
            }

            // Status line
            if (!app->mConnectedPortName.empty()) {
                const char* protoName = "detecting...";
                if (app->mDetectedProtocol == TrackingProtocol::VIPS) protoName = "VIPS";
                else if (app->mDetectedProtocol == TrackingProtocol::FreeD) protoName = "FreeD";
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Connected: %s (%s)",
                    app->mConnectedPortName.c_str(), protoName);
            } else {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Not connected");
            }
        } else {
            // UDP mode
            ImGui::Text("Port:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            if (app->mUdpListening) ImGui::BeginDisabled();
            ImGui::InputText("##udpport", app->mUdpPortBuf, sizeof(app->mUdpPortBuf),
                ImGuiInputTextFlags_CharsDecimal);
            if (app->mUdpListening) ImGui::EndDisabled();

            ImGui::SameLine();
            if (!app->mUdpListening) {
                if (ImGui::Button("Listen##udp")) {
                    int port = atoi(app->mUdpPortBuf);
                    if (port > 0 && port <= 65535) {
                        app->mTrackingInitialized = false;
                        app->openUdpPort(port);
                    }
                }
            } else {
                if (ImGui::Button("Stop##udp")) {
                    app->closeUdpPort();
                    {
                        std::lock_guard<std::mutex> lock2(app->mTrackingMutex);
                        app->mTrackingData.valid = false;
                    }
                    app->mTrackingInitialized = false;
                    app->mOrientationSmoothed = false;
                }
            }

            // Status line
            if (app->mUdpListening) {
                const char* protoName = "detecting...";
                if (app->mDetectedProtocol == TrackingProtocol::VIPS) protoName = "VIPS";
                else if (app->mDetectedProtocol == TrackingProtocol::FreeD) protoName = "FreeD";
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Listening on port %d (%s)",
                    app->mUdpPort, protoName);
            } else {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Not listening");
            }
        }
    }

    // Video Input selection
    if (ImGui::CollapsingHeader("Video Input", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Refresh##video")) {
            app->mVideoDevices = app->enumerateVideoDevices();
            app->mSelectedVideoDevice = -1;
            if (!app->mConnectedVideoDeviceName.empty()) {
                for (int i = 0; i < (int)app->mVideoDevices.size(); i++) {
                    if (app->mVideoDevices[i] == app->mConnectedVideoDeviceName) {
                        app->mSelectedVideoDevice = i;
                        break;
                    }
                }
            }
        }

        {
            const char* preview = (app->mSelectedVideoDevice >= 0 && app->mSelectedVideoDevice < (int)app->mVideoDevices.size())
                ? app->mVideoDevices[app->mSelectedVideoDevice].c_str() : "-- Select --";
            if (ImGui::BeginCombo("Device##video", preview)) {
                for (int i = 0; i < (int)app->mVideoDevices.size(); i++) {
                    bool isSelected = (app->mSelectedVideoDevice == i);
                    if (ImGui::Selectable(app->mVideoDevices[i].c_str(), isSelected)) {
                        app->mSelectedVideoDevice = i;
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        if (app->mConnectedVideoDeviceName.empty()) {
            bool canConnect = (app->mSelectedVideoDevice >= 0 && app->mSelectedVideoDevice < (int)app->mVideoDevices.size());
            if (!canConnect) ImGui::BeginDisabled();
            if (ImGui::Button("Connect##video")) {
                app->shutdownWebcam();
                app->mUseGradientBg = false;
                app->initWebcam(app->mSelectedVideoDevice);
            }
            if (!canConnect) ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Disconnect##video")) {
                app->shutdownWebcam();
            }
        }

        if (!app->mConnectedVideoDeviceName.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Connected: %s", app->mConnectedVideoDeviceName.c_str());
        } else {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Not connected");
        }

        // Camera exposure control
        if (app->mWebcamEnabled && app->mCamExposureAvailable) {
            ImGui::Separator();
            int exp = (int)app->mCamExposure;
            if (ImGui::SliderInt("Exposure##cam", &exp, (int)app->mCamExposureMin, (int)app->mCamExposureMax)) {
                app->setCameraExposure((long)exp, false);
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Auto##camexp", &app->mCamExposureAuto)) {
                app->setCameraExposure(app->mCamExposure, app->mCamExposureAuto);
            }
        }

        ImGui::Separator();
        bool gradientWas = app->mUseGradientBg;
        if (ImGui::Checkbox("Gradient background (no camera)", &app->mUseGradientBg)) {
            if (app->mUseGradientBg) {
                // Disconnect webcam if connected, then set up gradient
                if (app->mWebcamEnabled) {
                    app->shutdownWebcam();
                }
                app->setupGradientBackground();
            } else {
                // Turning off gradient — clear background view so it's just black/transparent
                app->mUseGradientBg = false;
            }
        }
    }

    // Lens Distortion
    if (ImGui::CollapsingHeader("Lens Distortion")) {
#ifdef _WIN32
        if (ImGui::Button("Load Calibration...")) {
            SDL_SysWMinfo wmInfo;
            SDL_VERSION(&wmInfo.version);
            SDL_GetWindowWMInfo(app->mControlWindow, &wmInfo);
            std::string path = openFileDialog(wmInfo.info.win.window,
                "Load Lens Calibration",
                "Lens Files\0*.ulens;*.json\0ULens Files\0*.ulens\0JSON Files\0*.json\0All Files\0*.*\0",
                "ulens");
            if (!path.empty()) {
                app->loadDistortionCalibration(path);
            }
        }
#endif
        if (app->mDistortionCalibLoaded) {
            // Enable/disable toggle
            if (ImGui::Checkbox("Enable Distortion", &app->mDistortionEnabled)) {
                if (app->mDistortionEnabled) {
                    if (!app->mDistortionSetup) {
                        app->generateDistortionUVMap();
                        app->setupDistortion();
                    }
                } else {
                    app->teardownDistortion();
                }
            }

            // Grid overlay toggle
            if (app->mDistortionSetup && app->mDistortionMI) {
                if (ImGui::Checkbox("Show Grid", &app->mDistortionShowGrid)) {
                    app->mDistortionMI->setParameter("showGrid", app->mDistortionShowGrid ? 1.0f : 0.0f);
                }
            }

            // Show calibration info
            if (!app->mDistortionCameraName.empty()) {
                ImGui::Text("Camera: %s", app->mDistortionCameraName.c_str());
            }
            // Show filename
            std::string calibName = app->mDistortionCalibFile;
            size_t slash = calibName.find_last_of("\\/");
            if (slash != std::string::npos) calibName = calibName.substr(slash + 1);
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "File: %s", calibName.c_str());

            ImGui::Text("Image: %dx%d", app->mDistortionImageW, app->mDistortionImageH);

            if (ImGui::TreeNode("Coefficients")) {
                ImGui::Text("K1: %.6f", app->mDistK1);
                ImGui::Text("K2: %.6f", app->mDistK2);
                ImGui::Text("K3: %.6f", app->mDistK3);
                ImGui::Text("P1: %.6f", app->mDistP1);
                ImGui::Text("P2: %.6f", app->mDistP2);
                ImGui::Text("Fx: %.6f  Fy: %.6f", app->mDistFx, app->mDistFy);
                ImGui::Text("Cx: %.6f  Cy: %.6f", app->mDistCx, app->mDistCy);
                ImGui::TreePop();
            }
        }
    }

    ImGui::Separator();

    // Visibility toggles
    if (ImGui::CollapsingHeader("Visibility", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Cone##vis", &app->mShowCone)) {
            imgui_toggleEntityVisibility(app, app->mConeEntity, app->mShowCone);
        }
        if (ImGui::Checkbox("Cube##vis", &app->mShowCube)) {
            if (app->mShowCube) {
                app->mScene->addEntity(app->mCubeEntity);
            } else {
                app->mScene->remove(app->mCubeEntity);
            }
        }
        for (size_t i = 0; i < app->mGlbObjects.size(); i++) {
            auto& obj = app->mGlbObjects[i];
            std::string label = obj.name + "##vis" + std::to_string(i);
            if (ImGui::Checkbox(label.c_str(), &obj.visible)) {
                imgui_toggleGlbVisibility(app, obj.asset, obj.visible);
            }
        }
        if (ImGui::Checkbox("Ground Plane##vis", &app->mShowGroundPlane)) {
            imgui_toggleEntityVisibility(app, app->mGroundEntity, app->mShowGroundPlane);
        }
        if (ImGui::Checkbox("Trail Markers##vis", &app->mShowTrailMarkers)) {
            for (auto& marker : app->mTrailMarkers) {
                imgui_toggleEntityVisibility(app, marker, app->mShowTrailMarkers);
            }
        }
    }

    // Cone controls
    if (ImGui::CollapsingHeader("Cone")) {
        bool coneChanged = false;
        coneChanged |= ImGui::DragFloat3("Position##cone", &app->mConePos.x, 0.1f);
        coneChanged |= ImGui::DragFloat("Scale##cone", &app->mConeScale, 0.05f, 0.1f, 10.0f);
        if (ImGui::Button("+90 X##cone")) { app->mConeRot[0]++; coneChanged = true; } ImGui::SameLine();
        if (ImGui::Button("+90 Y##cone")) { app->mConeRot[1]++; coneChanged = true; } ImGui::SameLine();
        if (ImGui::Button("+90 Z##cone")) { app->mConeRot[2]++; coneChanged = true; }
        if (coneChanged) app->updateConeTransform();
    }

    // GLB object controls (dynamic)
    int removeIdx = -1;
    for (size_t i = 0; i < app->mGlbObjects.size(); i++) {
        auto& obj = app->mGlbObjects[i];
        std::string header = obj.name + "##obj" + std::to_string(i);
        if (ImGui::CollapsingHeader(header.c_str())) {
            bool changed = false;
            std::string posLabel = "Position##obj" + std::to_string(i);
            changed |= ImGui::DragFloat3(posLabel.c_str(), &obj.position.x, 0.1f);
            float deg = obj.yRotation * 180.0f / 3.14159265358979f;
            std::string rotLabel = "Y Rotation##obj" + std::to_string(i);
            if (ImGui::DragFloat(rotLabel.c_str(), &deg, 1.0f, -360.0f, 360.0f, "%.1f deg")) {
                obj.yRotation = deg * 3.14159265358979f / 180.0f;
                changed = true;
            }
            std::string scaleLabel = "Scale##obj" + std::to_string(i);
            changed |= ImGui::DragFloat(scaleLabel.c_str(), &obj.scale, 0.05f, 0.01f, 100.0f);
            std::string aabbLabel = "AABB Offset##obj" + std::to_string(i);
            changed |= ImGui::DragFloat3(aabbLabel.c_str(), &obj.aabbCenterOffset.x, 0.01f);
            std::string bx = "+90 X##obj" + std::to_string(i);
            std::string by = "+90 Y##obj" + std::to_string(i);
            std::string bz = "+90 Z##obj" + std::to_string(i);
            if (ImGui::Button(bx.c_str())) { obj.rot90[0]++; changed = true; } ImGui::SameLine();
            if (ImGui::Button(by.c_str())) { obj.rot90[1]++; changed = true; } ImGui::SameLine();
            if (ImGui::Button(bz.c_str())) { obj.rot90[2]++; changed = true; }
            if (changed) app->updateObjectTransform(obj);
            std::string removeLabel = "Remove##obj" + std::to_string(i);
            if (ImGui::Button(removeLabel.c_str())) {
                removeIdx = (int)i;
            }
        }
    }
    if (removeIdx >= 0) {
        auto& obj = app->mGlbObjects[removeIdx];
        if (obj.asset) {
            app->mScene->removeEntities(obj.asset->getEntities(), obj.asset->getEntityCount());
            app->mAssetLoader->destroyAsset(obj.asset);
        }
        app->mGlbObjects.erase(app->mGlbObjects.begin() + removeIdx);
    }

    // Sun Light
    if (ImGui::CollapsingHeader("Sun Light")) {
        if (ImGui::DragFloat("Intensity##sun", &app->mSunIntensity, 1000.0f, 0.0f, 300000.0f, "%.0f lux")) {
            auto& lm = app->mEngine->getLightManager();
            auto li = lm.getInstance(app->mSunLight);
            if (li) lm.setIntensity(li, app->mSunIntensity);
        }
        float lightYawDeg = app->mLightYaw * 180.0f / 3.14159265358979f;
        float lightPitchDeg = app->mLightPitch * 180.0f / 3.14159265358979f;
        bool lightChanged = false;
        lightChanged |= ImGui::DragFloat("Yaw##sun", &lightYawDeg, 1.0f, -180.0f, 180.0f, "%.1f deg");
        lightChanged |= ImGui::DragFloat("Pitch##sun", &lightPitchDeg, 1.0f, -90.0f, 0.0f, "%.1f deg");
        if (lightChanged) {
            app->mLightYaw = lightYawDeg * 3.14159265358979f / 180.0f;
            app->mLightPitch = lightPitchDeg * 3.14159265358979f / 180.0f;
        }
    }

    // IBL
    if (ImGui::CollapsingHeader("IBL")) {
        if (ImGui::DragFloat("Intensity##ibl", &app->mIBLIntensity, 100.0f, 0.0f, 50000.0f, "%.0f")) {
            if (app->mIndirectLight) {
                app->mIndirectLight->setIntensity(app->mIBLIntensity);
            }
        }
    }

    // Exposure
    if (ImGui::CollapsingHeader("Exposure")) {
        if (ImGui::SliderFloat("EV##exposure", &app->mExposure, 5.0f, 20.0f, "%.1f")) {
            float shutterSpeed = 256.0f / std::pow(2.0f, app->mExposure);
            app->mCamera->setExposure(16.0f, shutterSpeed, 100.0f);
        }
    }

    // Screen-Space Reflections
    if (ImGui::CollapsingHeader("Screen-Space Reflections")) {
        bool ssrChanged = false;
        ssrChanged |= ImGui::Checkbox("Enabled##ssr", &app->mSSREnabled);
        if (!app->mSSREnabled) ImGui::BeginDisabled();
        ssrChanged |= ImGui::SliderFloat("Max Distance##ssr", &app->mSSRMaxDistance, 0.5f, 50.0f, "%.1f");
        ssrChanged |= ImGui::SliderFloat("Thickness##ssr", &app->mSSRThickness, 0.01f, 2.0f, "%.2f");
        ssrChanged |= ImGui::SliderFloat("Bias##ssr", &app->mSSRBias, 0.001f, 0.1f, "%.3f");
        ssrChanged |= ImGui::SliderFloat("Stride##ssr", &app->mSSRStride, 1.0f, 8.0f, "%.1f");
        if (!app->mSSREnabled) ImGui::EndDisabled();
        if (ssrChanged) {
            View::ScreenSpaceReflectionsOptions ssr;
            ssr.enabled = app->mSSREnabled;
            ssr.thickness = app->mSSRThickness;
            ssr.bias = app->mSSRBias;
            ssr.maxDistance = app->mSSRMaxDistance;
            ssr.stride = app->mSSRStride;
            app->mView->setScreenSpaceReflectionsOptions(ssr);
        }
    }

    // Environment Reflections
    if (ImGui::CollapsingHeader("Environment")) {
#ifdef _WIN32
        if (ImGui::Button("Load HDR...")) {
            SDL_SysWMinfo wmInfo;
            SDL_VERSION(&wmInfo.version);
            SDL_GetWindowWMInfo(app->mControlWindow, &wmInfo);
            std::string path = openFileDialog(wmInfo.info.win.window,
                "Load HDR Environment", "HDR Files\0*.hdr\0EXR Files\0*.exr\0All Files\0*.*\0", "hdr");
            if (!path.empty()) {
                app->loadHdrEnvironment(path);
            }
        }
#endif
        if (app->mUseHdrEnv) {
            ImGui::SameLine();
            if (ImGui::Button("Reset to Procedural")) {
                app->clearHdrEnvironment();
            }
            // Show loaded HDR filename
            std::string hdrName = app->mHdrEnvPath;
            size_t slash = hdrName.find_last_of("\\/");
            if (slash != std::string::npos) hdrName = hdrName.substr(slash + 1);
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "HDR: %s", hdrName.c_str());
        }

        // Procedural sliders (disabled when HDR is active)
        if (app->mUseHdrEnv) ImGui::BeginDisabled();
        bool envChanged = false;
        envChanged |= ImGui::SliderFloat("Ground##env", &app->mEnvGroundBrightness, 0.0f, 5.0f, "%.2f");
        envChanged |= ImGui::SliderFloat("Sky##env", &app->mEnvSkyBrightness, 0.0f, 5.0f, "%.2f");
        envChanged |= ImGui::SliderFloat("Sky Blue##env", &app->mEnvSkyBlueness, 0.0f, 3.0f, "%.2f");
        envChanged |= ImGui::SliderFloat("Horizon##env", &app->mEnvHorizonBrightness, 0.0f, 5.0f, "%.2f");
        envChanged |= ImGui::SliderFloat("Studio Lights##env", &app->mEnvLightIntensity, 0.0f, 5.0f, "%.2f");
        if (envChanged) {
            app->regenerateCubemap();
        }
        if (app->mUseHdrEnv) ImGui::EndDisabled();
    }

    // Ground Plane
    if (ImGui::CollapsingHeader("Ground Plane")) {
        if (ImGui::DragFloat("Y Position##ground", &app->mGroundWorldY, 0.1f, -10.0f, 10.0f)) {
            auto& tcm = app->mEngine->getTransformManager();
            auto ti = tcm.getInstance(app->mGroundEntity);
            if (ti) {
                tcm.setTransform(ti, mat4f::translation(float3{0.0f, app->mGroundWorldY, 0.0f}));
            }
        }
        if (ImGui::SliderFloat("Shadow Strength", &app->mShadowStrength, 0.0f, 1.0f)) {
            if (app->mGroundMI) {
                app->mGroundMI->setParameter("shadowStrength", app->mShadowStrength);
            }
        }
    }

    // Camera (only when tracking is inactive)
    {
        bool trackingActive = false;
        {
            std::lock_guard<std::mutex> lock(app->mTrackingMutex);
            trackingActive = app->mTrackingData.valid;
        }
        if (!trackingActive && ImGui::CollapsingHeader("Camera")) {
            ImGui::Checkbox("Auto Orbit", &app->mAutoOrbit);
            ImGui::DragFloat("Distance", &app->mCameraDistance, 0.1f, 2.0f, 20.0f);
            float camYawDeg = app->mCameraYaw * 180.0f / 3.14159265358979f;
            float camPitchDeg = app->mCameraPitch * 180.0f / 3.14159265358979f;
            if (ImGui::DragFloat("Yaw##cam", &camYawDeg, 1.0f)) {
                app->mCameraYaw = camYawDeg * 3.14159265358979f / 180.0f;
            }
            if (ImGui::DragFloat("Pitch##cam", &camPitchDeg, 1.0f, -80.0f, 80.0f)) {
                app->mCameraPitch = camPitchDeg * 3.14159265358979f / 180.0f;
            }
            ImGui::DragFloat3("Target", &app->mCameraTarget.x, 0.1f);
        }
    }

    // Scene File
    if (ImGui::CollapsingHeader("Scene File")) {
        ImGui::InputText("Filename##scene", app->mSceneFilename, sizeof(app->mSceneFilename));
        if (ImGui::Button("Reload Scene")) {
            app->loadScene(app->mSceneFilename);
        }
#ifdef _WIN32
        ImGui::SameLine();
        if (ImGui::Button("Save As...")) {
            SDL_SysWMinfo wmInfo;
            SDL_VERSION(&wmInfo.version);
            SDL_GetWindowWMInfo(app->mControlWindow, &wmInfo);
            std::string path = saveFileDialog(wmInfo.info.win.window,
                "Save Scene", "JSON Files\0*.json\0All Files\0*.*\0", "json");
            if (!path.empty()) {
                strncpy(app->mSceneFilename, path.c_str(), sizeof(app->mSceneFilename) - 1);
                app->saveScene(path);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Open...")) {
            SDL_SysWMinfo wmInfo;
            SDL_VERSION(&wmInfo.version);
            SDL_GetWindowWMInfo(app->mControlWindow, &wmInfo);
            std::string path = openFileDialog(wmInfo.info.win.window,
                "Open Scene", "JSON Files\0*.json\0All Files\0*.*\0", "json");
            if (!path.empty()) {
                strncpy(app->mSceneFilename, path.c_str(), sizeof(app->mSceneFilename) - 1);
                app->loadScene(path);
            }
        }
#endif
    }

    // Add GLB Object
    if (ImGui::CollapsingHeader("Add GLB Object")) {
        ImGui::InputText("GLB File##add", app->mNewGlbFilename, sizeof(app->mNewGlbFilename));
        ImGui::InputText("Name##add", app->mNewGlbName, sizeof(app->mNewGlbName));
#ifdef _WIN32
        if (ImGui::Button("Browse...##glb")) {
            SDL_SysWMinfo wmInfo;
            SDL_VERSION(&wmInfo.version);
            SDL_GetWindowWMInfo(app->mControlWindow, &wmInfo);
            std::string path = openFileDialog(wmInfo.info.win.window,
                "Select GLB Model", "GLB Files\0*.glb\0All Files\0*.*\0", "glb");
            if (!path.empty()) {
                strncpy(app->mNewGlbFilename, path.c_str(), sizeof(app->mNewGlbFilename) - 1);
                std::string name = path;
                size_t slash = name.find_last_of("\\/");
                if (slash != std::string::npos) name = name.substr(slash + 1);
                size_t dot = name.find_last_of('.');
                if (dot != std::string::npos) name = name.substr(0, dot);
                strncpy(app->mNewGlbName, name.c_str(), sizeof(app->mNewGlbName) - 1);
            }
        }
        ImGui::SameLine();
#endif
        if (ImGui::Button("Add Object") && strlen(app->mNewGlbFilename) > 0) {
            SceneObject obj;
            obj.name = strlen(app->mNewGlbName) > 0 ? app->mNewGlbName : app->mNewGlbFilename;
            obj.glbFilename = app->mNewGlbFilename;
            obj.visible = true;
            app->loadGlbObject(obj);
            if (obj.asset) {
                app->mGlbObjects.push_back(std::move(obj));
                app->mNewGlbFilename[0] = '\0';
                app->mNewGlbName[0] = '\0';
            }
        }
    }

    // Recording
    if (ImGui::CollapsingHeader("Recording")) {
        if (app->mRecording) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "RECORDING");
            ImGui::Text("Frames: %d", app->mRecordFrameCount);
            if (ImGui::Button("Stop Recording")) {
                app->stopRecording();
            }
        } else {
#ifdef _WIN32
            if (ImGui::Button("Output...")) {
                SDL_SysWMinfo wmInfo;
                SDL_VERSION(&wmInfo.version);
                SDL_GetWindowWMInfo(app->mControlWindow, &wmInfo);
                std::string path = saveFileDialog(wmInfo.info.win.window,
                    "Recording Output", "MP4 Video\0*.mp4\0All Files\0*.*\0", "mp4");
                if (!path.empty()) {
                    app->mRecordOutputPath = path;
                }
            }
            ImGui::SameLine();
#endif
            if (ImGui::Button("Start Recording")) {
                app->startRecording();
            }
            // Show current output path
            std::string outName = app->mRecordOutputPath;
            if (outName.empty()) outName = "(default: recording.mp4)";
            size_t slash = outName.find_last_of("\\/");
            if (slash != std::string::npos) outName = outName.substr(slash + 1);
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", outName.c_str());
        }
    }

    ImGui::End();
}
