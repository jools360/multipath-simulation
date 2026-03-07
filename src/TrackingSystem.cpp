#include "TrackingSystem.h"
#include "FilamentApp.h"

#include <filament/TransformManager.h>
#include <filament/Camera.h>

#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

// FreeD decode helpers
static float freed_decodeAngle(const uint8_t* p) {
    // 3-byte big-endian signed integer, shifted left 8 to sign-extend via int32_t
    int32_t raw = (int32_t(p[0]) << 24) | (int32_t(p[1]) << 16) | (int32_t(p[2]) << 8);
    return float(raw) / 32768.0f / 256.0f;
}

static float freed_decodePosition(const uint8_t* p) {
    // 3-byte big-endian signed integer, returns millimetres
    int32_t raw = (int32_t(p[0]) << 24) | (int32_t(p[1]) << 16) | (int32_t(p[2]) << 8);
    return float(raw) / 64.0f / 256.0f;
}

static uint8_t freed_checksum(const uint8_t* data) {
    int sum = 64;
    for (int i = 0; i < 28; i++) sum -= data[i];
    return (uint8_t)(sum & 0xFF);
}

bool tracking_openSerialPort(FilamentApp* app, const std::string& portName) {
#ifdef _WIN32
    std::string fullName = "\\\\.\\" + portName;

    app->mSerialPort = CreateFileA(
        fullName.c_str(),
        GENERIC_READ,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL
    );

    if (app->mSerialPort == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to open serial port " << portName << std::endl;
        return false;
    }

    app->mSerialEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!app->mSerialEvent) {
        std::cerr << "Failed to create serial event" << std::endl;
        CloseHandle(app->mSerialPort);
        app->mSerialPort = INVALID_HANDLE_VALUE;
        return false;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(app->mSerialPort, &dcb)) {
        std::cerr << "Failed to get serial port state" << std::endl;
        CloseHandle(app->mSerialEvent); app->mSerialEvent = NULL;
        CloseHandle(app->mSerialPort); app->mSerialPort = INVALID_HANDLE_VALUE;
        return false;
    }

    if (app->mTrackingProtocol == TrackingProtocol::FreeD) {
        dcb.BaudRate = 38400;
        dcb.ByteSize = 8;
        dcb.Parity = ODDPARITY;
        dcb.StopBits = ONESTOPBIT;
    } else {
        // VIPS or AutoDetect: start at VIPS settings
        dcb.BaudRate = 115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
    }

    if (!SetCommState(app->mSerialPort, &dcb)) {
        std::cerr << "Failed to set serial port state" << std::endl;
        CloseHandle(app->mSerialEvent); app->mSerialEvent = NULL;
        CloseHandle(app->mSerialPort); app->mSerialPort = INVALID_HANDLE_VALUE;
        return false;
    }

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    SetCommTimeouts(app->mSerialPort, &timeouts);

    SetCommMask(app->mSerialPort, EV_RXCHAR);

    app->mDetectedProtocol = TrackingProtocol::AutoDetect;
    app->mSerialRunning = true;
    app->mSerialThread = std::thread(&FilamentApp::serialReadThread, app);

    std::cout << "Serial port " << portName << " opened at "
              << (app->mTrackingProtocol == TrackingProtocol::FreeD ? "38400" : "115200")
              << " baud (overlapped I/O)" << std::endl;
    return true;
#else
    std::cerr << "Serial port not supported on this platform" << std::endl;
    return false;
#endif
}

void tracking_closeSerialPort(FilamentApp* app) {
    app->mSerialRunning = false;
#ifdef _WIN32
    if (app->mSerialEvent) SetEvent(app->mSerialEvent);
#endif
    if (app->mSerialThread.joinable()) {
        app->mSerialThread.join();
    }
#ifdef _WIN32
    if (app->mSerialPort != INVALID_HANDLE_VALUE) {
        CloseHandle(app->mSerialPort);
        app->mSerialPort = INVALID_HANDLE_VALUE;
    }
    if (app->mSerialEvent) {
        CloseHandle(app->mSerialEvent);
        app->mSerialEvent = NULL;
    }
#endif
}

std::vector<std::string> tracking_enumerateComPorts() {
    std::vector<std::string> ports;
#ifdef _WIN32
    char buf[65536];
    DWORD len = QueryDosDeviceA(NULL, buf, sizeof(buf));
    if (len > 0) {
        const char* p = buf;
        while (*p) {
            std::string name(p);
            if (name.compare(0, 3, "COM") == 0) {
                ports.push_back(name);
            }
            p += name.size() + 1;
        }
    }
    std::sort(ports.begin(), ports.end(), [](const std::string& a, const std::string& b) {
        int na = std::atoi(a.c_str() + 3);
        int nb = std::atoi(b.c_str() + 3);
        return na < nb;
    });
#endif
    return ports;
}

void tracking_serialReadThread(FilamentApp* app) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    enum class State { IDLE, VIPS_HEADER2, VIPS_LEN_LO, VIPS_LEN_HI, VIPS_BODY, FREED_BODY };
    State state = State::IDLE;
    std::vector<uint8_t> msgBuf;
    msgBuf.reserve(512);
    uint16_t msgLength = 0;

    const TrackingProtocol proto = app->mTrackingProtocol;
    const bool allowVIPS = (proto == TrackingProtocol::AutoDetect || proto == TrackingProtocol::VIPS);
    const bool allowFreeD = (proto == TrackingProtocol::AutoDetect || proto == TrackingProtocol::FreeD);

    // Auto-detect: track time since start; if no valid VIPS after 2s at 115200, switch to 38400/odd for FreeD
    auto startTime = std::chrono::steady_clock::now();
    bool autoSwitchedToFreeD = false;

    OVERLAPPED ov = {};
    ov.hEvent = app->mSerialEvent;

    while (app->mSerialRunning) {
        // Auto-detect: if no messages detected after 2 seconds on VIPS settings, switch to FreeD serial settings
        if (proto == TrackingProtocol::AutoDetect && !autoSwitchedToFreeD
            && app->mDetectedProtocol == TrackingProtocol::AutoDetect) {
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 2) {
                autoSwitchedToFreeD = true;
                std::cout << "Auto-detect: no VIPS data, switching to 38400/odd parity for FreeD" << std::endl;
                DCB dcb = {0};
                dcb.DCBlength = sizeof(DCB);
                if (GetCommState(app->mSerialPort, &dcb)) {
                    dcb.BaudRate = 38400;
                    dcb.Parity = ODDPARITY;
                    SetCommState(app->mSerialPort, &dcb);
                }
            }
        }

        uint8_t byte = 0;
        DWORD bytesRead = 0;

        ResetEvent(app->mSerialEvent);
        BOOL ok = ReadFile(app->mSerialPort, &byte, 1, &bytesRead, &ov);

        if (!ok) {
            if (GetLastError() == ERROR_IO_PENDING) {
                DWORD waitResult = WaitForSingleObject(app->mSerialEvent, 100);
                if (!app->mSerialRunning) break;
                if (waitResult != WAIT_OBJECT_0) continue;
                if (!GetOverlappedResult(app->mSerialPort, &ov, &bytesRead, FALSE)) continue;
            } else {
                if (!app->mSerialRunning) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
        }

        if (bytesRead == 0) continue;

        switch (state) {
        case State::IDLE:
            if (byte == VIPS_HEADER_1 && allowVIPS) {
                msgBuf.clear();
                msgBuf.push_back(byte);
                state = State::VIPS_HEADER2;
            } else if (byte == FREED_MSG_D1 && allowFreeD) {
                msgBuf.clear();
                msgBuf.push_back(byte);
                state = State::FREED_BODY;
            }
            break;

        case State::VIPS_HEADER2:
            if (byte == VIPS_HEADER_2) {
                msgBuf.push_back(byte);
                state = State::VIPS_LEN_LO;
            } else {
                state = State::IDLE;
            }
            break;

        case State::VIPS_LEN_LO:
            msgBuf.push_back(byte);
            msgLength = byte;
            state = State::VIPS_LEN_HI;
            break;

        case State::VIPS_LEN_HI:
            msgBuf.push_back(byte);
            msgLength |= ((uint16_t)byte << 8);
            if (msgLength < 32 || msgLength > 512) {
                state = State::IDLE;
            } else {
                state = State::VIPS_BODY;
            }
            break;

        case State::VIPS_BODY:
            msgBuf.push_back(byte);
            if (msgBuf.size() >= msgLength) {
                if (tracking_parseVIPSMessage(app, msgBuf.data(), msgBuf.size())) {
                    app->mDetectedProtocol = TrackingProtocol::VIPS;
                }
                state = State::IDLE;
            }
            break;

        case State::FREED_BODY:
            msgBuf.push_back(byte);
            if ((int)msgBuf.size() >= FREED_PACKET_LEN) {
                if (tracking_parseFreeDMessage(app, msgBuf.data(), msgBuf.size())) {
                    app->mDetectedProtocol = TrackingProtocol::FreeD;
                }
                state = State::IDLE;
            }
            break;
        }
    }

    CancelIo(app->mSerialPort);
#endif
}

bool tracking_parseVIPSMessage(FilamentApp* app, const uint8_t* data, size_t length) {
    if (length < 32) return false;

    if (data[0] != VIPS_HEADER_1 || data[1] != VIPS_HEADER_2) return false;

    uint32_t flags = *reinterpret_cast<const uint32_t*>(&data[4]);
    uint32_t timeMS = *reinterpret_cast<const uint32_t*>(&data[8]);
    double x = *reinterpret_cast<const double*>(&data[12]);
    double y = *reinterpret_cast<const double*>(&data[20]);
    float z = *reinterpret_cast<const float*>(&data[28]);

    CameraTrackingData newData;
    newData.x = x;
    newData.y = y;
    newData.z = z;
    newData.vipsTimeMS = timeMS;

    size_t offset = 32;

    // OUTPUT_STATUS (0x0002)
    if (flags & 0x0002) {
        if (offset + 4 <= length) {
            newData.beacons = data[offset];
            newData.solutionType = data[offset + 1];
        }
        offset += 4;
    }

    // OUTPUT_ORIENTATION (0x0004)
    if (flags & OUTPUT_ORIENTATION) {
        if (offset + 12 <= length) {
            newData.roll = *reinterpret_cast<const float*>(&data[offset]);
            newData.pitch = *reinterpret_cast<const float*>(&data[offset + 4]);
            newData.yaw = *reinterpret_cast<const float*>(&data[offset + 8]);
        }
        offset += 12;
    }

    newData.valid = true;

    auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    newData.localTimeUs = nowUs;

    {
        std::lock_guard<std::mutex> lock(app->mTrackingMutex);
        if (app->mTrackingData.valid) {
            app->mTrackingPrev = app->mTrackingData;
        }
        app->mTrackingData = newData;
        app->mTrackingLatestLocalTime = std::chrono::steady_clock::now();

        app->mTrackingBuffer.push_back(newData);
        int64_t cutoff = nowUs - 2000000;
        while (!app->mTrackingBuffer.empty() && app->mTrackingBuffer.front().localTimeUs < cutoff) {
            app->mTrackingBuffer.pop_front();
        }
    }

    app->mSerialMsgCount++;
    app->mTrackingSeq++;

    {
        std::lock_guard<std::mutex> lock(app->mTrailQueueMutex);
        app->mTrailQueue.push_back(newData);
    }

    // Log every serial message
    if (app->mCsvSerialFile) {
        auto utcNow = std::chrono::system_clock::now();
        int64_t serialUTC_us = std::chrono::duration_cast<std::chrono::microseconds>(
            utcNow.time_since_epoch()).count();
        int64_t lastVideoUTC_us = app->mLastVideoFrameUTC_us.load(std::memory_order_relaxed);
        fprintf(app->mCsvSerialFile, "%lld,%lld,%u,%.4f,%.4f,%.4f,%.6f,%.6f,%.4f\n",
                serialUTC_us, lastVideoUTC_us, newData.vipsTimeMS,
                newData.yaw, newData.pitch, newData.roll,
                newData.x, newData.y, newData.z);
    }

    return true;
}

bool tracking_parseFreeDMessage(FilamentApp* app, const uint8_t* data, size_t length) {
    if ((int)length < FREED_PACKET_LEN) return false;
    if (data[0] != FREED_MSG_D1) return false;

    // Validate checksum
    uint8_t expected = freed_checksum(data);
    if (data[28] != expected) return false;

    // Decode rotation (degrees)
    float pitch = freed_decodeAngle(&data[2]);   // tilt
    float yaw   = freed_decodeAngle(&data[5]);   // pan
    float roll  = freed_decodeAngle(&data[8]);

    // Decode position (millimetres) -> convert to metres
    float posZ = freed_decodePosition(&data[11]) / 1000.0f;
    float posX = freed_decodePosition(&data[14]) / 1000.0f;
    float posY = freed_decodePosition(&data[17]) / 1000.0f;

    CameraTrackingData newData;
    newData.x = posX;
    newData.y = posY;
    newData.z = posZ;
    newData.roll = roll;
    newData.pitch = pitch;
    newData.yaw = yaw;
    newData.valid = true;

    auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    newData.localTimeUs = nowUs;

    {
        std::lock_guard<std::mutex> lock(app->mTrackingMutex);
        if (app->mTrackingData.valid) {
            app->mTrackingPrev = app->mTrackingData;
        }
        app->mTrackingData = newData;
        app->mTrackingLatestLocalTime = std::chrono::steady_clock::now();

        app->mTrackingBuffer.push_back(newData);
        int64_t cutoff = nowUs - 2000000;
        while (!app->mTrackingBuffer.empty() && app->mTrackingBuffer.front().localTimeUs < cutoff) {
            app->mTrackingBuffer.pop_front();
        }
    }

    app->mSerialMsgCount++;
    app->mTrackingSeq++;

    {
        std::lock_guard<std::mutex> lock(app->mTrailQueueMutex);
        app->mTrailQueue.push_back(newData);
    }

    return true;
}

bool tracking_openUdpPort(FilamentApp* app, int port) {
#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return false;
    }

    app->mUdpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (app->mUdpSocket == INVALID_SOCKET) {
        std::cerr << "Failed to create UDP socket: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return false;
    }

    // Allow address reuse
    int reuse = 1;
    setsockopt(app->mUdpSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);

    if (bind(app->mUdpSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "Failed to bind UDP port " << port << ": " << WSAGetLastError() << std::endl;
        closesocket(app->mUdpSocket);
        app->mUdpSocket = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    app->mUdpPort = port;
    app->mDetectedProtocol = TrackingProtocol::AutoDetect;
    app->mUdpRunning = true;
    app->mUdpListening = true;
    app->mUdpThread = std::thread(&FilamentApp::udpReadThread, app);

    std::cout << "UDP listening on port " << port << std::endl;
    return true;
#else
    std::cerr << "UDP not supported on this platform" << std::endl;
    return false;
#endif
}

void tracking_closeUdpPort(FilamentApp* app) {
    app->mUdpRunning = false;
#ifdef _WIN32
    if (app->mUdpSocket != INVALID_SOCKET) {
        closesocket(app->mUdpSocket);
        app->mUdpSocket = INVALID_SOCKET;
    }
#endif
    if (app->mUdpThread.joinable()) {
        app->mUdpThread.join();
    }
    app->mUdpListening = false;
#ifdef _WIN32
    WSACleanup();
#endif
}

void tracking_udpReadThread(FilamentApp* app) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    const TrackingProtocol proto = app->mTrackingProtocol;

    uint8_t buf[512];
    while (app->mUdpRunning) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(app->mUdpSocket, &readSet);

        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout

        int sel = select(0, &readSet, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        if (!app->mUdpRunning) break;

        sockaddr_in from = {};
        int fromLen = sizeof(from);
        int received = recvfrom(app->mUdpSocket, (char*)buf, sizeof(buf), 0,
                                (sockaddr*)&from, &fromLen);
        if (received <= 0) continue;

        // Try to parse based on protocol setting
        if (received >= FREED_PACKET_LEN && buf[0] == FREED_MSG_D1
            && (proto == TrackingProtocol::AutoDetect || proto == TrackingProtocol::FreeD)) {
            if (tracking_parseFreeDMessage(app, buf, received)) {
                app->mDetectedProtocol = TrackingProtocol::FreeD;
            }
        } else if (received >= 4 && buf[0] == VIPS_HEADER_1 && buf[1] == VIPS_HEADER_2
            && (proto == TrackingProtocol::AutoDetect || proto == TrackingProtocol::VIPS)) {
            if (tracking_parseVIPSMessage(app, buf, received)) {
                app->mDetectedProtocol = TrackingProtocol::VIPS;
            }
        }
    }
#endif
}

void tracking_updateSceneFromTracking(FilamentApp* app, bool /*newWebcamFrame*/) {
    const auto& tracking = app->mWebcamTrackingDisplay;
    if (!tracking.valid) return;

    app->mTrackingDisplay = tracking;

    app->mDbgRawYaw = tracking.yaw;
    app->mDbgSmoothedYaw = tracking.yaw;
    app->mDbgRawPitch = tracking.pitch;
    app->mDbgSmoothedPitch = tracking.pitch;
    app->mDbgRawX = (float)tracking.x;
    app->mDbgRawY = (float)tracking.y;
    app->mDbgRawZ = tracking.z;
    app->mDbgSmoothedX = (float)tracking.x;
    app->mDbgSmoothedY = (float)tracking.y;
    app->mDbgSmoothedZ = tracking.z;

    const float SCENE_DISTANCE = 1.0f;
    const float PI = 3.14159265358979f;

    // Convert VIPS position to Filament world: X->X, Z->Y, -Y->Z
    float camFilX = (float)tracking.x;
    float camFilY = tracking.z;
    float camFilZ = -(float)tracking.y;

    // EMA smoothing for orientation
    float rawYaw = tracking.yaw;
    float rawPitch = tracking.pitch;
    float rawRoll = tracking.roll;
    if (!app->mOrientationSmoothed) {
        app->mSmoothedYaw = rawYaw;
        app->mSmoothedPitch = rawPitch;
        app->mSmoothedRoll = rawRoll;
        app->mOrientationSmoothed = true;
    } else {
        float yawDiff = rawYaw - app->mSmoothedYaw;
        if (yawDiff > 180.0f) yawDiff -= 360.0f;
        if (yawDiff < -180.0f) yawDiff += 360.0f;
        app->mSmoothedYaw += app->mOrientSmoothAlpha * yawDiff;
        if (app->mSmoothedYaw >= 360.0f) app->mSmoothedYaw -= 360.0f;
        if (app->mSmoothedYaw < 0.0f) app->mSmoothedYaw += 360.0f;

        app->mSmoothedPitch += app->mOrientSmoothAlpha * (rawPitch - app->mSmoothedPitch);
        app->mSmoothedRoll += app->mOrientSmoothAlpha * (rawRoll - app->mSmoothedRoll);
    }
    app->mDbgSmoothedYaw = app->mSmoothedYaw;
    app->mDbgSmoothedPitch = app->mSmoothedPitch;

    float yawFil = -app->mSmoothedYaw * PI / 180.0f;
    app->mDbgYawFil = yawFil;
    float pitchFil = app->mSmoothedPitch * PI / 180.0f;
    float rollFil = app->mSmoothedRoll * PI / 180.0f;

    mat4f R = mat4f::rotation(yawFil, float3{0, 1, 0}) *
              mat4f::rotation(pitchFil, float3{1, 0, 0}) *
              mat4f::rotation(rollFil, float3{0, 0, -1});

    mat4f T_WC = mat4f::translation(float3{camFilX, camFilY, camFilZ}) * R;

    // On first frame: anchor scene in front of initial camera
    if (!app->mTrackingInitialized) {
        app->mTrackingInitialized = true;

        float4 anchor_cam = {0.0f, 0.0f, -SCENE_DISTANCE, 1.0f};
        float4 anchor_world = T_WC * anchor_cam;

        std::cout << "Initial VIPS: X=" << tracking.x << " Y=" << tracking.y
                  << " Z=" << tracking.z << " Yaw=" << tracking.yaw << std::endl;
        std::cout << "Anchor (Filament): (" << anchor_world.x << ", "
                  << anchor_world.y << ", " << anchor_world.z << ")" << std::endl;

        app->mAnchorWorldPos = {anchor_world.x, anchor_world.y, anchor_world.z};

        auto& tcm = app->mEngine->getTransformManager();
        mat4f sceneRot = mat4f::rotation(PI * 0.5f, float3{0, 1, 0});
        mat4f anchorXform = mat4f::translation(float3{anchor_world.x, anchor_world.y, anchor_world.z}) * sceneRot;

        auto xformEntity = [&](Entity e) {
            auto ti = tcm.getInstance(e);
            if (ti) tcm.setTransform(ti, anchorXform * tcm.getTransform(ti));
        };

        xformEntity(app->mConeEntity);
        {
            auto cti = tcm.getInstance(app->mConeEntity);
            if (cti) {
                mat4f ct = tcm.getTransform(cti);
                app->mConePos = {ct[3][0], ct[3][1], ct[3][2]};
            }
        }
        xformEntity(app->mSunLight);
        for (auto& obj : app->mGlbObjects) {
            if (obj.asset) {
                xformEntity(obj.asset->getRoot());
                auto oti = tcm.getInstance(obj.asset->getRoot());
                if (oti) {
                    mat4f ot = tcm.getTransform(oti);
                    obj.position = {ot[3][0], ot[3][1], ot[3][2]};
                }
            }
        }
        if (app->mGroundEntity) {
            xformEntity(app->mGroundEntity);
            auto gti = tcm.getInstance(app->mGroundEntity);
            if (gti) {
                mat4f gt = tcm.getTransform(gti);
                gt[3][1] = app->mGroundWorldY;
                tcm.setTransform(gti, gt);
                std::cout << "Ground plane world Y=" << app->mGroundWorldY << std::endl;
            }
        }
    }

    // Each frame: set camera from current tracking pose
    float3 camPos = {T_WC[3][0] + app->mDebugOffset.x, T_WC[3][1], T_WC[3][2] + app->mDebugOffset.z};
    float3 camForward = {-T_WC[2][0], -T_WC[2][1], -T_WC[2][2]};
    float3 camUp = {T_WC[1][0], T_WC[1][1], T_WC[1][2]};

    if (app->mCamera)
        app->mCamera->lookAt(camPos, camPos + camForward * 5.0f, camUp);

    app->mLastCamPos = camPos;
    app->mLastCamForward = camForward;
    app->mTrailReady = true;
}
