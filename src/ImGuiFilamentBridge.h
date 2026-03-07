#pragma once

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Camera.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/ColorGrading.h>
#include <utils/EntityManager.h>

#include <imgui.h>
#include <vector>

class ImGuiFilamentBridge {
public:
    ImGuiFilamentBridge(filament::Engine* engine, filament::Material* material);
    ~ImGuiFilamentBridge();

    // Upload font atlas texture (call once after ImGui::GetIO().Fonts->Build())
    void createFontAtlasTexture();

    // Update the bridge with new draw data each frame
    void update(ImDrawData* drawData);

    // Get the Filament View for rendering
    filament::View* getView() const { return mView; }

    // Update viewport when window resizes
    void setViewport(uint32_t width, uint32_t height);

private:
    void destroyRenderables();

    filament::Engine* mEngine = nullptr;
    filament::Material* mMaterial = nullptr;
    filament::Scene* mScene = nullptr;
    filament::View* mView = nullptr;
    filament::Camera* mCamera = nullptr;
    utils::Entity mCameraEntity;

    filament::Texture* mFontTexture = nullptr;
    filament::ColorGrading* mColorGrading = nullptr;

    // Pooled resources for renderables
    struct RenderPrimitive {
        utils::Entity entity;
        filament::MaterialInstance* materialInstance = nullptr;
        filament::VertexBuffer* vertexBuffer = nullptr;
        filament::IndexBuffer* indexBuffer = nullptr;
        uint32_t vertexCapacity = 0;
        uint32_t indexCapacity = 0;
    };
    std::vector<RenderPrimitive> mRenderPrimitives;
    size_t mActivePrimitives = 0;

    uint32_t mViewportWidth = 1920;
    uint32_t mViewportHeight = 1080;
};
