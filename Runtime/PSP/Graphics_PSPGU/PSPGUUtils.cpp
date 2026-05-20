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
}

#endif // POLYPHASE_PLATFORM_ADDON
