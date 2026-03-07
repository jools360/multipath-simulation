#pragma once

#include <string>
#include <vector>

class FilamentApp;

void webcam_init(FilamentApp* app, int deviceIndex = 0);
void webcam_shutdown(FilamentApp* app);
bool webcam_updateFrame(FilamentApp* app);
void webcam_createBackgroundQuad(FilamentApp* app);
void webcam_setupGradientBackground(FilamentApp* app);
std::vector<std::string> webcam_enumerateDevices();
void webcam_setCameraExposure(FilamentApp* app, long value, bool autoExp);
