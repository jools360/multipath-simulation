#pragma once

#include <string>

class FilamentApp;
struct SceneObject;

// Scene save/load
void scene_save(FilamentApp* app, const std::string& filename);
void scene_load(FilamentApp* app, const std::string& filename);

// GLB model management
void scene_loadGlbObject(FilamentApp* app, SceneObject& obj);
void scene_destroyAllGlbObjects(FilamentApp* app);
void scene_updateObjectTransform(FilamentApp* app, SceneObject& obj);

// File path utilities
std::string scene_resolveFilePath(const std::string& filename);
std::string scene_exeDirPath(const std::string& filename);

#ifdef _WIN32
#include <windows.h>
std::string openFileDialog(HWND owner, const char* title, const char* filter, const char* defaultExt);
std::string saveFileDialog(HWND owner, const char* title, const char* filter, const char* defaultExt);
#endif
