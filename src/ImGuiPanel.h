#pragma once

#include <utils/Entity.h>

namespace filament { namespace gltfio { class FilamentAsset; } }

class FilamentApp;

void imgui_buildPanel(FilamentApp* app);
void imgui_toggleEntityVisibility(FilamentApp* app, utils::Entity entity, bool visible);
void imgui_toggleGlbVisibility(FilamentApp* app, filament::gltfio::FilamentAsset* asset, bool visible);
