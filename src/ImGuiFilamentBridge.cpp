#include "ImGuiFilamentBridge.h"

#include <filament/Viewport.h>
#include <filament/ColorGrading.h>
#include <math/mat4.h>
#include <math/vec3.h>

#include <cstring>
#include <algorithm>

using namespace filament;
using namespace filament::math;
using namespace utils;

// Filament requires TANGENTS attribute even for unlit materials.
// Pack ImDrawVert (pos, uv, col) + dummy tangent into this struct.
struct FilamentImGuiVertex {
    float position[2];
    float uv[2];
    uint32_t color;
    int16_t tangents[4];  // dummy {0,0,0,32767}
};

ImGuiFilamentBridge::ImGuiFilamentBridge(Engine* engine, Material* material)
    : mEngine(engine), mMaterial(material)
{
    mScene = mEngine->createScene();

    EntityManager& em = EntityManager::get();
    mCameraEntity = em.create();
    mCamera = mEngine->createCamera(mCameraEntity);

    mView = mEngine->createView();
    mView->setScene(mScene);
    mView->setCamera(mCamera);
    mView->setViewport({0, 0, mViewportWidth, mViewportHeight});
    // Post-processing must be ON for TRANSLUCENT blend mode to work in Filament
    mView->setPostProcessingEnabled(true);
    mView->setShadowingEnabled(false);
    mView->setBlendMode(View::BlendMode::TRANSLUCENT);

    // Use LINEAR tone mapping so ACES doesn't darken ImGui's dark panel colors
    mColorGrading = ColorGrading::Builder()
        .toneMapping(ColorGrading::ToneMapping::LINEAR)
        .build(*mEngine);
    mView->setColorGrading(mColorGrading);

    // Standard ortho projection (bottom=0, top=height).
    // We flip ImGui vertex Y coords instead of using a flipped camera,
    // because post-processing applies its own Y-flip that would cause double-flip.
    mCamera->setProjection(Camera::Projection::ORTHO,
        0.0, (double)mViewportWidth,
        0.0, (double)mViewportHeight,
        -1.0, 1.0);
    mCamera->lookAt({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
}

ImGuiFilamentBridge::~ImGuiFilamentBridge() {
    destroyRenderables();

    // Destroy pooled primitives
    for (auto& prim : mRenderPrimitives) {
        if (prim.materialInstance) mEngine->destroy(prim.materialInstance);
        if (prim.vertexBuffer) mEngine->destroy(prim.vertexBuffer);
        if (prim.indexBuffer) mEngine->destroy(prim.indexBuffer);
        if (prim.entity) {
            mEngine->getRenderableManager().destroy(prim.entity);
            EntityManager::get().destroy(prim.entity);
        }
    }
    mRenderPrimitives.clear();

    if (mColorGrading) mEngine->destroy(mColorGrading);
    if (mFontTexture) mEngine->destroy(mFontTexture);
    if (mView) mEngine->destroy(mView);
    if (mScene) mEngine->destroy(mScene);
    if (mCamera) {
        mEngine->destroyCameraComponent(mCameraEntity);
        EntityManager::get().destroy(mCameraEntity);
    }
}

void ImGuiFilamentBridge::createFontAtlasTexture() {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    mFontTexture = Texture::Builder()
        .width(width)
        .height(height)
        .levels(1)
        .format(Texture::InternalFormat::RGBA8)
        .sampler(Texture::Sampler::SAMPLER_2D)
        .build(*mEngine);

    size_t dataSize = (size_t)width * height * 4;
    uint8_t* uploadData = new uint8_t[dataSize];
    memcpy(uploadData, pixels, dataSize);

    Texture::PixelBufferDescriptor pbd(
        uploadData, dataSize,
        Texture::Format::RGBA, Texture::Type::UBYTE,
        [](void* buffer, size_t, void*) { delete[] static_cast<uint8_t*>(buffer); }
    );
    mFontTexture->setImage(*mEngine, 0, std::move(pbd));

    io.Fonts->SetTexID((ImTextureID)(uintptr_t)mFontTexture);
}

void ImGuiFilamentBridge::setViewport(uint32_t width, uint32_t height) {
    mViewportWidth = width;
    mViewportHeight = height;
    mView->setViewport({0, 0, width, height});
    mCamera->setProjection(Camera::Projection::ORTHO,
        0.0, (double)width,
        0.0, (double)height,
        -1.0, 1.0);
}

void ImGuiFilamentBridge::destroyRenderables() {
    for (size_t i = 0; i < mActivePrimitives; i++) {
        mScene->remove(mRenderPrimitives[i].entity);
    }
    mActivePrimitives = 0;
}

void ImGuiFilamentBridge::update(ImDrawData* drawData) {
    if (!drawData || drawData->TotalVtxCount == 0) {
        destroyRenderables();
        return;
    }

    // Remove previous frame's renderables from scene
    destroyRenderables();

    // Count total draw commands we need
    size_t totalDrawCmds = 0;
    for (int n = 0; n < drawData->CmdListsCount; n++) {
        totalDrawCmds += drawData->CmdLists[n]->CmdBuffer.Size;
    }

    // Ensure we have enough pooled primitives
    while (mRenderPrimitives.size() < totalDrawCmds) {
        RenderPrimitive prim;
        prim.entity = EntityManager::get().create();
        auto& tcm = mEngine->getTransformManager();
        tcm.create(prim.entity);
        prim.materialInstance = mMaterial->createInstance();
        mRenderPrimitives.push_back(prim);
    }

    size_t primIdx = 0;
    float fbWidth = drawData->DisplaySize.x * drawData->FramebufferScale.x;
    float fbHeight = drawData->DisplaySize.y * drawData->FramebufferScale.y;

    for (int n = 0; n < drawData->CmdListsCount; n++) {
        const ImDrawList* cmdList = drawData->CmdLists[n];
        int vtxCount = cmdList->VtxBuffer.Size;
        int idxCount = cmdList->IdxBuffer.Size;

        // Convert ImGui vertices to Filament format
        // Flip Y: ImGui uses top-left origin, our ortho camera uses standard bottom-left
        auto* filVerts = new FilamentImGuiVertex[vtxCount];
        for (int i = 0; i < vtxCount; i++) {
            const ImDrawVert& iv = cmdList->VtxBuffer[i];
            filVerts[i].position[0] = iv.pos.x;
            filVerts[i].position[1] = (float)mViewportHeight - iv.pos.y;
            filVerts[i].uv[0] = iv.uv.x;
            filVerts[i].uv[1] = iv.uv.y;
            filVerts[i].color = iv.col;
            filVerts[i].tangents[0] = 0;
            filVerts[i].tangents[1] = 0;
            filVerts[i].tangents[2] = 0;
            filVerts[i].tangents[3] = 32767;
        }

        // Copy indices (ImGui uses uint16 or uint32 depending on config, we use uint16)
        auto* filIndices = new uint16_t[idxCount];
        for (int i = 0; i < idxCount; i++) {
            filIndices[i] = (uint16_t)cmdList->IdxBuffer[i];
        }

        // Process each draw command
        for (int cmd_i = 0; cmd_i < cmdList->CmdBuffer.Size; cmd_i++) {
            const ImDrawCmd& pcmd = cmdList->CmdBuffer[cmd_i];

            if (pcmd.UserCallback) {
                pcmd.UserCallback(cmdList, &pcmd);
                continue;
            }

            if (primIdx >= mRenderPrimitives.size()) break;
            auto& prim = mRenderPrimitives[primIdx];

            // Recreate vertex buffer if needed (or if capacity is insufficient)
            if (!prim.vertexBuffer || prim.vertexCapacity < (uint32_t)vtxCount) {
                if (prim.vertexBuffer) mEngine->destroy(prim.vertexBuffer);
                uint32_t newCap = std::max((uint32_t)vtxCount, prim.vertexCapacity * 2);
                if (newCap < 256) newCap = 256;
                prim.vertexBuffer = VertexBuffer::Builder()
                    .vertexCount(newCap)
                    .bufferCount(1)
                    .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT2,
                               offsetof(FilamentImGuiVertex, position), sizeof(FilamentImGuiVertex))
                    .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2,
                               offsetof(FilamentImGuiVertex, uv), sizeof(FilamentImGuiVertex))
                    .attribute(VertexAttribute::COLOR, 0, VertexBuffer::AttributeType::UBYTE4,
                               offsetof(FilamentImGuiVertex, color), sizeof(FilamentImGuiVertex))
                    .normalized(VertexAttribute::COLOR)
                    .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::SHORT4,
                               offsetof(FilamentImGuiVertex, tangents), sizeof(FilamentImGuiVertex))
                    .normalized(VertexAttribute::TANGENTS)
                    .build(*mEngine);
                prim.vertexCapacity = newCap;
            }

            // Recreate index buffer if needed
            if (!prim.indexBuffer || prim.indexCapacity < (uint32_t)idxCount) {
                if (prim.indexBuffer) mEngine->destroy(prim.indexBuffer);
                uint32_t newCap = std::max((uint32_t)idxCount, prim.indexCapacity * 2);
                if (newCap < 256) newCap = 256;
                prim.indexBuffer = IndexBuffer::Builder()
                    .indexCount(newCap)
                    .bufferType(IndexBuffer::IndexType::USHORT)
                    .build(*mEngine);
                prim.indexCapacity = newCap;
            }

            // Upload vertex data (copy for async)
            {
                size_t vbSize = vtxCount * sizeof(FilamentImGuiVertex);
                auto* vbCopy = new uint8_t[vbSize];
                memcpy(vbCopy, filVerts, vbSize);
                prim.vertexBuffer->setBufferAt(*mEngine, 0,
                    VertexBuffer::BufferDescriptor(vbCopy, vbSize,
                        [](void* buf, size_t, void*) { delete[] static_cast<uint8_t*>(buf); }));
            }

            // Upload index data (copy for async)
            {
                size_t ibSize = idxCount * sizeof(uint16_t);
                auto* ibCopy = new uint8_t[ibSize];
                memcpy(ibCopy, filIndices, ibSize);
                prim.indexBuffer->setBuffer(*mEngine,
                    IndexBuffer::BufferDescriptor(ibCopy, ibSize,
                        [](void* buf, size_t, void*) { delete[] static_cast<uint8_t*>(buf); }));
            }

            // Set texture on material instance
            Texture* tex = mFontTexture;
            if (pcmd.TextureId) {
                tex = (Texture*)(uintptr_t)pcmd.TextureId;
            }
            if (tex) {
                TextureSampler sampler;
                sampler.setMagFilter(TextureSampler::MagFilter::LINEAR);
                sampler.setMinFilter(TextureSampler::MinFilter::LINEAR);
                prim.materialInstance->setParameter("albedo", tex, sampler);
            }

            // Calculate scissor rect — ImGui top-left origin, Filament bottom-left origin
            ImVec2 clipOff = drawData->DisplayPos;
            ImVec2 clipScale = drawData->FramebufferScale;
            float clipMinX = (pcmd.ClipRect.x - clipOff.x) * clipScale.x;
            float clipMinY = (pcmd.ClipRect.y - clipOff.y) * clipScale.y;
            float clipMaxX = (pcmd.ClipRect.z - clipOff.x) * clipScale.x;
            float clipMaxY = (pcmd.ClipRect.w - clipOff.y) * clipScale.y;

            if (clipMinX >= fbWidth || clipMinY >= fbHeight || clipMaxX < 0 || clipMaxY < 0)
                continue;

            // Clamp
            clipMinX = std::max(clipMinX, 0.0f);
            clipMinY = std::max(clipMinY, 0.0f);
            clipMaxX = std::min(clipMaxX, fbWidth);
            clipMaxY = std::min(clipMaxY, fbHeight);

            // Flip Y for Filament (bottom-left origin)
            uint32_t scissorLeft = (uint32_t)clipMinX;
            uint32_t scissorBottom = (uint32_t)(fbHeight - clipMaxY);
            uint32_t scissorWidth = (uint32_t)(clipMaxX - clipMinX);
            uint32_t scissorHeight = (uint32_t)(clipMaxY - clipMinY);

            // Build renderable
            mEngine->getRenderableManager().destroy(prim.entity);
            RenderableManager::Builder(1)
                .boundingBox({{-1e6, -1e6, -1e6}, {1e6, 1e6, 1e6}})
                .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                          prim.vertexBuffer, prim.indexBuffer,
                          (uint32_t)pcmd.IdxOffset, (uint32_t)pcmd.ElemCount)
                .material(0, prim.materialInstance)
                .culling(false)
                .receiveShadows(false)
                .castShadows(false)
                .screenSpaceContactShadows(false)
                .build(*mEngine, prim.entity);

            // Set scissor on the view for this draw — NOTE: Filament View::setScissor
            // is a view-wide scissor. Since we render one view, we can't per-draw-call
            // scissor easily. Instead we rely on ImGui's vertex clipping which is
            // generally sufficient for panels. For proper clipping, we'd need multiple views.
            // For now, skip scissor — ImGui clips most things via vertex data anyway.

            mScene->addEntity(prim.entity);
            primIdx++;
        }

        delete[] filVerts;
        delete[] filIndices;
    }

    mActivePrimitives = primIdx;
}
