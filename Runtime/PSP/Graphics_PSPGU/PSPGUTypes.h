/**
 * @file PSPGUTypes.h
 * @brief Vertex layout + GU-context state for the PSPGU renderer.
 *
 * PSP's hardware vertex format is fixed and mandatory-ordered:
 *
 *     [weights] [texcoord] [color] [normal] [position]
 *
 * Fields present in a draw are signalled via the `vtype` mask on
 * sceGuDrawArray. We use two layouts:
 *
 *   PspStaticVertex      — tex / normal / pos    (32 bytes per vertex)
 *   PspStaticColorVertex — tex / color / normal / pos (36 bytes per vertex)
 *
 * Layouts are packed (no implicit alignment); PSP's GE handles unaligned
 * field reads inside the vertex stream.
 */

#pragma once

#if defined(POLYPHASE_PLATFORM_ADDON)

#include <pspgu.h>
#include <stdint.h>

namespace psp
{
    // Static, unskinned, no vertex colour. Engine `Vertex` repacked.
    struct __attribute__((packed)) StaticVertex
    {
        float u, v;
        float nx, ny, nz;
        float x, y, z;
    };
    static_assert(sizeof(StaticVertex) == 32, "PSP static vertex must be 32 bytes");

    constexpr uint32_t kStaticVertexFlags =
        GU_TEXTURE_32BITF | GU_NORMAL_32BITF | GU_VERTEX_32BITF |
        GU_INDEX_16BIT    | GU_TRANSFORM_3D;

    // Static, unskinned, with vertex colour (Voxel3D / Terrain3D / VertexColor mesh).
    struct __attribute__((packed)) StaticColorVertex
    {
        float    u, v;
        uint32_t color;     // RGBA8888 little-endian = ABGR memory order
        float    nx, ny, nz;
        float    x, y, z;
    };
    static_assert(sizeof(StaticColorVertex) == 36, "PSP color vertex must be 36 bytes");

    constexpr uint32_t kStaticColorVertexFlags =
        GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_NORMAL_32BITF | GU_VERTEX_32BITF |
        GU_INDEX_16BIT    | GU_TRANSFORM_3D;

    // Particle billboard vertex — no normal (particles are typically unlit /
    // additive), no index buffer (we expand each quad to 6 verts at repack
    // time and just draw GU_TRIANGLES). Matches engine `VertexParticle`'s
    // 24 bytes, only the field order differs (PSP HW mandates
    // texture → color → position).
    struct __attribute__((packed)) ParticleVertex
    {
        float    u, v;
        uint32_t color;     // RGBA8888 little-endian = ABGR memory order
        float    x, y, z;
    };
    static_assert(sizeof(ParticleVertex) == 24, "PSP particle vertex must be 24 bytes");

    constexpr uint32_t kParticleVertexFlags =
        GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D;
}

#endif // POLYPHASE_PLATFORM_ADDON
