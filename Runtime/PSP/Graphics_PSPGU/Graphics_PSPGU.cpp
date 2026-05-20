/**
 * @file Graphics_PSPGU.cpp
 * @brief Phase-2 PSPGU renderer — enough to draw textured static meshes.
 *
 *  Phase 1 just cleared to the engine's clear colour every frame. Phase 2
 *  fills in the minimum set of GFX_* hooks needed to render one or more
 *  StaticMesh3D nodes:
 *
 *    - sceGu init / shutdown / begin-frame / end-frame
 *    - texture upload (RGBA8 only for V1 — no swizzle, no quantize yet)
 *    - static-mesh resource build (engine Vertex → PSP layout, repacked)
 *    - per-draw state: view+proj matrices, world matrix, texture bind,
 *      pipeline depth/blend/cull config
 *
 *  Everything else (skeletal, particles, UI quads, post-process, light bake)
 *  stays a no-op stub and gets filled in in later phases.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Graphics/Graphics.h"
#include "Engine/Renderer.h"
#include "Engine/World.h"
#include "Engine/Assets/Material.h"
#include "Engine/Assets/MaterialLite.h"
#include "Engine/Assets/StaticMesh.h"
#include "Engine/Assets/Texture.h"
#include "Engine/Nodes/3D/StaticMesh3d.h"
#include "Engine/Nodes/3D/Camera3d.h"
#include "Log.h"

#include "Graphics_PSPGU/PSPGUTypes.h"
#include "Graphics_PSPGU/PSPGUUtils.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>

#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Module state
// -----------------------------------------------------------------------------

namespace
{
    constexpr int kScreenW = 480;
    constexpr int kScreenH = 272;
    constexpr int kBufW    = 512;       // PSP buffer pitch must be 16-aligned, ≥ width.

    // 256 KB command buffer in main RAM, 16-byte aligned. The PSP GE consumes
    // this each frame between sceGuStart and sceGuFinish.
    static unsigned int __attribute__((aligned(16))) sGuCmdList[262144 / 4];

    static bool sGuInitialised = false;

    // Default white texture used when a draw has no bound texture. Avoids the
    // PSP rendering grey/undefined garbage from whatever was last in TMU 0.
    static uint32_t __attribute__((aligned(16))) sWhiteTexel[4] = {
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    };

    // ---- helpers -----------------------------------------------------------

    inline uint32_t PspClearColor(const glm::vec4& cc)
    {
        const uint8_t r = (uint8_t)(cc.r * 255.0f);
        const uint8_t g = (uint8_t)(cc.g * 255.0f);
        const uint8_t b = (uint8_t)(cc.b * 255.0f);
        const uint8_t a = (uint8_t)(cc.a * 255.0f);
        return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    }

    // Upload the active camera's view + projection matrices into the GE.
    // CRITICAL: uses raw sceGuSetMatrix (NOT sceGum*) — see memory
    // project_psp_pspgum_breaks_state. sceGum* calls retroactively corrupt
    // already-issued 3D draws in this PSPSDK build.
    static const ScePspFMatrix4 sIdentityMtx = {
        { 1.0f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f, 1.0f },
    };

    void UploadCameraMatrices()
    {
        Renderer* renderer = Renderer::Get();
        if (renderer == nullptr) return;
        World* world = renderer->GetCurrentWorld();
        if (world == nullptr) return;
        Camera3D* cam = world->GetActiveCamera();
        if (cam == nullptr) return;

        const glm::mat4 view = cam->GetViewMatrix();
        const glm::mat4 proj = cam->GetProjectionMatrix();

        // glm::mat4 (column-major) and ScePspFMatrix4 have identical 16-float
        // layout. Direct copy works.
        ScePspFMatrix4 projPsp, viewPsp;
        memcpy(&projPsp, &proj[0][0], sizeof(float) * 16);
        memcpy(&viewPsp, &view[0][0], sizeof(float) * 16);

        sceGuSetMatrix(GU_PROJECTION, &projPsp);
        sceGuSetMatrix(GU_VIEW,       &viewPsp);
        sceGuSetMatrix(GU_MODEL,      &sIdentityMtx);
    }

    // Bind a Texture asset's resource to TMU 0. nullptr ⇒ fall back to the
    // built-in white 1×1 so untextured materials still render.
    void BindTexture(Texture* tex)
    {
        if (tex == nullptr)
        {
            sceGuTexImage(0, 2, 2, 2, sWhiteTexel);
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            sceGuTexWrap(GU_REPEAT, GU_REPEAT);
            sceGuEnable(GU_TEXTURE_2D);
            return;
        }

        TextureResource* r = tex->GetResource();
        if (r == nullptr || r->mPixels == nullptr || r->mWidth == 0)
        {
            BindTexture(nullptr);
            return;
        }

        // Ensure the texture pixels are flushed from CPU cache. The GE reads
        // texture data via DMA from RAM, bypassing CPU cache — without this,
        // a freshly-uploaded texture can come back as zeros / garbage.
        const uint32_t texBytes = (uint32_t)r->mBufWidth * r->mHeight * 4u;
        sceKernelDcacheWritebackRange(r->mPixels, texBytes);

        sceGuTexMode(r->mPsm, /*maxMips=*/0, /*a2=*/0, /*swizzle=*/r->mSwizzled);
        sceGuTexImage(0, (int)r->mWidth, (int)r->mHeight, (int)r->mBufWidth, r->mPixels);
        // GU_TCC_RGB: use texture RGB, ignore texture alpha (avoids hiding
        // pixels where the texture has alpha=0 borders).
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_REPEAT, GU_REPEAT);
        // PSPSDK cube sample sets these explicitly; default may have been
        // left in some funky state by other engine calls.
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        sceGuTexEnvColor(0xFFFFFFFFu);
        // GU_TEXTURE = 4th matrix slot (texture coord transform). Force to
        // identity so UVs pass through unchanged. Without this, stale data
        // in this slot can warp the right face's UV sampling off the image.
        sceGuSetMatrix(GU_TEXTURE, &sIdentityMtx);
        sceGuEnable(GU_TEXTURE_2D);
    }
}

// =============================================================================
// Lifecycle + frame
// =============================================================================

void GFX_Initialize()
{
    if (sGuInitialised) return;

    // VRAM layout: front buffer | back buffer | depth buffer.
    void* fbp0 = (void*)0;
    void* fbp1 = (void*)(kBufW * kScreenH * 4);
    void* zbp  = (void*)(kBufW * kScreenH * 4 * 2);

    sceGuInit();
    sceGuStart(GU_DIRECT, sGuCmdList);

    sceGuDrawBuffer(GU_PSM_8888, fbp0, kBufW);
    sceGuDispBuffer(kScreenW, kScreenH, fbp1, kBufW);
    sceGuDepthBuffer(zbp, kBufW);

    sceGuOffset(2048 - (kScreenW / 2), 2048 - (kScreenH / 2));
    sceGuViewport(2048, 2048, kScreenW, kScreenH);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, kScreenW, kScreenH);
    sceGuEnable(GU_SCISSOR_TEST);

    // PSP depth is inverted (16-bit, far = 0). With sceGuDepthRange(65535,0)
    // we map near→65535 / far→0, so GEQUAL keeps fragments closer to the
    // camera.
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_FALSE);       // mask 0 = write enabled (PSP inversion)

    // Cull OFF until the engine's mesh winding convention is verified against
    // PSP's GU_CW front-face expectation. CLIP_PLANES must be ON — disabling
    // it caused the rasterizer to silently drop all 3D primitives in testing.
    sceGuFrontFace(GU_CCW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_CLIP_PLANES);

    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_BLEND);

    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    sGuInitialised = true;
    LogDebug("Graphics_PSPGU: init OK (480x272 RGBA8 + depth16)");
}

void GFX_Shutdown()
{
    if (!sGuInitialised) return;
    sceGuDisplay(GU_FALSE);
    sceGuTerm();
    sGuInitialised = false;
}

void GFX_BeginFrame()
{
    if (!sGuInitialised) return;
    sceGuStart(GU_DIRECT, sGuCmdList);

    const glm::vec4 cc = Renderer::Get() ? Renderer::Get()->GetClearColor() : glm::vec4(0, 0, 0, 1);
    sceGuClearColor(PspClearColor(cc));
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

    // Engine draws follow this call. BeginRenderPass(Forward) sets the
    // camera matrices; per-draw code sets the model matrix and binds
    // texture. Depth + cull state restored to engine-friendly defaults.
    sceGuEnable(GU_DEPTH_TEST);
}

void GFX_EndFrame()
{
    if (!sGuInitialised) return;
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

void GFX_BeginScreen(uint32_t /*screenIndex*/) {}
void GFX_BeginView(uint32_t /*viewIndex*/) {}

bool GFX_ShouldCullLights() { return true; }

void GFX_BeginRenderPass(RenderPassId pass)
{
    if (pass == RenderPassId::Forward)
    {
        UploadCameraMatrices();
    }
}

void GFX_EndRenderPass() {}

void GFX_SetPipelineState(PipelineConfig config)
{
    // Phase 2 maps the configs we actually hit (Forward + Translucent) to
    // their GU state. Other configs map to "forward" until later phases need
    // them — gives a safe default.
    switch (config)
    {
        case PipelineConfig::Translucent:
        case PipelineConfig::Additive:
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_ADD,
                           (config == PipelineConfig::Additive) ? GU_FIX : GU_SRC_ALPHA,
                           (config == PipelineConfig::Additive) ? GU_FIX : GU_ONE_MINUS_SRC_ALPHA,
                           0, 0);
            sceGuDepthMask(GU_TRUE);    // don't write depth for translucents
            break;

        case PipelineConfig::Forward:
        case PipelineConfig::Opaque:
        default:
            sceGuDisable(GU_BLEND);
            sceGuDepthMask(GU_FALSE);
            break;
    }
}

// Engine `Renderer` passes its scene-tab viewport dims (which may be 480 OR
// the editor's scene-window dims like 312x200). PSP can only render to the
// physical 480x272 framebuffer, so clamp.
void GFX_SetViewport(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/, bool)
{
    sceGuOffset(2048 - (480 / 2), 2048 - (272 / 2));
    sceGuViewport(2048, 2048, 480, 272);
}
void GFX_SetScissor(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/, bool)
{
    sceGuScissor(0, 0, 480, 272);
}

glm::mat4 GFX_MakePerspectiveMatrix(float fovyDegrees, float aspectRatio, float zNear, float zFar)
{
    return glm::perspective(glm::radians(fovyDegrees), aspectRatio, zNear, zFar);
}
glm::mat4 GFX_MakeOrthographicMatrix(float left, float right, float bottom, float top, float zNear, float zFar)
{
    return glm::ortho(left, right, bottom, top, zNear, zFar);
}

void GFX_SetFog(const FogSettings& /*fogSettings*/) {}
void GFX_DrawLines(const std::vector<Line>& /*lines*/) {}
void GFX_DrawFullscreen() {}
void GFX_ResizeWindow() {}
void GFX_Reset() {}
Node3D* GFX_ProcessHitCheck(World* /*world*/, int32_t /*x*/, int32_t /*y*/, uint32_t* /*outInstance*/) { return nullptr; }
uint32_t GFX_GetNumViews() { return 1; }
void GFX_SetFrameRate(int32_t /*frameRate*/) {}
void GFX_PathTrace() {}
void GFX_BeginLightBake() {}
void GFX_UpdateLightBake() {}
void GFX_EndLightBake() {}
bool GFX_IsLightBakeInProgress() { return false; }
float GFX_GetLightBakeProgress() { return 0.0f; }
void GFX_EnableMaterials(bool /*enable*/) {}
void GFX_BeginGpuTimestamp(const char* /*name*/) {}
void GFX_EndGpuTimestamp(const char* /*name*/) {}

// =============================================================================
// Texture resources
// =============================================================================
//
// Phase 2: RGBA8888 linear only. No swizzle, no mip levels, no RGB565
// quantization. Buffer-width padded to next 16-pixel boundary (PSP texture
// pitch requirement).
//
// memalign on PSP is in <malloc.h> (newlib). Allocations stay in main RAM;
// VRAM packing happens in Phase 6.

void GFX_CreateTextureResource(Texture* texture, std::vector<uint8_t>& /*data*/)
{
    if (texture == nullptr) return;
    TextureResource* r = texture->GetResource();
    if (r == nullptr) return;

    const uint32_t srcW = texture->GetWidth();
    const uint32_t srcH = texture->GetHeight();
    if (srcW == 0 || srcH == 0) return;

    const std::vector<uint8_t>& pixels = texture->GetPixels();
    if (pixels.empty()) return;

    // PSP texture pitch must be a multiple of 16 pixels (= 64 bytes at RGBA8).
    const uint32_t bufW = (srcW + 15u) & ~15u;
    const uint32_t bytes = bufW * srcH * 4u;

    void* dst = memalign(16, bytes);
    if (dst == nullptr) return;

    if (bufW == srcW)
    {
        memcpy(dst, pixels.data(), bytes);
    }
    else
    {
        // Row-by-row copy with right-edge padding (replicate last column —
        // anything is fine since UVs ≤ 1 won't sample padding).
        const uint8_t* src = pixels.data();
        uint8_t* d = (uint8_t*)dst;
        for (uint32_t y = 0; y < srcH; ++y)
        {
            memcpy(d + y * bufW * 4, src + y * srcW * 4, srcW * 4);
            // pad
            for (uint32_t x = srcW; x < bufW; ++x)
            {
                memcpy(d + y * bufW * 4 + x * 4, d + y * bufW * 4 + (srcW - 1) * 4, 4);
            }
        }
    }

    r->mPixels   = dst;
    r->mWidth    = srcW;
    r->mHeight   = srcH;
    r->mBufWidth = bufW;
    r->mPsm      = GU_PSM_8888;
    r->mMipCount = 0;
    r->mSwizzled = 0;

    sceKernelDcacheWritebackRange(dst, bytes);
}

void GFX_DestroyTextureResource(Texture* texture)
{
    if (texture == nullptr) return;
    TextureResource* r = texture->GetResource();
    if (r == nullptr) return;
    if (r->mPixels != nullptr)
    {
        free(r->mPixels);
        r->mPixels = nullptr;
    }
    r->mWidth = r->mHeight = r->mBufWidth = 0;
}

void GFX_UpdateTextureResourcePixels(Texture* /*texture*/, const uint8_t* /*src*/, uint32_t /*srcWidth*/, uint32_t /*srcHeight*/)
{
    // Streaming textures (video player) — not wired in Phase 2.
}

// =============================================================================
// Material resources
// =============================================================================
//
// Fixed-function PSP has no shader compilation step; material state is applied
// per-draw via sceGu* calls. No persistent resource needed.

void GFX_CreateMaterialResource(Material* /*material*/) {}
void GFX_DestroyMaterialResource(Material* /*material*/) {}

// =============================================================================
// Static-mesh resources
// =============================================================================

void GFX_CreateStaticMeshResource(StaticMesh* staticMesh, bool hasColor, uint32_t numVertices, void* vertices, uint32_t numIndices, IndexType* indices)
{
    if (staticMesh == nullptr || vertices == nullptr || indices == nullptr) return;
    if (numVertices == 0 || numIndices == 0) return;

    StaticMeshResource* r = staticMesh->GetResource();
    if (r == nullptr) return;

    const uint32_t stride = hasColor ? sizeof(psp::StaticColorVertex) : sizeof(psp::StaticVertex);
    const uint32_t vBytes = stride * numVertices;
    const uint32_t iBytes = sizeof(IndexType) * numIndices;

    void* vBuf = memalign(16, vBytes);
    void* iBuf = memalign(16, iBytes);
    if (vBuf == nullptr || iBuf == nullptr)
    {
        if (vBuf) free(vBuf);
        if (iBuf) free(iBuf);
        return;
    }

    if (hasColor)
    {
        psp::RepackVerticesColor(static_cast<const VertexColor*>(vertices),
                                  numVertices,
                                  static_cast<psp::StaticColorVertex*>(vBuf));
    }
    else
    {
        psp::RepackVertices(static_cast<const Vertex*>(vertices),
                             numVertices,
                             static_cast<psp::StaticVertex*>(vBuf));
    }

    memcpy(iBuf, indices, iBytes);

    r->mVertexData   = vBuf;
    r->mIndexData    = iBuf;
    r->mNumVertices  = numVertices;
    r->mNumIndices   = numIndices;
    r->mVertexStride = stride;
    r->mVertexFlags  = hasColor ? psp::kStaticColorVertexFlags : psp::kStaticVertexFlags;

    sceKernelDcacheWritebackRange(vBuf, vBytes);
    sceKernelDcacheWritebackRange(iBuf, iBytes);
}

void GFX_DestroyStaticMeshResource(StaticMesh* staticMesh)
{
    if (staticMesh == nullptr) return;
    StaticMeshResource* r = staticMesh->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { free(r->mVertexData); r->mVertexData = nullptr; }
    if (r->mIndexData  != nullptr) { free(r->mIndexData);  r->mIndexData  = nullptr; }
    r->mNumVertices = r->mNumIndices = r->mVertexStride = r->mVertexFlags = 0;
}

// =============================================================================
// Skeletal mesh — Phase 3
// =============================================================================

void GFX_CreateSkeletalMeshResource(SkeletalMesh* /*sm*/, uint32_t /*numVerts*/, VertexSkinned* /*verts*/, uint32_t /*numIdx*/, IndexType* /*idx*/) {}
void GFX_DestroySkeletalMeshResource(SkeletalMesh* /*sm*/) {}

// =============================================================================
// Comp resources
// =============================================================================

void GFX_CreateStaticMeshCompResource(StaticMesh3D* /*c*/) {}
void GFX_DestroyStaticMeshCompResource(StaticMesh3D* c)
{
    if (c == nullptr) return;
    StaticMeshCompResource* r = c->GetResource();
    if (r != nullptr && r->mColorVertexData != nullptr)
    {
        free(r->mColorVertexData);
        r->mColorVertexData = nullptr;
    }
}
void GFX_UpdateStaticMeshCompResourceColors(StaticMesh3D* /*c*/)
{
    // Instance colours — Phase 3 feature. Keep stub so engine code's
    // "always-refresh-on-change" path is harmless.
}

// =============================================================================
// Static-mesh draw — the hot path
// =============================================================================

void GFX_DrawStaticMeshComp(StaticMesh3D* comp, StaticMesh* meshOverride)
{
    if (!sGuInitialised || comp == nullptr) return;

    StaticMesh* mesh = meshOverride ? meshOverride : comp->GetStaticMesh();
    if (mesh == nullptr) return;

    StaticMeshResource* r = mesh->GetResource();
    if (r == nullptr || r->mVertexData == nullptr || r->mIndexData == nullptr) return;
    if (r->mNumIndices == 0) return;

    // Per-draw state. Cull stays disabled until engine mesh winding is
    // verified against PSP's CW convention.
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_LIGHTING);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuColor(0xFFFFFFFFu);

    // Bind material's slot-0 texture (white 2×2 fallback if material/tex null).
    Material* matBase = comp->GetMaterial();
    MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
    Texture* tex = mat ? mat->GetTexture(0) : nullptr;
    BindTexture(tex);

    // Use engine matrices (already uploaded by BeginRenderPass(Forward)) plus
    // the cube's world matrix. Engine vertex data fed straight to the GE.
    ScePspFMatrix4 worldMtx;
    const glm::mat4& world = comp->GetRenderTransform();
    memcpy(&worldMtx, &world[0][0], sizeof(float) * 16);
    sceGuSetMatrix(GU_MODEL, &worldMtx);

    sceGuDrawArray(GU_TRIANGLES,
                   r->mVertexFlags,
                   (int)r->mNumIndices,
                   r->mIndexData,
                   r->mVertexData);
}

// =============================================================================
// The remaining GFX_* surface — stubs until later phases
// =============================================================================

void GFX_CreateSkeletalMeshCompResource(SkeletalMesh3D* /*c*/) {}
void GFX_DestroySkeletalMeshCompResource(SkeletalMesh3D* /*c*/) {}
void GFX_ReallocateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* /*c*/, uint32_t /*n*/) {}
void GFX_UpdateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* /*c*/, const std::vector<Vertex>& /*sv*/) {}
void GFX_DrawSkeletalMeshComp(SkeletalMesh3D* /*c*/) {}
bool GFX_IsCpuSkinningRequired(SkeletalMesh3D* /*c*/) { return true; }

void GFX_DrawShadowMeshComp(ShadowMesh3D* /*c*/) {}
void GFX_DrawInstancedMeshComp(InstancedMesh3D* /*c*/) {}

void GFX_CreateTextMeshCompResource(TextMesh3D* /*c*/) {}
void GFX_DestroyTextMeshCompResource(TextMesh3D* /*c*/) {}
void GFX_UpdateTextMeshCompVertexBuffer(TextMesh3D* /*c*/, const std::vector<Vertex>& /*v*/) {}
void GFX_DrawTextMeshComp(TextMesh3D* /*c*/) {}

void GFX_CreateVoxel3DResource(Voxel3D* /*v*/) {}
void GFX_DestroyVoxel3DResource(Voxel3D* /*v*/) {}
void GFX_UpdateVoxel3DResource(Voxel3D* /*v*/, const std::vector<VertexColor>& /*v*/, const std::vector<IndexType>& /*i*/) {}
void GFX_DrawVoxel3D(Voxel3D* /*v*/) {}

void GFX_CreateTerrain3DResource(Terrain3D* /*t*/) {}
void GFX_DestroyTerrain3DResource(Terrain3D* /*t*/) {}
void GFX_UpdateTerrain3DResource(Terrain3D* /*t*/, const std::vector<VertexColor>& /*v*/, const std::vector<IndexType>& /*i*/) {}
void GFX_DrawTerrain3D(Terrain3D* /*t*/) {}

void GFX_CreateTileMap2DResource(TileMap2D* /*t*/) {}
void GFX_DestroyTileMap2DResource(TileMap2D* /*t*/) {}
void GFX_UpdateTileMap2DResource(TileMap2D* /*t*/, const std::vector<VertexColor>& /*v*/, const std::vector<IndexType>& /*i*/) {}
void GFX_DrawTileMap2D(TileMap2D* /*t*/) {}

void GFX_CreateParticleCompResource(Particle3D* /*c*/) {}
void GFX_DestroyParticleCompResource(Particle3D* /*c*/) {}
void GFX_UpdateParticleCompVertexBuffer(Particle3D* /*c*/, const std::vector<VertexParticle>& /*v*/) {}
void GFX_DrawParticleComp(Particle3D* /*c*/) {}

void GFX_CreateQuadResource(Quad* /*q*/) {}
void GFX_DestroyQuadResource(Quad* /*q*/) {}
void GFX_UpdateQuadResourceVertexData(Quad* /*q*/) {}
void GFX_DrawQuad(Quad* /*q*/) {}

void GFX_CreateQuadBorderResource(Quad* /*q*/) {}
void GFX_DestroyQuadBorderResource(Quad* /*q*/) {}
void GFX_UpdateQuadBorderResourceVertexData(Quad* /*q*/) {}
void GFX_DrawQuadBorder(Quad* /*q*/) {}

void GFX_CreateTextResource(Text* /*t*/) {}
void GFX_DestroyTextResource(Text* /*t*/) {}
void GFX_UpdateTextResourceVertexData(Text* /*t*/) {}
void GFX_DrawText(Text* /*t*/) {}

void GFX_CreatePolyResource(Poly* /*p*/) {}
void GFX_DestroyPolyResource(Poly* /*p*/) {}
void GFX_UpdatePolyResourceVertexData(Poly* /*p*/) {}
void GFX_DrawPoly(Poly* /*p*/) {}

void GFX_DrawStaticMesh(StaticMesh* /*mesh*/, Material* /*material*/, const glm::mat4& /*transform*/, glm::vec4 /*color*/) {}

void GFX_RenderPostProcessPasses() {}

#endif // POLYPHASE_PLATFORM_ADDON
