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
#include "Engine/Maths.h"   // DEGREES_TO_RADIANS for widget rotation
#include "Engine/Renderer.h"
#include "Engine/World.h"
#include "Engine/Assets/Material.h"
#include "Engine/Assets/MaterialLite.h"
#include "Engine/Assets/StaticMesh.h"
#include "Engine/Assets/SkeletalMesh.h"
#include "Engine/Assets/Texture.h"
#include "Engine/Assets/Font.h"
#include "Engine/Nodes/3D/StaticMesh3d.h"
#include "Engine/Nodes/3D/ShadowMesh3d.h"
#include "Engine/Nodes/3D/SkeletalMesh3d.h"
#include "Engine/Nodes/3D/InstancedMesh3d.h"
#include "Engine/Nodes/3D/Particle3d.h"
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
    //
    // 4x4 (not 2x2) because PSP requires the texture buffer's row STRIDE to
    // be a multiple of 16 BYTES — for PSM_8888 (4 B/texel) that's a
    // bufWidth of at least 4 texels. Calling sceGuTexImage with bufWidth=2
    // (stride=8 B) causes the GE's texture-sampler DMA to read row 0
    // correctly but row 1 from the wrong offset (past the buffer), giving
    // garbage / transparent results in the bottom half of the texture.
    //
    // Symptom: untextured widgets (Button quad fills, QuadBorder, Poly
    // lines, anything using the white fallback) rendered with their top
    // half tinted correctly and their bottom half empty / transparent —
    // because UV.y < 0.5 sampled valid white texels but UV.y >= 0.5
    // sampled garbage. Fixed by making the fallback 4x4 and passing
    // bufWidth=4 to sceGuTexImage.
    static uint32_t __attribute__((aligned(16))) sWhiteTexel[16] = {
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
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

        // colorScale (default 2.0) restores vertex colours the engine pre-divides
        // by 2. GX applies it as a FINAL output multiply, computing lighting at
        // half-brightness first so nothing saturates. PSP fixed-function has no
        // final output-scale stage, so ambient must be used AS-IS: pre-multiplying
        // it by colorScale pushes a normal ambient (e.g. 0.63 -> 1.26) past 1.0,
        // saturating every surface to full white and flattening ALL directional
        // shading. The (scaled) light below then provides the gradient on top.
        const float colorScale = renderer->GetColorScale();

        const glm::vec4 ambient = world->GetAmbientLightColor();
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
                psptype     = (ld.mType == LightType::Spot) ? GU_SPOTLIGHT : GU_POINTLIGHT;
                posOrDir.x  = ld.mPosition.x;
                posOrDir.y  = ld.mPosition.y;
                posOrDir.z  = ld.mPosition.z;
            }

            sceGuLight(i, psptype, GU_DIFFUSE_AND_SPECULAR, &posOrDir);

            if (ld.mType == LightType::Spot)
            {
                // GE spot direction points along the beam (GL convention); cutoff
                // register takes the cosine of the outer half-angle. Exponent is
                // picked so intensity reaches ~50% at the inner half-angle.
                ScePspFVector3 spotDir;
                spotDir.x = ld.mDirection.x;
                spotDir.y = ld.mDirection.y;
                spotDir.z = ld.mDirection.z;

                float cosOuter = cosf(glm::radians(glm::clamp(ld.mOuterConeAngle, 0.1f, 89.9f)));
                float cosInner = cosf(glm::radians(glm::clamp(ld.mInnerConeAngle, 0.0f, 89.0f)));
                cosInner = std::min(cosInner, 0.9999f);
                float exponent = glm::clamp(logf(0.5f) / logf(cosInner), 0.0f, 128.0f);

                sceGuLightSpot(i, &spotDir, exponent, cosOuter);
            }

            const glm::vec4 lightCol = ld.mColor * ld.mIntensity * colorScale;
            sceGuLightColor(i, GU_DIFFUSE,  PackColorAbgr(lightCol, 1.0f));
            sceGuLightColor(i, GU_SPECULAR, PackColorAbgr(lightCol, 1.0f));

            if ((ld.mType == LightType::Point || ld.mType == LightType::Spot) && ld.mRadius > 0.0f)
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

    // Frame-scoped ring buffer for per-draw CPU vertex transforms
    // (ApplyUIDrawTransform writes UV-scaled + tint-modulated + position-
    // baked vertex copies here, since Text/Quad widgets can't be modified
    // in-place — the next dirty cycle would re-apply the transform on
    // already-transformed data).
    //
    // The ring is reset at GFX_BeginFrame and never freed mid-frame, so a
    // slice handed out to one draw stays valid until sceGuSync at end-of-
    // frame. This is mandatory on PSP: sceGuDrawArray records the vertex
    // POINTER into the GE command list and the GE consumes it
    // asynchronously between sceGuFinish/sceGuSync, NOT at the call site.
    // Reusing a single scratch buffer means every draw's pointer aliases
    // the same memory, so the GE reads whatever the LAST writer left
    // behind. Symptom: button quads rendering with vertices from some
    // unrelated text draw — "anchored at top, bottom empty" was caused by
    // exactly this aliasing.
    //
    // 256 KB is plenty: each Phase 4 UI vertex is 24 B, and even a busy
    // frame with a few hundred buttons/text widgets at ~50 verts each
    // tops out around 100 KB. Grows on overflow so we never silently drop.
    static constexpr uint32_t kUIRingInitialBytes = 256 * 1024;
    static void*    sUIRing        = nullptr;
    static uint32_t sUIRingCap     = 0;
    static uint32_t sUIRingOffset  = 0;

    void* GetUIScratch(uint32_t bytes)
    {
        // 16-byte align each slice — PSP DMA requires it for vertex reads.
        const uint32_t alignedBytes = (bytes + 15u) & ~15u;

        // Grow on first call or on overflow. Free + reallocate (rather
        // than realloc) because any extant slices belong to in-flight
        // sceGuDrawArray pointers that won't be touched until sceGuSync
        // at end-of-frame, but we only grow at the START of a draw before
        // any GE work has begun for the current frame's UI batch — so it's
        // safe to drop the old buffer.
        if (sUIRing == nullptr ||
            sUIRingOffset + alignedBytes > sUIRingCap)
        {
            const uint32_t newCap =
                glm::max(sUIRingCap > 0 ? sUIRingCap * 2 : kUIRingInitialBytes,
                         sUIRingOffset + alignedBytes);
            if (sUIRing != nullptr) free(sUIRing);
            sUIRing       = memalign(16, newCap);
            sUIRingCap    = (sUIRing != nullptr) ? newCap : 0;
            sUIRingOffset = 0;
        }
        if (sUIRing == nullptr) return nullptr;

        uint8_t* slice = static_cast<uint8_t*>(sUIRing) + sUIRingOffset;
        sUIRingOffset += alignedBytes;
        return slice;
    }

    // Reset the ring at frame start. Safe because sceGuSync at the end of
    // the previous frame guarantees the GE has consumed every pointer
    // handed out from the ring during that frame.
    void ResetUIScratch() { sUIRingOffset = 0; }

    // Returns the buffer the GE should draw from after applying the widget's
    // uniform tint, scaling normalised UVs to PSP texel units, and optionally
    // baking a per-vertex position scale+translate.
    //
    // Three transforms baked into one scratch pass:
    //
    //   (1) Tint. Vulkan/GX apply widget uniform colour in a shader / TEV
    //       stage; PSP has no shader and only one TEV stage (sceGuTexFunc),
    //       so we pre-multiply tint into per-vertex colour.
    //
    //   (2) UV scale to texel units. PSP GU_TRANSFORM_2D ("through" mode)
    //       interprets float texcoords as TEXEL coords, NOT normalised
    //       0..1 like 3D mode. sceGuTexScale doesn't apply in through mode,
    //       so we have to bake the multiply into the vertices: each UV gets
    //       multiplied by the bound texture's width/height.
    //
    //   (3) Position transform: vertex.xy = vertex.xy * posScale + posOffset.
    //       Quad/Poly emit screen-space-pixel vertices already (posScale=1,
    //       posOffset=0 is the right no-op). Text emits font-native-pixel,
    //       widget-local vertices (cursor starts at 0,0 in local space at
    //       font's native point size) — Vulkan's text shader bakes
    //       posOffset=(rect.mX + justifiedOffset, rect.mY + justifiedOffset)
    //       and posScale=(scaledTextSize / fontSize) into a uniform. PSP has
    //       no shader; bake the same multiply+translate into the vertex
    //       buffer here.
    //
    //   (4) Rotation around screen-space pivot. Widget::mTransform is a mat3
    //       that's *supposed* to encode T(pivot) * R(angle) * T(-pivot), but
    //       the engine builds it as mat4(translate*rotate*translate) and then
    //       truncates to mat3 — which silently drops the translation columns
    //       (glm's mat3(mat4) keeps only the upper-left 3x3). So we ignore
    //       mTransform and apply rotation around pivot directly from
    //       widget->GetRotation() / GetPivot(). Pivot must be in SCREEN space
    //       (rect.mX + rect.mWidth*pivot.x, ditto Y) AFTER (3) has been
    //       applied — otherwise text vertices would rotate around the wrong
    //       point because their (3)-baked screen position differs from the
    //       widget rect coordinates.
    //
    // Returns nullptr to signal "skip the draw" when alpha rounds to 0.
    void* ApplyUIDrawTransform(const void* srcBuf, uint32_t numVerts,
                               const glm::vec4& tint, Texture* tex,
                               glm::vec2 posScale  = glm::vec2(1.0f, 1.0f),
                               glm::vec2 posOffset = glm::vec2(0.0f, 0.0f),
                               float rotRadians    = 0.0f,
                               glm::vec2 pivot     = glm::vec2(0.0f, 0.0f))
    {
        if (srcBuf == nullptr || numVerts == 0) return nullptr;

        const float tr = glm::clamp(tint.r, 0.0f, 1.0f);
        const float tg = glm::clamp(tint.g, 0.0f, 1.0f);
        const float tb = glm::clamp(tint.b, 0.0f, 1.0f);
        const float ta = glm::clamp(tint.a, 0.0f, 1.0f);

        if ((uint8_t)(ta * 255.0f) == 0) return nullptr;

        // Texture dims for the UV multiply. Null / unloaded textures fall
        // through to a 4x4 white fallback in BindUITexture (was 2x2 — PSP
        // PSM_8888 needs bufWidth >= 4 to satisfy the 16-byte row-stride
        // requirement; see sWhiteTexel comment). All 16 texels are white,
        // so any UV samples white regardless of how we scale here.
        float uScale = 1.0f;
        float vScale = 1.0f;
        if (tex != nullptr && tex->GetResource() != nullptr &&
            tex->GetResource()->mPixels != nullptr)
        {
            uScale = (float)tex->GetResource()->mWidth;
            vScale = (float)tex->GetResource()->mHeight;
        }
        else
        {
            uScale = 4.0f;
            vScale = 4.0f;
        }

        // Rotation fast-path: rotRadians ~= 0 means widget isn't rotated, so
        // we skip the per-vertex sin/cos / pivot translate entirely.
        const bool rotated = (fabsf(rotRadians) > 1e-6f);
        const float cs = rotated ? cosf(rotRadians) : 1.0f;
        const float sn = rotated ? sinf(rotRadians) : 0.0f;

        const uint32_t bytes = numVerts * sizeof(PspUIVertex);
        void* dst = GetUIScratch(bytes);
        if (dst == nullptr) return const_cast<void*>(srcBuf);

        const PspUIVertex* s = static_cast<const PspUIVertex*>(srcBuf);
        PspUIVertex* d = static_cast<PspUIVertex*>(dst);
        for (uint32_t i = 0; i < numVerts; ++i)
        {
            d[i].u = s[i].u * uScale;
            d[i].v = s[i].v * vScale;

            float x = s[i].x * posScale.x + posOffset.x;
            float y = s[i].y * posScale.y + posOffset.y;

            if (rotated)
            {
                const float dx = x - pivot.x;
                const float dy = y - pivot.y;
                x = pivot.x + dx * cs - dy * sn;
                y = pivot.y + dx * sn + dy * cs;
            }

            d[i].x = x;
            d[i].y = y;
            d[i].z = s[i].z;

            // engine packs R in low byte (verified Phase 2)
            const uint32_t c = s[i].color;
            const uint8_t r8 = (uint8_t)(((c >>  0) & 0xFF) * tr);
            const uint8_t g8 = (uint8_t)(((c >>  8) & 0xFF) * tg);
            const uint8_t b8 = (uint8_t)(((c >> 16) & 0xFF) * tb);
            const uint8_t a8 = (uint8_t)(((c >> 24) & 0xFF) * ta);
            d[i].color = ((uint32_t)a8 << 24) | ((uint32_t)b8 << 16) |
                         ((uint32_t)g8 <<  8) |  (uint32_t)r8;
        }
        sceKernelDcacheWritebackRange(dst, bytes);
        return dst;
    }

    // Compute widget's screen-space pivot point + rotation in radians for
    // ApplyUIDrawTransform. mRect already has the pivot shift applied (see
    // Widget::UpdateRect lines around 745), so the pivot in screen space is
    // simply rect.mX + rect.mWidth * pivot.x, ditto Y.
    void GetWidgetRotation(Widget* w, float& outRadians, glm::vec2& outPivot)
    {
        const Rect r = w->GetRect();
        const glm::vec2 p = w->GetPivot();
        outRadians = w->GetRotation() * DEGREES_TO_RADIANS;
        outPivot   = glm::vec2(r.mX + r.mWidth * p.x,
                               r.mY + r.mHeight * p.y);
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

    // Map engine FilterType -> PSP GU filter constant. PSP only exposes
    // NEAREST and LINEAR (no separate min/mag/mipmap variants in this build).
    int FilterToGu(FilterType ft)
    {
        return (ft == FilterType::Nearest) ? GU_NEAREST : GU_LINEAR;
    }

    // Map engine WrapMode -> PSP GU wrap constant. PSP has only CLAMP and
    // REPEAT — Mirror falls back to REPEAT (closest behavioural neighbour;
    // a true mirror would need CPU-side UV flipping).
    int WrapToGu(WrapMode wm)
    {
        return (wm == WrapMode::Clamp) ? GU_CLAMP : GU_REPEAT;
    }

    // Bind a UI texture (Quad's texture, font atlas, etc) with sensible
    // defaults for screen-space alpha-blended UI. Falls back to white when
    // null. Uses GU_TFX_MODULATE + GU_TCC_RGBA so per-vertex colour and
    // alpha both apply.
    //
    // Note: per-texel UV scaling for TRANSFORM_2D ("through") mode happens
    // in ApplyUIDrawTransform — sceGuTexScale doesn't apply in through mode
    // so the multiply has to be baked into the vertex buffer.
    void BindUITexture(Texture* tex)
    {
        if (tex == nullptr || tex->GetResource() == nullptr ||
            tex->GetResource()->mPixels == nullptr)
        {
            // 4x4 white fallback — see sWhiteTexel comment for why 2x2 is
            // unsafe on PSP (bufWidth < 4 violates 16-byte stride alignment
            // for PSM_8888 and the GE reads garbage on the second row).
            sceGuTexImage(0, 4, 4, 4, sWhiteTexel);
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
        sceGuTexFilter(FilterToGu(tex->GetFilterType()), FilterToGu(tex->GetFilterType()));
        sceGuTexWrap(WrapToGu(tex->GetWrapMode()), WrapToGu(tex->GetWrapMode()));
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
            // 4x4 white fallback — see sWhiteTexel comment for why 2x2 is
            // unsafe on PSP (bufWidth=2 violates the 16-byte stride
            // requirement for PSM_8888 and the second row reads garbage).
            sceGuTexImage(0, 4, 4, 4, sWhiteTexel);
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
        // Honour the texture asset's filter + wrap mode (see BindUITexture
        // comment — same hardcoding bug affected 3D meshes; cube material
        // marked Nearest was bleeding into smooth gradients).
        sceGuTexFilter(FilterToGu(tex->GetFilterType()), FilterToGu(tex->GetFilterType()));
        sceGuTexWrap(WrapToGu(tex->GetWrapMode()), WrapToGu(tex->GetWrapMode()));
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

    // Reset the UI-vertex ring buffer so this frame's draws start with a
    // clean offset. The previous frame's sceGuSync at GFX_EndFrame already
    // guaranteed the GE has consumed every pointer we handed out last
    // frame — safe to overwrite from byte 0 again.
    ResetUIScratch();

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

void GFX_SetFog(const FogSettings& fogSettings)
{
    if (!sGuInitialised) return;

    if (fogSettings.mEnabled)
    {
        // PSP GE fog is linear distance (eye-space Z) fog: clear at 'near', fully
        // fogged at 'far'. There is no hardware exponential mode, so Exponential
        // falls back to the same linear ramp. Called once per frame from the
        // engine's BeginFrame; the UI pipeline disables GU_FOG so 2D isn't fogged.
        const uint32_t color = PackColorAbgr(fogSettings.mColor, 1.0f);
        sceGuFog(fogSettings.mNear, fogSettings.mFar, color);
        sceGuEnable(GU_FOG);
    }
    else
    {
        sceGuDisable(GU_FOG);
    }
}
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

    // PSP sampler requires power-of-two width/height for the dims passed to
    // sceGuTexImage — non-POT dims get HW-clamped down to the next-lower
    // POT (so a 256x144 video texture binds as 256x128 and the bottom 16
    // rows are unsampleable). Pad both axes UP to next POT, copy source
    // pixels into the top-left of the padded buffer, and tell the engine
    // the visible region via Texture::SetUVMax so the Quad widget
    // multiplies its UVs and stops sampling at the content edge. Same
    // pattern Wii/GX uses for its non-multiple-of-4 streaming textures
    // (Graphics_GX.cpp ~390). bufWidth must additionally be a multiple of
    // 16 pixels (= 64 B stride at RGBA8); POT widths ≥ 16 satisfy this
    // trivially.
    auto nextPow2 = [](uint32_t v) -> uint32_t {
        if (v <= 1u) return 1u;
        --v;
        v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
        return v + 1u;
    };
    uint32_t potW = nextPow2(srcW);
    uint32_t potH = nextPow2(srcH);
    if (potW < 16u) potW = 16u;
    const uint32_t bufW  = potW;
    const uint32_t bytes = bufW * potH * 4u;

    void* dst = memalign(16, bytes);
    if (dst == nullptr) return;

    // Copy source rows into top-left of the padded buffer; replicate the
    // last source column/row into the right/bottom padding so linear
    // filtering near the content edge doesn't blend into garbage. UVMax
    // (set below) means the padding bands aren't normally sampled at all
    // — the edge-replication is a belt-and-braces safety for off-by-one
    // texel sampling.
    {
        const uint8_t* src = pixels.data();
        uint8_t* d = (uint8_t*)dst;
        for (uint32_t y = 0; y < srcH; ++y)
        {
            memcpy(d + y * bufW * 4, src + y * srcW * 4, srcW * 4);
            if (srcW < bufW)
            {
                const uint32_t lastPx = *reinterpret_cast<const uint32_t*>(d + y * bufW * 4 + (srcW - 1) * 4);
                uint32_t* dpad = reinterpret_cast<uint32_t*>(d + y * bufW * 4 + srcW * 4);
                for (uint32_t x = srcW; x < bufW; ++x) *dpad++ = lastPx;
            }
        }
        if (srcH < potH)
        {
            const uint8_t* lastRow = (uint8_t*)dst + (srcH - 1) * bufW * 4;
            for (uint32_t y = srcH; y < potH; ++y)
                memcpy((uint8_t*)dst + y * bufW * 4, lastRow, bufW * 4);
        }
    }

    // Tell the engine the visible sub-rect of the POT buffer so the Quad
    // widget (and any 3D mesh sampling this texture) crops UVs to the
    // content region. Without this the renderer assumes UV (0..1) maps to
    // the full physical extent — content gets letterboxed into the upper-
    // left of the visible area, OR (since most 3D meshes ignore UVMax and
    // just sample 0..1) the padding bands become visible as black strips.
    texture->SetUVMax(glm::vec2(float(srcW) / float(bufW),
                                float(srcH) / float(potH)));

    r->mPixels   = dst;
    r->mWidth    = bufW;   // PSP HW samples up to mWidth × mHeight — must be POT
    r->mHeight   = potH;
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

void GFX_UpdateTextureResourcePixels(Texture* texture, const uint8_t* src,
                                     uint32_t srcWidth, uint32_t srcHeight)
{
    // Streaming texture upload — used by the VideoPlayer addon to push each
    // decoded video frame into the existing texture's GPU buffer. Same
    // RGBA8 layout + 16-pixel pitch alignment as GFX_CreateTextureResource;
    // we just skip the (re)allocation and copy into the existing slot.
    //
    // The decoder always submits frames at the texture's authored
    // dimensions, but the caller's `srcWidth/srcHeight` could differ if a
    // codec returned an off-by-one frame, so we clip to whichever is
    // smaller. Any unwritten rows just keep their previous frame's pixels
    // — better than leaving them as garbage / black slits.
    if (texture == nullptr || src == nullptr || srcWidth == 0 || srcHeight == 0) return;
    TextureResource* r = texture->GetResource();
    if (r == nullptr || r->mPixels == nullptr) return;
    if (r->mWidth == 0 || r->mHeight == 0 || r->mBufWidth == 0) return;

    const uint32_t copyW = (srcWidth  < r->mWidth)  ? srcWidth  : r->mWidth;
    const uint32_t copyH = (srcHeight < r->mHeight) ? srcHeight : r->mHeight;
    const uint32_t dstStrideBytes = r->mBufWidth * 4u;
    const uint32_t srcStrideBytes = srcWidth     * 4u;

    uint8_t* dst = static_cast<uint8_t*>(r->mPixels);

    if (copyW == r->mWidth && copyW == srcWidth && dstStrideBytes == srcStrideBytes)
    {
        // Fast path — strides match (bufW == srcW), single contiguous copy.
        memcpy(dst, src, copyH * dstStrideBytes);
    }
    else
    {
        // Row-by-row: src has tight srcWidth pitch, dst has padded mBufWidth.
        // Pad the right edge by replicating the last copied pixel — UVs at or
        // below 1.0 won't sample padding anyway, and any value beats stale
        // data from the previous frame leaking through filtering.
        for (uint32_t y = 0; y < copyH; ++y)
        {
            uint8_t*       drow = dst + y * dstStrideBytes;
            const uint8_t* srow = src + y * srcStrideBytes;
            memcpy(drow, srow, copyW * 4u);
            if (copyW < r->mBufWidth)
            {
                const uint32_t lastPx = *reinterpret_cast<const uint32_t*>(drow + (copyW - 1) * 4u);
                uint32_t* dpad = reinterpret_cast<uint32_t*>(drow + copyW * 4u);
                for (uint32_t x = copyW; x < r->mBufWidth; ++x)
                {
                    *dpad++ = lastPx;
                }
            }
        }
    }

    // GE samples textures via DMA — flush CPU dcache for the rows we just
    // touched so the next BindTexture sees fresh pixels, not whatever was
    // cached from the previous frame.
    sceKernelDcacheWritebackRange(dst, copyH * dstStrideBytes);
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
// Skeletal mesh — CPU-skinned, indexed draw
//
// Strategy:
//   - GFX_IsCpuSkinningRequired returns true → engine CPU-skins every frame
//     and hands us a std::vector<Vertex> (engine layout) via
//     GFX_UpdateSkeletalMeshCompVertexBuffer.
//   - Mesh resource keeps a copy of the bind-pose mesh's *indices* (those
//     don't change per frame). The bind-pose VertexSkinned* data is
//     ignored — only the comp's per-frame skinned vertex buffer is drawn.
//   - Component resource holds a dynamic per-frame vertex buffer in PSP
//     HW format (psp::StaticVertex — same layout the static-mesh path uses).
//   - GFX_DrawSkeletalMeshComp binds material+texture+world matrix then
//     sceGuDrawArray with comp's vertex buffer + mesh's indices.
//
// HW skinning via GU_WEIGHTS(n) + sceGuBoneMatrix is possible but limited
// to 8 bones per draw. Real models routinely exceed that; partitioning is
// non-trivial. CPU path works for any bone count and matches what the
// engine's GameCube path does today.
// =============================================================================

void GFX_CreateSkeletalMeshResource(SkeletalMesh* sm,
                                    uint32_t /*numVerts*/,
                                    VertexSkinned* /*verts*/,
                                    uint32_t numIdx,
                                    IndexType* idx)
{
    if (sm == nullptr || idx == nullptr || numIdx == 0) return;
    SkeletalMeshResource* r = sm->GetResource();
    if (r == nullptr) return;

    const uint32_t iBytes = sizeof(IndexType) * numIdx;
    void* iBuf = memalign(16, iBytes);
    if (iBuf == nullptr) return;
    memcpy(iBuf, idx, iBytes);

    r->mIndexData  = iBuf;
    r->mNumIndices = numIdx;

    sceKernelDcacheWritebackRange(iBuf, iBytes);
}

void GFX_DestroySkeletalMeshResource(SkeletalMesh* sm)
{
    if (sm == nullptr) return;
    SkeletalMeshResource* r = sm->GetResource();
    if (r == nullptr) return;
    if (r->mIndexData != nullptr) { free(r->mIndexData); r->mIndexData = nullptr; }
    r->mNumIndices = 0;
}

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

void GFX_CreateSkeletalMeshCompResource(SkeletalMesh3D* /*c*/)
{
    // No-op — the per-frame vertex buffer is allocated lazily on the first
    // GFX_ReallocateSkeletalMeshCompVertexBuffer call (engine knows the
    // skinned-vertex count, we don't until then). The SkeletalMeshCompResource
    // struct's pointers default-init to nullptr, which is the correct
    // "not yet allocated" state for the destroy path.
}

void GFX_DestroySkeletalMeshCompResource(SkeletalMesh3D* c)
{
    if (c == nullptr) return;
    SkeletalMeshCompResource* r = c->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { free(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexCapacity = 0;
    r->mNumVertices    = 0;
    r->mVertexStride   = 0;
}

void GFX_ReallocateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* c, uint32_t numVerts)
{
    if (c == nullptr || numVerts == 0) return;
    SkeletalMeshCompResource* r = c->GetResource();
    if (r == nullptr) return;

    const uint32_t stride = sizeof(psp::StaticVertex);
    const uint32_t needed = stride * numVerts;
    if (r->mVertexCapacity >= needed && r->mVertexData != nullptr)
    {
        // Existing buffer is large enough — just update the high-water count.
        r->mNumVertices  = numVerts;
        r->mVertexStride = stride;
        return;
    }

    if (r->mVertexData != nullptr) { free(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexData     = memalign(16, needed);
    r->mVertexCapacity = (r->mVertexData != nullptr) ? needed : 0;
    r->mNumVertices    = (r->mVertexData != nullptr) ? numVerts : 0;
    r->mVertexStride   = stride;
}

void GFX_UpdateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* c,
                                            const std::vector<Vertex>& skinnedVertices)
{
    if (c == nullptr || skinnedVertices.empty()) return;
    SkeletalMeshCompResource* r = c->GetResource();
    if (r == nullptr) return;

    const uint32_t n = (uint32_t)skinnedVertices.size();
    const uint32_t needed = sizeof(psp::StaticVertex) * n;

    // Defensive grow — the engine calls Reallocate first, but if the skin
    // produces more verts than expected we'd otherwise blow past the buffer.
    if (r->mVertexCapacity < needed || r->mVertexData == nullptr)
    {
        GFX_ReallocateSkeletalMeshCompVertexBuffer(c, n);
        if (r->mVertexData == nullptr) return;
    }

    psp::RepackVertices(skinnedVertices.data(), n,
                        static_cast<psp::StaticVertex*>(r->mVertexData));
    r->mNumVertices = n;

    sceKernelDcacheWritebackRange(r->mVertexData, needed);
}

void GFX_DrawSkeletalMeshComp(SkeletalMesh3D* c)
{
    if (!sGuInitialised || c == nullptr) return;

    SkeletalMesh* mesh = c->GetSkeletalMesh();
    if (mesh == nullptr) return;

    SkeletalMeshResource* mr = mesh->GetResource();
    SkeletalMeshCompResource* cr = c->GetResource();
    if (mr == nullptr || cr == nullptr) return;
    if (mr->mIndexData == nullptr || mr->mNumIndices == 0) return;
    if (cr->mVertexData == nullptr || cr->mNumVertices == 0) return;

    // Same material+texture binding pattern as StaticMesh3D — read material
    // from the comp (fall back to the renderer's default), bind, then bind
    // its base texture.
    Material* matBase = c->GetMaterial();
    MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
    BindMaterial(mat, /*useVertexColor=*/false);
    BindTexture(mat ? mat->GetTexture(0) : nullptr);

    // World matrix from the comp's render transform. View+projection were
    // already set by BeginRenderPass(Forward).
    ScePspFMatrix4 worldMtx;
    const glm::mat4& world = c->GetRenderTransform();
    memcpy(&worldMtx, &world[0][0], sizeof(float) * 16);
    sceGuSetMatrix(GU_MODEL, &worldMtx);

    // psp::kStaticVertexFlags already includes GU_INDEX_16BIT + GU_TRANSFORM_3D
    // plus the tex/normal/pos format flags matching psp::StaticVertex layout.
    sceGuDrawArray(GU_TRIANGLES,
                   psp::kStaticVertexFlags,
                   (int)mr->mNumIndices,
                   mr->mIndexData,
                   cr->mVertexData);
}

bool GFX_IsCpuSkinningRequired(SkeletalMesh3D* /*c*/)
{
    // PSP HW skinning (sceGuBoneMatrix + GU_WEIGHTS) is capped at 8 bones
    // per draw — most real models exceed that and would need per-batch
    // partitioning. CPU skinning is uncapped and matches GameCube/3DS
    // defaults. Optimisation for future: switch this to a per-comp check
    // that returns false when bone count <= 8 and use the HW path then.
    return true;
}

// Simple "blob" shadow: draw the shadow mesh as a flat, unlit, translucent quad
// tinted by the world shadow colour. GX uses a 3-pass alpha-buffer trick to avoid
// double-blending overlapping shadow triangles; PSP draws a single translucent pass,
// which is fine for the flat disc/quad blob meshes used under characters.
void GFX_DrawShadowMeshComp(ShadowMesh3D* c)
{
    if (!sGuInitialised || c == nullptr) return;

    StaticMesh* mesh = c->GetStaticMesh();
    if (mesh == nullptr) return;

    StaticMeshResource* r = mesh->GetResource();
    if (r == nullptr || r->mVertexData == nullptr || r->mIndexData == nullptr) return;
    if (r->mNumIndices == 0) return;

    World* world = Renderer::Get() ? Renderer::Get()->GetCurrentWorld() : nullptr;
    const glm::vec4 shadowColor = world ? world->GetShadowColor()
                                        : glm::vec4(0.0f, 0.0f, 0.0f, 0.5f);

    // Untextured (white 1x1 MODULATE lets sceGuColor through), unlit, flat colour.
    BindTexture(nullptr);
    sceGuDisable(GU_LIGHTING);
    sceGuColor(PackColorAbgr(shadowColor, shadowColor.a));

    // Translucent so it darkens the floor; depth-tested against the scene but no
    // depth writes (a shadow must not occlude anything). Cull disabled so the flat
    // shadow quad shows regardless of its winding relative to the camera.
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_GEQUAL);  // PSP inverted depth (far = 0)
    sceGuDepthMask(GU_TRUE);    // GU_TRUE = depth writes masked OFF

    ScePspFMatrix4 worldMtx;
    const glm::mat4& worldXform = c->GetRenderTransform();
    memcpy(&worldMtx, &worldXform[0][0], sizeof(float) * 16);
    sceGuSetMatrix(GU_MODEL, &worldMtx);

    sceGuDrawArray(GU_TRIANGLES,
                   r->mVertexFlags,
                   (int)r->mNumIndices,
                   r->mIndexData,
                   r->mVertexData);

    // Re-enable depth writes for subsequent opaque draws.
    sceGuDepthMask(GU_FALSE);
}

// PSP has no HW instancing — fixed-function GE has no per-instance vertex
// attribute mechanism, no SSBO, no shader to read a transform array from. So
// we issue one draw per instance with sceGuSetMatrix in between. The mesh
// resource itself (vertex/index buffers) and the material+texture binding
// stay set across the loop — only the world matrix changes per iteration.
//
// Performance: each iteration is one matrix upload + one sceGuDrawArray. For
// modest instance counts (foliage clumps, scatter props in the dozens-to-
// hundreds range) this is fine on PSP. For thousands of instances per draw
// you'd want a display-list batch or HW-skin-style trick — out of scope
// here; cross that bridge when an actual scene needs it.
void GFX_DrawInstancedMeshComp(InstancedMesh3D* comp)
{
    if (!sGuInitialised || comp == nullptr) return;

    StaticMesh* mesh = comp->GetStaticMesh();
    if (mesh == nullptr) return;

    StaticMeshResource* r = mesh->GetResource();
    if (r == nullptr || r->mVertexData == nullptr || r->mIndexData == nullptr) return;
    if (r->mNumIndices == 0) return;

    const uint32_t numInstances = comp->GetNumInstances();
    if (numInstances == 0) return;

    // Bind material+texture ONCE for all instances — they're shared. Same
    // pattern as DrawStaticMeshComp; the only thing that varies per
    // instance is the world matrix.
    Material* matBase = comp->GetMaterial();
    MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
    BindMaterial(mat, /*useVertexColor=*/false);
    BindTexture(mat ? mat->GetTexture(0) : nullptr);

    const glm::mat4& compXform = comp->GetRenderTransform();

    for (uint32_t i = 0; i < numInstances; ++i)
    {
        // CalculateInstanceTransform returns the instance's transform
        // RELATIVE TO THE COMP — combine with the comp's render transform
        // to get the instance's true world matrix. Vulkan does the same
        // multiply in the vertex shader's SSBO read; we do it CPU-side.
        const glm::mat4 worldXform = compXform * comp->CalculateInstanceTransform((int32_t)i);

        ScePspFMatrix4 worldMtx;
        memcpy(&worldMtx, &worldXform[0][0], sizeof(float) * 16);
        sceGuSetMatrix(GU_MODEL, &worldMtx);

        sceGuDrawArray(GU_TRIANGLES,
                       r->mVertexFlags,
                       (int)r->mNumIndices,
                       r->mIndexData,
                       r->mVertexData);
    }
}

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

// =============================================================================
// Particles
//
// Engine simulates particles CPU-side and produces a std::vector<VertexParticle>
// with 4 corner verts per particle (billboard quad). Our job each frame:
//   1. Reallocate per-comp PSP vertex buffer to fit 6 verts/particle (after
//      4→6 triangle expansion).
//   2. Repack engine vertex layout (pos / uv / color) into PSP HW order
//      (tex / color / pos) AND expand each quad to two triangles in the
//      same `(0,1,2)(2,1,3)` order GameCube + Vulkan use.
//   3. Draw with the particle's material — typically unlit + additive or
//      alpha blend; BindMaterial reads those off the material asset.
//
// useLocalSpace toggle: when true, particles are positioned relative to the
// component's transform (moving the emitter drags the particles with it).
// When false (default), particles are spawned in world space (emitter
// position becomes their spawn origin, motion is independent). Matches
// Vulkan / GameCube behaviour.
// =============================================================================

void GFX_CreateParticleCompResource(Particle3D* /*c*/)
{
    // No-op — buffer allocated lazily on first UpdateVertexBuffer call when
    // we know the particle count. ParticleCompResource fields default-init
    // to nullptr / 0 which is the correct "not yet allocated" state.
}

void GFX_DestroyParticleCompResource(Particle3D* c)
{
    if (c == nullptr) return;
    ParticleCompResource* r = c->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { free(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexCapacity = 0;
    r->mNumVertices    = 0;
    r->mVertexStride   = 0;
}

void GFX_UpdateParticleCompVertexBuffer(Particle3D* c, const std::vector<VertexParticle>& vertices)
{
    if (c == nullptr) return;
    ParticleCompResource* r = c->GetResource();
    if (r == nullptr) return;

    const uint32_t numInputVerts = (uint32_t)vertices.size();
    const uint32_t numParticles  = numInputVerts / 4;        // engine emits 4 verts/particle
    const uint32_t numOutVerts   = numParticles * 6;         // expand to triangle list
    if (numParticles == 0)
    {
        r->mNumVertices = 0;
        return;
    }

    const uint32_t stride = sizeof(psp::ParticleVertex);
    const uint32_t needed = stride * numOutVerts;

    // Grow on overflow only — keeps the buffer stable when particle count
    // oscillates frame-to-frame.
    if (r->mVertexCapacity < needed || r->mVertexData == nullptr)
    {
        if (r->mVertexData != nullptr) { free(r->mVertexData); r->mVertexData = nullptr; }
        r->mVertexData     = memalign(16, needed);
        r->mVertexCapacity = (r->mVertexData != nullptr) ? needed : 0;
        if (r->mVertexData == nullptr) { r->mNumVertices = 0; return; }
    }

    psp::RepackParticleVertices(vertices.data(), numParticles,
                                static_cast<psp::ParticleVertex*>(r->mVertexData));
    r->mNumVertices  = numOutVerts;
    r->mVertexStride = stride;

    sceKernelDcacheWritebackRange(r->mVertexData, needed);
}

void GFX_DrawParticleComp(Particle3D* c)
{
    if (!sGuInitialised || c == nullptr) return;
    if (c->GetNumParticles() == 0) return;

    ParticleCompResource* r = c->GetResource();
    if (r == nullptr || r->mVertexData == nullptr || r->mNumVertices == 0) return;

    // Material: read off the comp, fall back to renderer default. Particles
    // modulate the texture by per-vertex colour so useVertexColor=true.
    Material* matBase = c->GetMaterial();
    MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
    BindMaterial(mat, /*useVertexColor=*/true);
    BindTexture(mat ? mat->GetTexture(0) : nullptr);

    // World matrix: identity for world-space particles (they live in world
    // coords already), comp's transform for local-space particles
    // (they're authored in the comp's local frame and follow the emitter).
    // Particle3D doesn't have GetRenderTransform — uses GetTransform from
    // its Node3D base. Same behaviour as GameCube's GFX_DrawParticleComp.
    ScePspFMatrix4 worldMtx;
    if (c->GetUseLocalSpace())
    {
        const glm::mat4 world = c->GetTransform();
        memcpy(&worldMtx, &world[0][0], sizeof(float) * 16);
    }
    else
    {
        const glm::mat4 ident(1.0f);
        memcpy(&worldMtx, &ident[0][0], sizeof(float) * 16);
    }
    sceGuSetMatrix(GU_MODEL, &worldMtx);

    sceGuDrawArray(GU_TRIANGLES,
                   psp::kParticleVertexFlags,
                   (int)r->mNumVertices,
                   nullptr,                  // no index buffer — verts are already expanded
                   r->mVertexData);
}

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

void GFX_DrawQuad(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetResource();
    if (r == nullptr || r->mVertexData == nullptr) return;
    const uint32_t n = quad->GetNumVertices();
    if (n == 0) return;

    // [UIDBG] temp instrumentation — track down the phantom fullscreen green
    // overlay. Strip once identified.
    static int sQuadDbg = 0;
    if (sQuadDbg < 8)
    {
        Texture* tex = quad->GetTexture();
        const VertexUI* verts = quad->GetVertices();
        LogDebug("[UIDBG] DrawQuad name='%s' n=%u tex=%s v0=(%.1f,%.1f) v0.col=0x%08X widgetColor=(%.2f,%.2f,%.2f,%.2f)",
                 quad->GetName().c_str(), n,
                 tex != nullptr ? tex->GetName().c_str() : "<null>",
                 verts ? verts[0].mPosition.x : 0.0f,
                 verts ? verts[0].mPosition.y : 0.0f,
                 verts ? verts[0].mColor : 0u,
                 quad->GetColor().r, quad->GetColor().g, quad->GetColor().b, quad->GetColor().a);
        ++sQuadDbg;
    }

    float rotRad; glm::vec2 pivot;
    GetWidgetRotation(quad, rotRad, pivot);
    void* drawBuf = ApplyUIDrawTransform(r->mVertexData, n, quad->GetColor(), quad->GetTexture(),
                                         glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 0.0f),
                                         rotRad, pivot);
    if (drawBuf == nullptr) return;   // tint alpha = 0

    SetupUIPipeline();
    BindUITexture(quad->GetTexture());

    if (drawBuf == r->mVertexData)
    {
        sceKernelDcacheWritebackRange(r->mVertexData, r->mVertexCapacity);
    }
    sceGuDrawArray(GU_TRIANGLE_FAN, kUIVertexFlags, (int)n, nullptr, drawBuf);
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

void GFX_DrawQuadBorder(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetBorderResource();
    if (r == nullptr || r->mVertexData == nullptr) return;
    const uint32_t n = quad->GetBorderNumVertices();
    if (n == 0) return;

    static int sBorderDbg = 0;
    if (sBorderDbg < 4)
    {
        LogDebug("[UIDBG] DrawQuadBorder name='%s' n=%u borderColor=(%.2f,%.2f,%.2f,%.2f)",
                 quad->GetName().c_str(), n,
                 quad->GetBorderColor().r, quad->GetBorderColor().g,
                 quad->GetBorderColor().b, quad->GetBorderColor().a);
        ++sBorderDbg;
    }

    // Border uses GetBorderColor() as the uniform tint, not GetColor().
    // No texture (white fallback in BindUITexture), so pass nullptr.
    // Border rotates with the parent Quad.
    float rotRad; glm::vec2 pivot;
    GetWidgetRotation(quad, rotRad, pivot);
    void* drawBuf = ApplyUIDrawTransform(r->mVertexData, n, quad->GetBorderColor(), nullptr,
                                         glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 0.0f),
                                         rotRad, pivot);
    if (drawBuf == nullptr) return;

    SetupUIPipeline();
    BindUITexture(nullptr); // solid border fill against the built-in white texel

    if (drawBuf == r->mVertexData)
    {
        sceKernelDcacheWritebackRange(r->mVertexData, r->mVertexCapacity);
    }
    sceGuDrawArray(GU_TRIANGLE_FAN, kUIVertexFlags, (int)n, nullptr, drawBuf);
}

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

void GFX_DrawText(Text* text)
{
    if (text == nullptr) return;
    TextResource* r = text->GetResource();
    if (r == nullptr || r->mVertexData == nullptr) return;

    Font* font = text->GetFont();
    if (font == nullptr) return;
    Texture* fontTex = font->GetTexture();
    if (fontTex == nullptr) return;

    const uint32_t numVisible = text->GetNumVisibleCharacters();
    if (numVisible == 0) return;
    const uint32_t numVerts = numVisible * TEXT_VERTS_PER_CHAR;

    static int sTextDbg = 0;
    if (sTextDbg < 4)
    {
        LogDebug("[UIDBG] DrawText name='%s' text='%s' chars=%u font='%s'",
                 text->GetName().c_str(),
                 text->GetText().c_str(), numVisible,
                 font->GetName().c_str());
        ++sTextDbg;
    }

    // Text vertices are widget-LOCAL and at the font's native point size
    // (cursor starts at 0,0 at fontSize point size). Vulkan/GX apply
    // (rect.x + justifiedOffset, rect.y + justifiedOffset) translation and
    // (scaledTextSize / fontSize) scale via shader/TEV uniform. PSP has
    // neither, so we bake the same transform into the vertex buffer here —
    // otherwise every Text widget renders at top-left at font-native size
    // regardless of its anchor.
    Font* fontAsset = text->GetFont();
    const int32_t fontSize = fontAsset ? fontAsset->GetSize() : 32;
    const float textScale = (fontSize > 0)
        ? (text->GetScaledTextSize() / (float)fontSize) : 1.0f;
    const Rect rect = text->GetRect();
    const glm::vec2 justified = text->GetJustifiedOffset();
    const glm::vec2 posScale(textScale, textScale);
    const glm::vec2 posOffset(rect.mX + justified.x, rect.mY + justified.y);

    float rotRad; glm::vec2 pivot;
    GetWidgetRotation(text, rotRad, pivot);
    void* drawBuf = ApplyUIDrawTransform(r->mVertexData, numVerts,
                                         text->GetColor(), fontTex,
                                         posScale, posOffset,
                                         rotRad, pivot);
    if (drawBuf == nullptr) return;

    SetupUIPipeline();
    BindUITexture(fontTex);

    if (drawBuf == r->mVertexData)
    {
        sceKernelDcacheWritebackRange(r->mVertexData, r->mVertexCapacity);
    }
    sceGuDrawArray(GU_TRIANGLES, kUIVertexFlags, (int)numVerts, nullptr, drawBuf);
}

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

void GFX_DrawPoly(Poly* poly)
{
    if (poly == nullptr) return;
    PolyResource* r = poly->GetResource();
    if (r == nullptr || r->mVertexData == nullptr) return;
    const uint32_t n = poly->GetNumVertices();
    if (n == 0) return;

    static int sPolyDbg = 0;
    if (sPolyDbg < 4)
    {
        LogDebug("[UIDBG] DrawPoly name='%s' n=%u", poly->GetName().c_str(), n);
        ++sPolyDbg;
    }

    // Poly draws as line-strip with the white fallback — no texture sampling.
    float rotRad; glm::vec2 pivot;
    GetWidgetRotation(poly, rotRad, pivot);
    void* drawBuf = ApplyUIDrawTransform(r->mVertexData, n, poly->GetColor(), nullptr,
                                         glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 0.0f),
                                         rotRad, pivot);
    if (drawBuf == nullptr) return;

    SetupUIPipeline();
    BindUITexture(nullptr);  // Poly is a line strip; texture sampling not meaningful

    if (drawBuf == r->mVertexData)
    {
        sceKernelDcacheWritebackRange(r->mVertexData, r->mVertexCapacity);
    }
    // Engine Poly maps to LINE_STRIP topology (matches PipelineConfig::Poly in
    // the Vulkan backend). PSP line thickness is fixed at 1 px — Poly::mLineWidth
    // is silently ignored.
    sceGuDrawArray(GU_LINE_STRIP, kUIVertexFlags, (int)n, nullptr, drawBuf);
}

void GFX_DrawStaticMesh(StaticMesh* /*mesh*/, Material* /*material*/, const glm::mat4& /*transform*/, glm::vec4 /*color*/) {}

void GFX_RenderPostProcessPasses() {}

#endif // POLYPHASE_PLATFORM_ADDON
