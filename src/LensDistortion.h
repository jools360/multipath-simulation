#pragma once

#include <string>

class FilamentApp;

void distortion_loadCalibration(FilamentApp* app, const std::string& path);
void distortion_generateUVMap(FilamentApp* app);
void distortion_setup(FilamentApp* app);
void distortion_teardown(FilamentApp* app);
