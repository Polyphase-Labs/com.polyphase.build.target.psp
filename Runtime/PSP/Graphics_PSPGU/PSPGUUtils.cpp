#if defined(POLYPHASE_PLATFORM_ADDON)

#include "PSPGUUtils.h"

#include "Vertex.h"

namespace psp
{
    void RepackVertices(const Vertex* src, uint32_t count, StaticVertex* dst)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            const Vertex& v = src[i];
            StaticVertex& o = dst[i];
            o.u  = v.mTexcoord0.x;
            o.v  = v.mTexcoord0.y;
            o.nx = v.mNormal.x;
            o.ny = v.mNormal.y;
            o.nz = v.mNormal.z;
            o.x  = v.mPosition.x;
            o.y  = v.mPosition.y;
            o.z  = v.mPosition.z;
        }
    }

    void RepackVerticesColor(const VertexColor* src, uint32_t count, StaticColorVertex* dst)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            const VertexColor& v = src[i];
            StaticColorVertex& o = dst[i];
            o.u     = v.mTexcoord0.x;
            o.v     = v.mTexcoord0.y;
            o.color = EngineRgbaToPspColor(v.mColor);
            o.nx    = v.mNormal.x;
            o.ny    = v.mNormal.y;
            o.nz    = v.mNormal.z;
            o.x     = v.mPosition.x;
            o.y     = v.mPosition.y;
            o.z     = v.mPosition.z;
        }
    }

    void RepackParticleVertices(const VertexParticle* src,
                                uint32_t numParticles,
                                ParticleVertex* dst)
    {
        // Quad → triangle-list expansion. Engine produces 4 corner verts per
        // particle; PSP draws GU_TRIANGLES so we emit 6 verts in the order
        // GameCube / Vulkan use: (0,1,2)(2,1,3). Per-particle colour is
        // already replicated across all 4 input verts by the simulator, so
        // we copy each one as-is.
        auto fill = [&](ParticleVertex& o, const VertexParticle& v) {
            o.u     = v.mTexcoord.x;
            o.v     = v.mTexcoord.y;
            o.color = EngineRgbaToPspColor(v.mColor);
            o.x     = v.mPosition.x;
            o.y     = v.mPosition.y;
            o.z     = v.mPosition.z;
        };

        for (uint32_t p = 0; p < numParticles; ++p)
        {
            const VertexParticle* in  = src + p * 4;
            ParticleVertex*       out = dst + p * 6;
            fill(out[0], in[0]);
            fill(out[1], in[1]);
            fill(out[2], in[2]);
            fill(out[3], in[2]);
            fill(out[4], in[1]);
            fill(out[5], in[3]);
        }
    }
}

#endif // POLYPHASE_PLATFORM_ADDON
