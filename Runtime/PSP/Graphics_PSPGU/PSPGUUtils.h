/**
 * @file PSPGUUtils.h
 * @brief Conversion helpers between engine (glm) and PSP (ScePsp*) types,
 *        plus the vertex-repack routines used by GFX_CreateStaticMeshResource.
 */

#pragma once

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "PSPGUTypes.h"

#include <glm/glm.hpp>
#include <pspgu.h>
#include <pspgum.h>
#include <stdint.h>

struct Vertex;
struct VertexColor;
struct VertexParticle;

namespace psp
{
    // Convert glm::mat4 (column-major) to PSPGU ScePspFMatrix4 (also column-
    // major). One memcpy works because both are 16 floats with identical
    // storage order. Inlined for the hot path.
    inline void GlmToScePspMtx(const glm::mat4& src, ScePspFMatrix4& dst)
    {
        // ScePspFMatrix4 is { ScePspFVector4 x,y,z,w; } each holding {x,y,z,w}.
        // Memory layout is identical to glm::mat4's m[16].
        __builtin_memcpy(&dst, &src[0][0], sizeof(float) * 16);
    }

    // Repack engine `Vertex` (pos / uv0 / uv1 / normal) into PSP static layout.
    // uv1 is dropped — PSP fixed-function uses one texture coord per vertex.
    void RepackVertices(const Vertex* src, uint32_t count, StaticVertex* dst);

    // Repack engine `VertexColor` (pos / uv0 / uv1 / normal / colour) into PSP
    // colour layout. Engine colour is RGBA8888; PSP wants ABGR in the uint32 so
    // when read byte-wise the order is r,g,b,a — that matches GU_COLOR_8888.
    // Engine stores it as RGBA in the uint32 so we swap to little-endian ABGR.
    void RepackVerticesColor(const VertexColor* src, uint32_t count, StaticColorVertex* dst);

    // Repack engine particle billboards into PSP format AND expand each
    // 4-vertex quad to 6 triangle-list vertices, matching the GameCube /
    // Vulkan winding (`0,1,2, 2,1,3` per particle).
    //
    // Input:  `src` has `numParticles * 4` VertexParticle entries.
    // Output: `dst` must hold `numParticles * 6` ParticleVertex entries.
    void RepackParticleVertices(const VertexParticle* src,
                                uint32_t numParticles,
                                ParticleVertex* dst);

    // Convert engine RGBA8 colour (R in low byte) to PSP GU_COLOR_8888 byte
    // order (A in high byte, B in next, G in next, R in low byte). On a
    // little-endian PSP, "RGBA in memory" == "ABGR as uint32 literal".
    inline uint32_t EngineRgbaToPspColor(uint32_t rgba)
    {
        const uint32_t r = (rgba >> 0)  & 0xFF;
        const uint32_t g = (rgba >> 8)  & 0xFF;
        const uint32_t b = (rgba >> 16) & 0xFF;
        const uint32_t a = (rgba >> 24) & 0xFF;
        return (a << 24) | (b << 16) | (g << 8) | r;
    }
}

#endif // POLYPHASE_PLATFORM_ADDON
