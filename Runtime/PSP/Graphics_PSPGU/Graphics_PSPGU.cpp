/**
 * @file Graphics_PSPGU.cpp
 * @brief PSPGU renderer (Phase 3) — textured + lit static-mesh rendering.
 *
 *  Phase 1: engine boots, clears the screen, no draws.
 *  Phase 2: textured static meshes render with engine matrices + transforms.
 *  Phase 3: per-material shading (Lit / Unlit), translucency + masked /
 *           additive blend modes, hardware vertex lighting (up to 4 of the
 *           engine's gathered lights via sceGuLight0..3), sky ambient,
 *           per-material cull + depthless overrides.
 *
 *  Still stubbed for later phases:
 *    - **UI / Quad / Text / Poly** — implementations exist for Create /
 *      Destroy / Update, but the Draw paths are no-op'd. Enabling them
 *      produces a fullscreen green overlay from an engine-internal widget
 *      we haven't isolated yet. Phase 4 finishes the UI path.
 *    - Skeletal meshes (Phase 3.5: HW 8-bone or CPU skin fallback)
 *    - Simple shadows (Phase 4)
 *    - Particles, post-process, light bake — Vulkan-only by design
 *
 *  Critical invariants (do not regress):
 *    - All matrix uploads use raw `sceGuSetMatrix`. `sceGum*` corrupts state.
 *    - `GU_CLIP_PLANES` stays enabled. Disabling drops all 3D primitives.
 *    - `GFX_SetViewport` / `GFX_SetScissor` hardcode PSP 480×272 dims —
 *      engine input often comes from editor scene-tab dimensions.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Graphics/Graphics.h"
#include "Graphics/GraphicsConstants.h"
#include "Engine/Renderer.h"
#include "Engine/World.h"
#include "Engine/Assets/Material.h"
#include "Engine/Assets/MaterialLite.h"
#include "Engine/Assets/StaticMesh.h"
#include "Engine/Assets/Texture.h"
#include "Engine/Assets/Font.h"
#include "Engine/Nodes/3D/StaticMesh3d.h"
#include "Engine/Nodes/3D/Camera3d.h"
#include "Engine/Nodes/Widgets/Widget.h"
#include "Engine/Nodes/Widgets/Quad.h"
#include "Engine/Nodes/Widgets/Text.h"
#include "Engine/Nodes/Widgets/Poly.h"
#include "Log.h"

#include "Graphics_PSPGU/PSPGUTypes.h"
#include "Graphics_PSPGU/PSPGUUtils.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>

#include <algorithm>
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

    // Pack a glm::vec4 colour (0..1) as PSP ABGR 8888.
    inline uint32_t PackColorAbgr(const glm::vec4& c, float alphaOverride = -1.0f)
    {
        const float a = (alphaOverride >= 0.0f) ? alphaOverride : c.a;
        const uint8_t cr = (uint8_t)(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
        const uint8_t cg = (uint8_t)(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
        const uint8_t cb = (uint8_t)(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
        const uint8_t ca = (uint8_t)(glm::clamp(a,   0.0f, 1.0f) * 255.0f);
        return ((uint32_t)ca << 24) | ((uint32_t)cb << 16) | ((uint32_t)cg << 8) | cr;
    }

    // Upload up to 4 closest lights from the engine's gathered list to the GE.
    // PSP supports 4 hardware lights (GU_LIGHT0..3); engine's Renderer pre-culls
    // and sorts. Sky ambient goes via sceGuAmbient. Called once per Forward
    // pass — light state persists across draws inside the pass.
    void UploadLightData()
    {
        Renderer* renderer = Renderer::Get();
        World* world = renderer ? renderer->GetCurrentWorld() : nullptr;
        if (world == nullptr) return;

        // Vulkan/3DS use a shader-side `out *= u_color_scale` (default 2.0)
        // that doubles the final fragment to compensate for the engine's
        // vertex-color pre-divide. PSP fixed-function has no shader, so the
        // output is dim by the same factor. Pre-multiply ambient + light
        // intensities by the scale here; PackColorAbgr clamps to [0,1] so
        // bright lights saturate instead of overflowing.
        const float colorScale = renderer->GetColorScale();

        const glm::vec4 ambient = world->GetAmbientLightColor() * colorScale;
        sceGuAmbient(PackColorAbgr(ambient, 1.0f));

        const std::vector<LightData>& lights = renderer->GetLightData();
        const uint32_t kPspMaxLights = 4;
        const uint32_t count = (uint32_t)std::min((size_t)kPspMaxLights, lights.size());

        for (uint32_t i = 0; i < count; ++i)
        {
            const LightData& ld = lights[i];

            // GU_DIFFUSE_AND_SPECULAR — PSP supports diffuse-only too, but
            // engine lighting model expects full Phong. Specular value is
            // material-side via sceGuLightColor(GU_SPECULAR, ...) below.
            int psptype = GU_POINTLIGHT;
            ScePspFVector3 posOrDir;
            if (ld.mType == LightType::Directional)
            {
                psptype     = GU_DIRECTIONAL;
                posOrDir.x  = -ld.mDirection.x;
                posOrDir.y  = -ld.mDirection.y;
                posOrDir.z  = -ld.mDirection.z;
            }
            else
            {
                psptype     = GU_POINTLIGHT;
                posOrDir.x  = ld.mPosition.x;
                posOrDir.y  = ld.mPosition.y;
                posOrDir.z  = ld.mPosition.z;
            }

            sceGuLight(i, psptype, GU_DIFFUSE_AND_SPECULAR, &posOrDir);

            const glm::vec4 lightCol = ld.mColor * ld.mIntensity * colorScale;
            sceGuLightColor(i, GU_DIFFUSE,  PackColorAbgr(lightCol, 1.0f));
            sceGuLightColor(i, GU_SPECULAR, PackColorAbgr(lightCol, 1.0f));

            if (ld.mType == LightType::Point && ld.mRadius > 0.0f)
            {
                // Map radius → attenuation. PSP att model is
                // 1 / (const + linear*d + quadratic*d^2). Match the engine's
                // smoothstep falloff loosely with quadratic dominant.
                const float r = ld.mRadius;
                sceGuLightAtt(i,
                              1.0f,                       // constant
                              2.0f / r,                   // linear
                              1.0f / (r * r));            // quadratic
            }

            sceGuEnable(GU_LIGHT0 + i);
        }

        // Disable any leftover light slots from a previous frame with more
        // lights — otherwise stale data lights this frame too.
        for (uint32_t i = count; i < kPspMaxLights; ++i)
        {
            sceGuDisable(GU_LIGHT0 + i);
        }
    }

    // Apply per-material GE state: colour, shading model (lighting on/off),
    // blend mode, optional cull override, optional depthless. Called from
    // every draw that uses an engine material.
    void BindMaterial(MaterialLite* mat, bool useVertexColor)
    {
        if (mat == nullptr)
        {
            sceGuColor(0xFFFFFFFFu);
            sceGuDisable(GU_LIGHTING);
            sceGuDisable(GU_BLEND);
            sceGuEnable(GU_DEPTH_TEST);
            return;
        }

        const ShadingModel shading = mat->GetShadingModel();
        const BlendMode    blend   = mat->GetBlendMode();
        const glm::vec4    color   = mat->GetColor();
        const float        opacity = (blend == BlendMode::Translucent || blend == BlendMode::Additive)
                                     ? mat->GetOpacity()
                                     : 1.0f;

        sceGuColor(PackColorAbgr(color, opacity));

        // Lighting on/off. Toon falls back to Lit for V1 (no toon LUT yet —
        // Phase 4 adds it via per-vertex pre-shade or a 1D ramp texture).
        if (shading == ShadingModel::Unlit)
        {
            sceGuDisable(GU_LIGHTING);
        }
        else
        {
            sceGuEnable(GU_LIGHTING);
            // PSP default material colours are dim (~0.2). Without explicit
            // setup, lit fragments come out near-black. Set material to full
            // white so the lighting equation produces ambient + light
            // contributions at their authored intensities; sceGuAmbient
            // (set per-frame from World::GetAmbientLightColor) provides the
            // scene ambient level.
            sceGuModelColor(/*emission*/ 0x00000000u,
                            /*ambient */ 0xFFFFFFFFu,
                            /*diffuse */ 0xFFFFFFFFu,
                            /*specular*/ 0xFFFFFFFFu);
        }

        // Blend mode → GE blend state.
        switch (blend)
        {
            case BlendMode::Translucent:
                sceGuEnable(GU_BLEND);
                sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
                sceGuDepthMask(GU_TRUE);     // no depth writes for translucent
                break;
            case BlendMode::Additive:
                sceGuEnable(GU_BLEND);
                sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFFu);
                sceGuDepthMask(GU_TRUE);
                break;
            case BlendMode::Masked:
                sceGuDisable(GU_BLEND);
                sceGuEnable(GU_ALPHA_TEST);
                sceGuAlphaFunc(GU_GREATER, 127, 0xFF);
                sceGuDepthMask(GU_FALSE);
                break;
            case BlendMode::Opaque:
            default:
                sceGuDisable(GU_BLEND);
                sceGuDisable(GU_ALPHA_TEST);
                sceGuDepthMask(GU_FALSE);
                break;
        }

        // Depthless materials (UI overlays, skybox) bypass depth.
        if (mat->IsDepthTestDisabled())
        {
            sceGuDisable(GU_DEPTH_TEST);
        }
        else
        {
            sceGuEnable(GU_DEPTH_TEST);
        }

        // Cull mode override from material. Engine meshes are CCW-front in
        // world space (matching 3DS port's GPU_CULL_BACK_CCW). On PSP that
        // means CullMode::Back → GU_CCW front (so CW back-faces get culled).
        // Earlier "viewport Y-flip inverts winding" theory was wrong; CCW
        // here matches both the engine's world winding and what visibly
        // produces outside-of-cube rendering.
        const CullMode cull = mat->GetCullMode();
        if (cull == CullMode::None)
        {
            sceGuDisable(GU_CULL_FACE);
        }
        else
        {
            sceGuEnable(GU_CULL_FACE);
            sceGuFrontFace(cull == CullMode::Back ? GU_CCW : GU_CW);
        }

        (void)useVertexColor;   // GE consumes per-vertex colour automatically
                                // when GU_COLOR_8888 is in the vtype mask.
    }

    // -------------------------------------------------------------------------
    // 2D UI vertex path (Phase 3 — Quad / Text / Poly widgets)
    // -------------------------------------------------------------------------
    // Engine `VertexUI` is { vec2 pos, vec2 tex, uint32 color } = 20 B.
    // PSP HW vertex field order is mandatory tex → color → pos. Repack into
    // 24-byte `PspUIVertex` with z=0 for depth (TRANSFORM_2D still requires
    // 3 vertex components per VERTEX_32BITF, even though x/y are screen-pixel
    // and z is depth-only).
    struct __attribute__((aligned(4))) PspUIVertex
    {
        float    u, v;
        uint32_t color;
        float    x, y, z;   // z=0 for all UI
    };
    static_assert(sizeof(PspUIVertex) == 24, "PSP UI vertex must be 24 bytes");

    constexpr uint32_t kUIVertexFlags =
        GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D;

    // Repack a run of engine VertexUI into PSP layout. Writes to `dst` which
    // must hold at least `count * sizeof(PspUIVertex)` bytes.
    void RepackUIVertices(const VertexUI* src, uint32_t count, PspUIVertex* dst)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            const VertexUI& s = src[i];
            PspUIVertex& d = dst[i];
            d.u     = s.mTexcoord.x;
            d.v     = s.mTexcoord.y;
            // Engine packs RGBA (R in low byte); PSP GU_COLOR_8888 wants the
            // same byte order so a direct copy is correct on a little-endian
            // host. Verified by Phase 2 vertex colours rendering as authored.
            d.color = s.mColor;
            d.x     = s.mPosition.x;
            d.y     = s.mPosition.y;
            d.z     = 0.0f;
        }
    }

    // Allocate/grow a UI vertex buffer to hold at least `bytes` bytes.
    // Returns the (possibly new) buffer. Old contents are discarded.
    void* EnsureUICapacity(void** bufp, uint32_t* capp, uint32_t bytes)
    {
        if (*capp >= bytes && *bufp != nullptr) return *bufp;
        if (*bufp != nullptr) { free(*bufp); *bufp = nullptr; }
        *bufp = memalign(16, bytes);
        *capp = (*bufp != nullptr) ? bytes : 0;
        return *bufp;
    }

    // Scratch buffer for per-draw CPU transforms (used by Text — engine
    // builds text verts in font-cursor local space, but PSP TRANSFORM_2D has
    // no matrix slot so we bake the rect-offset + scale into positions each
    // draw). Grows on demand.
    static void*    sUIScratch    = nullptr;
    static uint32_t sUIScratchCap = 0;

    void* GetUIScratch(uint32_t bytes)
    {
        if (sUIScratchCap >= bytes && sUIScratch != nullptr) return sUIScratch;
        if (sUIScratch != nullptr) { free(sUIScratch); sUIScratch = nullptr; }
        sUIScratch = memalign(16, bytes);
        sUIScratchCap = (sUIScratch != nullptr) ? bytes : 0;
        return sUIScratch;
    }

    // Per-widget upload helper used by Quad / Text / Poly. Repacks engine
    // VertexUI into PSP layout, ensures the dest buffer is large enough,
    // and flushes dcache so the GE sees the new bytes.
    //
    // Bounds-checks `count` against a sane max (UI widgets rarely exceed a
    // few hundred verts). Scene instantiation calls Update before the widget
    // has computed its vertex array, so count CAN be 0 or junk from a
    // not-yet-initialised widget. Defending against both.
    constexpr uint32_t kMaxUIVertsPerDraw = 16384;       // ~384 KB / draw

    void UploadUIVertices(void** bufp, uint32_t* capp,
                         const VertexUI* src, uint32_t count)
    {
        if (count == 0 || src == nullptr || bufp == nullptr || capp == nullptr) return;
        if (count > kMaxUIVertsPerDraw) return;          // bogus / uninit'd
        const uint32_t bytes = count * sizeof(PspUIVertex);
        void* dst = EnsureUICapacity(bufp, capp, bytes);
        if (dst == nullptr) return;
        RepackUIVertices(src, count, static_cast<PspUIVertex*>(dst));
        sceKernelDcacheWritebackRange(dst, bytes);
    }

    // Bind a UI texture (Quad's texture, font atlas, etc) with sensible
    // defaults for screen-space alpha-blended UI. Falls back to white when
    // null. Uses GU_TFX_MODULATE + GU_TCC_RGBA so per-vertex colour and
    // alpha both apply.
    void BindUITexture(Texture* tex)
    {
        if (tex == nullptr || tex->GetResource() == nullptr ||
            tex->GetResource()->mPixels == nullptr)
        {
            sceGuTexImage(0, 2, 2, 2, sWhiteTexel);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            sceGuTexWrap(GU_CLAMP, GU_CLAMP);
            sceGuTexScale(1.0f, 1.0f);
            sceGuTexOffset(0.0f, 0.0f);
            sceGuEnable(GU_TEXTURE_2D);
            return;
        }
        TextureResource* r = tex->GetResource();
        const uint32_t texBytes = (uint32_t)r->mBufWidth * r->mHeight * 4u;
        sceKernelDcacheWritebackRange(r->mPixels, texBytes);
        sceGuTexMode(r->mPsm, 0, 0, r->mSwizzled);
        sceGuTexImage(0, (int)r->mWidth, (int)r->mHeight, (int)r->mBufWidth, r->mPixels);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        sceGuSetMatrix(GU_TEXTURE, &sIdentityMtx);
        sceGuEnable(GU_TEXTURE_2D);
    }

    // Common state setup for all UI draws: alpha blend on, depth off, cull
    // off, lighting off. Per-draw color set by the caller via sceGuColor.
    void SetupUIPipeline()
    {
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDisable(GU_CULL_FACE);
        sceGuDisable(GU_LIGHTING);
        sceGuDisable(GU_FOG);
        sceGuDisable(GU_ALPHA_TEST);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    }

    // Bind a Texture asset's resource to TMU 0. nullptr ⇒ fall back to the
    // built-in white 1×1 so untextured materials still render.
    void BindTexture(Texture* tex)
    {
        if (tex == nullptr)
        {
            // White fallback. GU_TFX_MODULATE (not REPLACE) so the material's
            // colour set via sceGuColor in BindMaterial passes through —
            // otherwise an untextured brown-coloured material renders as
            // pure white.
            sceGuTexImage(0, 2, 2, 2, sWhiteTexel);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
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
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_REPEAT, GU_REPEAT);
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

    // TEMP DEBUG: cull face DISABLED at init (matches Phase 2 working state).
    // Phase 3 enabling cull broke rendering — investigating whether the
    // breakage is in init or in BindMaterial/UploadLightData.
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

    glm::vec4 cc = Renderer::Get() ? Renderer::Get()->GetClearColor() : glm::vec4(0, 0, 0, 1);
    cc.a = 1.0f;  // force opaque
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
        UploadLightData();
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

    static int sTexLog = 0;
    if (sTexLog < 32)
    {
        // Quick sample of first non-zero pixel to see if texture content is
        // real or all-white/all-zero.
        const uint8_t* px = (const uint8_t*)dst;
        uint32_t firstNonZero = 0;
        for (uint32_t i = 0; i < bytes; i += 4)
        {
            if (px[i] || px[i+1] || px[i+2])
            {
                firstNonZero = *(const uint32_t*)(px + i);
                break;
            }
        }
        LogDebug("PSPGU.CreateTex[%d]: %ux%u bufW=%u bytes=%u firstRgba=0x%08X tex=%p",
                 sTexLog, srcW, srcH, bufW, bytes, firstNonZero, (void*)texture);
        ++sTexLog;
    }
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

    static int sDrawLogCount = 0;
    if (sDrawLogCount < 8)
    {
        Material* matBaseDbg = comp->GetMaterial();
        MaterialLite* matDbg = Material::AsLite(matBaseDbg ? matBaseDbg : Renderer::Get()->GetDefaultMaterial());
        Texture* texDbg = matDbg ? matDbg->GetTexture(0) : nullptr;
        LogDebug("PSPGU.Draw[%d]: idx=%u stride=%u flags=0x%X tex=%p shading=%d blend=%d",
                 sDrawLogCount,
                 r->mNumIndices, r->mVertexStride, r->mVertexFlags, (void*)texDbg,
                 matDbg ? (int)matDbg->GetShadingModel() : -1,
                 matDbg ? (int)matDbg->GetBlendMode() : -1);
        ++sDrawLogCount;
    }

    // Phase 3: full per-draw material + texture binding.
    Material* matBase = comp->GetMaterial();
    MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
    BindMaterial(mat, /*useVertexColor=*/false);
    BindTexture(mat ? mat->GetTexture(0) : nullptr);

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

// =============================================================================
// UI — Quad / Text / Poly (Phase 3)
// =============================================================================
//
// All UI draws use GU_TRANSFORM_2D so vertex positions are raw screen pixels
// (origin top-left, 0..480 / 0..272). PSP's GE applies viewport but no matrix
// transform. Engine widgets pre-position their vertices in screen space, so
// we just repack VertexUI → PspUIVertex once per UpdateXXX call and replay
// with sceGuDrawArray.

void GFX_CreateQuadResource(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetResource();
    if (r == nullptr) return;
    const uint32_t bytes = Quad::kMaxQuadVertices * sizeof(PspUIVertex);
    EnsureUICapacity(&r->mVertexData, &r->mVertexCapacity, bytes);
}

void GFX_DestroyQuadResource(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { free(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexCapacity = 0;
}

void GFX_UpdateQuadResourceVertexData(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetResource();
    if (r == nullptr) return;
    UploadUIVertices(&r->mVertexData, &r->mVertexCapacity,
                     quad->GetVertices(), quad->GetNumVertices());
}

void GFX_DrawQuad(Quad* /*quad*/)
{
    // Deferred to Phase 4. Re-enabling produces a full-screen green overlay
    // from some engine-internal widget that fires Quad/Text draws even when
    // the user's scene has no real widget nodes. Investigate in Phase 4.
}

void GFX_CreateQuadBorderResource(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetBorderResource();
    if (r == nullptr) return;
    const uint32_t bytes = Quad::kMaxQuadVertices * sizeof(PspUIVertex);
    EnsureUICapacity(&r->mVertexData, &r->mVertexCapacity, bytes);
}

void GFX_DestroyQuadBorderResource(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetBorderResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { free(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexCapacity = 0;
}

void GFX_UpdateQuadBorderResourceVertexData(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetBorderResource();
    if (r == nullptr) return;
    UploadUIVertices(&r->mVertexData, &r->mVertexCapacity,
                     quad->GetBorderVertices(), quad->GetBorderNumVertices());
}

void GFX_DrawQuadBorder(Quad* /*quad*/) {}   // Deferred to Phase 4.

// ---- Text -------------------------------------------------------------------

void GFX_CreateTextResource(Text* text)
{
    // Lazy allocation — first UpdateXXX call sizes the buffer to whatever the
    // text node has reserved. Avoids paying for a max-size buffer for every
    // widget tree even if most are empty.
    (void)text;
}

void GFX_DestroyTextResource(Text* text)
{
    if (text == nullptr) return;
    TextResource* r = text->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { free(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexCapacity = 0;
    r->mNumBufferCharsAllocated = 0;
}

void GFX_UpdateTextResourceVertexData(Text* text)
{
    if (text == nullptr) return;
    TextResource* r = text->GetResource();
    if (r == nullptr) return;

    const uint32_t numCharsAlloc = text->GetNumCharactersAllocated();
    if (numCharsAlloc == 0 || text->GetText().empty()) return;

    const uint32_t bytes = numCharsAlloc * TEXT_VERTS_PER_CHAR * sizeof(PspUIVertex);
    if (r->mVertexCapacity < bytes)
    {
        if (r->mVertexData != nullptr) { free(r->mVertexData); r->mVertexData = nullptr; }
        r->mVertexData = memalign(16, bytes);
        r->mVertexCapacity = (r->mVertexData != nullptr) ? bytes : 0;
        r->mNumBufferCharsAllocated = (r->mVertexData != nullptr) ? numCharsAlloc : 0;
    }

    if (r->mVertexData == nullptr) return;

    // Repack the full allocated-chars range — engine writes blank verts to
    // unused slots already, so this is safe and stays cache-friendly.
    RepackUIVertices(text->GetVertices(),
                     numCharsAlloc * TEXT_VERTS_PER_CHAR,
                     static_cast<PspUIVertex*>(r->mVertexData));
    sceKernelDcacheWritebackRange(r->mVertexData, bytes);
}

void GFX_DrawText(Text* /*text*/) {}   // Deferred to Phase 4.

// ---- Poly -------------------------------------------------------------------

void GFX_CreatePolyResource(Poly* /*poly*/)
{
    // Lazy — first Update sizes the buffer.
}

void GFX_DestroyPolyResource(Poly* poly)
{
    if (poly == nullptr) return;
    PolyResource* r = poly->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { free(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexCapacity = 0;
}

void GFX_UpdatePolyResourceVertexData(Poly* poly)
{
    if (poly == nullptr) return;
    PolyResource* r = poly->GetResource();
    if (r == nullptr) return;
    UploadUIVertices(&r->mVertexData, &r->mVertexCapacity,
                     poly->GetVertices(), poly->GetNumVertices());
}

void GFX_DrawPoly(Poly* /*poly*/) {}   // Deferred to Phase 4.

void GFX_DrawStaticMesh(StaticMesh* /*mesh*/, Material* /*material*/, const glm::mat4& /*transform*/, glm::vec4 /*color*/) {}

void GFX_RenderPostProcessPasses() {}

#endif // POLYPHASE_PLATFORM_ADDON
