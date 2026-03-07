#include "Recording.h"
#include "FilamentApp.h"

#include <filament/Texture.h>
#include <filament/RenderTarget.h>
#include <filament/View.h>
#include <filament/Renderer.h>

#include <iostream>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

void recording_start(FilamentApp* app) {
    if (app->mRecording) return;

    // Launch FFmpeg with NVENC hardware encoding
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir(exePath);
    size_t lastSlash = exeDir.find_last_of("\\/");
    std::string ffmpegLocal = (lastSlash != std::string::npos)
        ? exeDir.substr(0, lastSlash + 1) + "ffmpeg.exe"
        : "ffmpeg.exe";
    // Use local ffmpeg.exe if it exists, otherwise fall back to PATH
    std::string ffmpegPath;
    if (GetFileAttributesA(ffmpegLocal.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ffmpegPath = ffmpegLocal;
    } else {
        ffmpegPath = "ffmpeg";
        std::cout << "ffmpeg.exe not found next to exe, using PATH" << std::endl;
    }

    SDL_GetWindowSize(app->mWindow, &app->mRecordWidth, &app->mRecordHeight);
    std::cout << "Recording resolution: " << app->mRecordWidth << "x" << app->mRecordHeight << std::endl;

    int recordFps = app->mWebcamFps > 0 ? app->mWebcamFps : 30;
    std::string sizeStr = std::to_string(app->mRecordWidth) + "x" + std::to_string(app->mRecordHeight);
    // Use user-configured output path, or default to exe directory
    if (app->mRecordOutputPath.empty()) {
        app->mRecordOutputPath = (lastSlash != std::string::npos)
            ? exeDir.substr(0, lastSlash + 1) + "recording.mp4"
            : "recording.mp4";
    }

    // On Windows, _popen uses cmd.exe /c which mishandles leading quotes.
    // Wrap the entire command in an extra set of quotes so cmd.exe parses correctly.
    std::string innerCmd =
        "\"" + ffmpegPath + "\" -y -hide_banner -loglevel error "
        "-f rawvideo -pix_fmt rgba -s " + sizeStr + " -r " + std::to_string(recordFps) + " "
        "-i pipe:0 "
        "-c:v hevc_nvenc -preset p7 -tune hq -rc vbr -cq 18 -b:v 0 "
        "-pix_fmt yuv420p \"" + app->mRecordOutputPath + "\"";
    std::string cmd = "\"" + innerCmd + "\"";
    std::cout << "Output: " << app->mRecordOutputPath << std::endl;
    std::cout << "Recording at " << recordFps << "fps (matching webcam)" << std::endl;

    app->mRecordPipe = _popen(cmd.c_str(), "w");
    if (!app->mRecordPipe) {
        std::cerr << "Failed to launch FFmpeg for recording" << std::endl;
        return;
    }
    _setmode(_fileno(app->mRecordPipe), _O_BINARY);

    // Create double-buffered offscreen RenderTargets
    for (int i = 0; i < 2; i++) {
        app->mRecordColor[i] = Texture::Builder()
            .width(app->mRecordWidth)
            .height(app->mRecordHeight)
            .levels(1)
            .format(Texture::InternalFormat::RGBA8)
            .usage(Texture::Usage::COLOR_ATTACHMENT | Texture::Usage::BLIT_SRC)
            .build(*app->mEngine);

        app->mRecordDepth[i] = Texture::Builder()
            .width(app->mRecordWidth)
            .height(app->mRecordHeight)
            .levels(1)
            .format(Texture::InternalFormat::DEPTH24)
            .usage(Texture::Usage::DEPTH_ATTACHMENT)
            .build(*app->mEngine);

        app->mRecordRT[i] = RenderTarget::Builder()
            .texture(RenderTarget::AttachmentPoint::COLOR, app->mRecordColor[i])
            .texture(RenderTarget::AttachmentPoint::DEPTH, app->mRecordDepth[i])
            .build(*app->mEngine);
    }

    // Create offscreen views
    app->mRecordBgView = app->mEngine->createView();
    app->mRecordBgView->setScene(app->mBackgroundScene);
    app->mRecordBgView->setCamera(app->mBackgroundCamera);
    app->mRecordBgView->setViewport({0, 0, (uint32_t)app->mRecordWidth, (uint32_t)app->mRecordHeight});
    app->mRecordBgView->setPostProcessingEnabled(false);
    app->mRecordBgView->setShadowingEnabled(false);

    app->mRecordMainView = app->mEngine->createView();
    app->mRecordMainView->setScene(app->mScene);
    app->mRecordMainView->setCamera(app->mCamera);
    app->mRecordMainView->setViewport({0, 0, (uint32_t)app->mRecordWidth, (uint32_t)app->mRecordHeight});
    app->mRecordMainView->setPostProcessingEnabled(true);
    app->mRecordMainView->setShadowingEnabled(true);
    app->mRecordMainView->setShadowType(View::ShadowType::PCF);
    app->mRecordMainView->setBlendMode(View::BlendMode::TRANSLUCENT);

    app->mRecordFrameIndex = 0;

    app->mRecordFrameCount = 0;
    app->mRecordStartTime = std::chrono::steady_clock::now();
    app->mRecordNextCapture = app->mRecordStartTime;
    app->mRecordThreadRunning = true;
    app->mRecordThread = std::thread([app]() {
        const size_t frameSize = (size_t)app->mRecordWidth * app->mRecordHeight * 4;
        while (true) {
            uint8_t* buf = nullptr;
            {
                std::unique_lock<std::mutex> lock(app->mRecordMutex);
                app->mRecordCV.wait(lock, [app]() {
                    return !app->mRecordQueue.empty() || !app->mRecordThreadRunning;
                });
                if (!app->mRecordThreadRunning && app->mRecordQueue.empty()) break;
                buf = app->mRecordQueue.front();
                app->mRecordQueue.pop();
            }
            fwrite(buf, 1, frameSize, app->mRecordPipe);
            app->mRecordFrameCount++;
            delete[] buf;
        }
    });
    app->mRecording = true;
    std::cout << "Recording started (NVENC HEVC via FFmpeg)" << std::endl;
}

void recording_stop(FilamentApp* app) {
    if (!app->mRecording) return;
    app->mRecording = false;
    app->mRecordThreadRunning = false;
    app->mRecordCV.notify_one();
    if (app->mRecordThread.joinable()) app->mRecordThread.join();
    if (app->mRecordPipe) {
        _pclose(app->mRecordPipe);
        app->mRecordPipe = nullptr;
    }
    auto elapsed = std::chrono::steady_clock::now() - app->mRecordStartTime;
    double secs = std::chrono::duration<double>(elapsed).count();
    double actualFps = app->mRecordFrameCount / secs;
    std::cout << "Recording stopped: " << app->mRecordFrameCount << " frames in "
              << secs << "s = " << actualFps << " fps" << std::endl;
    if (FILE* f = fopen("recording_stats.txt", "w")) {
        fprintf(f, "Frames: %d\nDuration: %.2f s\nActual FPS: %.2f\n", app->mRecordFrameCount, secs, actualFps);
        fclose(f);
    }

    // Destroy offscreen recording resources
    if (app->mRecordBgView) { app->mEngine->destroy(app->mRecordBgView); app->mRecordBgView = nullptr; }
    if (app->mRecordMainView) { app->mEngine->destroy(app->mRecordMainView); app->mRecordMainView = nullptr; }
    for (int i = 0; i < 2; i++) {
        if (app->mRecordRT[i]) { app->mEngine->destroy(app->mRecordRT[i]); app->mRecordRT[i] = nullptr; }
        if (app->mRecordColor[i]) { app->mEngine->destroy(app->mRecordColor[i]); app->mRecordColor[i] = nullptr; }
        if (app->mRecordDepth[i]) { app->mEngine->destroy(app->mRecordDepth[i]); app->mRecordDepth[i] = nullptr; }
    }
}
