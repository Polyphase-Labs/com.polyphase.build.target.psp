# Polyphase PSP Build Target

A Polyphase Engine native addon that adds **Sony PSP** (PlayStation Portable) as a
build target. Selecting `PSP (Homebrew)` from the Build & Run menu cooks the
project, cross-compiles via PSPSDK, wraps the binary into an `EBOOT.PBP`, and
launches it in PPSSPP (or copies it to a real PSP via `pspsh`).

## What this addon provides

- **Build pipeline integration** (Variant 2 platform-extension): generated
  `PolyphasePlatform_*.h` bridge headers route engine type forks (`ThreadObject`,
  `MutexObject`, `SocketHandle`, etc.) to PSP-specific definitions without
  modifying engine source.
- **PSPGU rendering backend** (`Runtime/PSP/Graphics_PSPGU/`) — implements the
  engine's `GFX_*` surface against Sony's GU library:
  - Textured static meshes
  - Camera projection + view + per-draw world matrices
  - Per-pass viewport / scissor / pipeline state
- **System runtime** (`Runtime/PSP/System_PSP.cpp`, etc.) — file I/O, threads,
  mutexes, timers, memory stats via `sceIo`, `sceKernel`, `scePower`.
- **`Makefile_PSP`** — wildcard-discovers engine + addon + project sources;
  resolves PSPDEV from env/WSL automatically; links `pspgum_vfpu` + `pspvfpu`.

## Hardware target

The PSP (PSP-1000 / 2000 / 3000) is a fixed-function handheld console with the
following hard limits. Code that exceeds these will fail to render, crash, or
silently produce wrong output.

### CPU

| Spec | Value |
| --- | --- |
| Core | MIPS Allegrex (R4000-compatible, 32-bit, little-endian) |
| Default clock | 222 MHz |
| Max clock | 333 MHz (set by addon at boot via `scePowerSetClockFrequency`) |
| VFPU | 128-bit, 32 vector registers (used by `libpspgum_vfpu` for matrix math) |
| L1 cache | 16 KB I + 16 KB D |
| FPU | Hardware 32-bit IEEE-754 single-precision |
| No FPU 64-bit | `double` falls back to slow software emulation — avoid in hot paths |

### GPU (Graphics Engine, "GE")

| Spec | Value |
| --- | --- |
| Clock | 166 MHz (set at boot) |
| Pipeline | **Fixed-function** — no programmable shaders. Lighting / texturing / blending all configured via `sceGu*` state registers. |
| Max triangles/sec | ~33 M (theoretical), realistic ~2–5 M for textured + lit |
| Vertex transform | T&L done by GE, or by VFPU (manual skinning path), or by CPU |
| Hardware bones (skinning) | Up to **8 per draw** via `GU_WEIGHTS_BITS` (not implemented in this addon yet) |

### Memory

| Region | PSP-1000 | PSP-2000 / 3000 | Notes |
| --- | --- | --- | --- |
| **Main RAM (user)** | **32 MB** | **64 MB** | The engine assumes 32 MB for compatibility. Watch BSS/data + heap. |
| Kernel | 4 MB | 4 MB | Reserved; not accessible to user code |
| **VRAM** | **2 MB** | 2 MB | Used by: front buffer + back buffer + depth buffer + cached textures |
| Memory Stick (MS) | varies | varies | Read/write via `sceIo*` at `ms0:/PSP/GAME/<id>/...` |
| Internal flash | — | 32 MB | Game saves only — not addressable for code |

VRAM allocation in this addon (fixed at boot):

```
0x000000 ─┐
          │  Front framebuffer  512 × 272 × 4 = 0x88000 bytes (557 KB)
0x088000 ─┤
          │  Back framebuffer   512 × 272 × 4 = 0x88000 bytes
0x110000 ─┤
          │  16-bit depth       512 × 272 × 2 = 0x44000 bytes (278 KB)
0x154000 ─┘  (about 1.3 MB remaining for VRAM textures — Phase 6+)
0x200000     (end of VRAM)
```

### Display

| Spec | Value |
| --- | --- |
| Native resolution | **480 × 272** |
| Aspect ratio | 16:9 (1.7647…) |
| Refresh rate | 59.94 Hz |
| Color format | RGBA8888 (this addon) — alternatives 5650 / 5551 / 4444 are smaller |
| Depth buffer | 16-bit |

### Texture limits

| Spec | Value |
| --- | --- |
| Max texture size | **512 × 512 per mip level** (HW limit) |
| Min texture size | 1 × 1 |
| Width / height | **Must be power of 2** (1, 2, 4, …, 512). Non-PoT is **not** supported by the GE. |
| Pitch alignment | Buffer width must be a multiple of **16 pixels** (= 64 bytes at RGBA8). This addon pads to 16-aligned pitch in `GFX_CreateTextureResource`. |
| Phase 2 formats | RGBA8888 linear only. RGB565 / RGBA4444 / RGBA5551 / 8-bit indexed available in HW but not wired yet. |
| Swizzling | Not used in Phase 2. PSP HW prefers 16-byte tile swizzled textures for fastest sampling — Phase 6 will add cook-time swizzle. |
| Mipmaps | Not used in Phase 2. |
| Wrap modes | `REPEAT`, `CLAMP`, `MIRROR` available; addon currently uses `REPEAT`. |
| Filtering | `NEAREST` / `LINEAR` / mipmap variants. Addon uses `LINEAR`. |

### Mesh limits

| Spec | Value |
| --- | --- |
| Max indices per `sceGuDrawArray` | **65535** (16-bit index limit) |
| Max vertices in a buffer | unlimited in RAM, but typically `< 4096` for fluent draw |
| Vertex format (static, this addon) | `GU_TEXTURE_32BITF \| GU_NORMAL_32BITF \| GU_VERTEX_32BITF \| GU_INDEX_16BIT \| GU_TRANSFORM_3D` |
| Vertex stride (static) | 32 bytes — `float u,v; float nx,ny,nz; float x,y,z;` |
| Vertex stride (color) | 36 bytes — adds `uint32_t color` between tex and normal |
| **Mandatory field order** | tex → color → normal → pos (PSP HW fixed) |
| Per-vertex alignment | 4-byte (natural) |

### Audio (stub in Phase 2)

| Spec | Value |
| --- | --- |
| Backend | `sceAudio` |
| Hardware channels | 8 |
| Sample rate | Fixed 44.1 kHz, 16-bit signed stereo PCM |

### Input (stub in Phase 2)

| Spec | Value |
| --- | --- |
| Backend | `sceCtrl` |
| Buttons | × ○ □ △ L R Start Select D-pad |
| Analog stick | 1 (PSP-1000 also has IR for Vita compat, ignored) |
| Touchscreen | None |

### Network (stub in Phase 2)

| Spec | Value |
| --- | --- |
| Backend (planned) | `sceNet` / `sceNetAdhoc` (Adhoc) or `sceNetApctl` (Infrastructure) |
| Status | Stub returning "no network" — HTTP backend reports `Initialize() returned false` |

## Current support status

| Subsystem | State | Notes |
| --- | --- | --- |
| Engine boot + main loop | ✅ | `Main_PSP.cpp` with exit-callback wiring |
| File I/O | ✅ | `sceIo*` |
| Threads, mutexes, timers | ✅ | `sceKernel*` — binary semaphore for mutex |
| Logging | ✅ | `stdout` + `ms0:/PSP/GAME/POLYPHASE/polyphase.log` |
| **Textured static meshes** | ✅ | Phase 2 — 32-byte vertex, RGBA8 linear textures |
| Lit / shaded meshes | ⚠️ Stub | Phase 3 — `sceGuLight` up to 4 lights, vertex lighting |
| Skeletal meshes (skinning) | ⚠️ Stub | Phase 3 — HW 8 bones or CPU skin fallback |
| Translucency / blending | ⚠️ Stub | Phase 3 — wired but vertex-color path needed |
| UI / widgets / text | ⚠️ Stub | Phase 4 — `GU_TRANSFORM_2D` quad pipeline |
| Audio | ⚠️ Stub | Phase 5 — `sceAudio` 8-channel mixing |
| Input | ⚠️ Stub | Phase 5 — `sceCtrlReadBufferPositive` |
| Network | ⚠️ Stub | Deferred — `sceNetAdhoc` requires WLAN handshake |
| Save data | ⚠️ Stub | `ms0:/PSP/SAVEDATA/<id>/` — not wired |
| Cook-time texture swizzle | ⚠️ Stub | Phase 6 — `CookAsset` callback |
| Light bake / path trace | ❌ | Vulkan-only by design |
| Shadow mapping | ❌ | PSP cannot sample depth as texture |
| Post-processing | ❌ | PSP has no spare fillrate |

## Build environment

| Requirement | Notes |
| --- | --- |
| PSPDEV | https://pspdev.github.io — install to `C:\pspdev` (Windows) or `/opt/pspdev` (Linux/macOS) |
| WSL (Windows) | Ubuntu 22.04+; the addon auto-translates Windows paths to `/mnt/c/...` |
| PPSSPP | https://www.ppsspp.org — for emulator launches. **Use Vulkan or D3D11** renderer (OpenGL has known issues with some PSP buffer states). |
| Make | GNU make ≥ 4.0 |

The Makefile resolves PSPDEV in this order: command-line `PSPDEV=...` → env
var → common install paths (`/usr/local/pspdev`, `/opt/pspdev`, `$HOME/pspdev`,
`/mnt/c/pspdev`). Calling `psp-config` is deliberately avoided since
`bash -lc` from the editor doesn't always source `~/.bashrc`.

## Profile options (per Build Profile)

The Build Profile UI for the PSP target exposes:

- **Title** — shown in the PSP's XMB game menu
- **Disc ID** — 9-char `PARAM.SFO` ID (default `POLY00001`)
- **Firmware** — minimum PSP firmware version (default `6.60`)
- **Custom Makefile** — override `Makefile_PSP` path if you have a fork
- **ICON0.PNG** / **PIC1.PNG** — XMB icon (144×80) and background (480×272)
- **WSL Distribution** (Windows only) — defaults to `Ubuntu`
- **PSPDEV path** — overrides auto-detect

## Known quirks (from Phase 2 development)

These are written up in detail in the engine's `.claude/skills/polyphase-buildtarget`
notes; summarising the important ones:

- **`sceGum*` matrix functions corrupt 3D draws** in this PSPSDK build — even when
  called *after* the draw. The PSPGU renderer uses raw `sceGuSetMatrix` /
  `sceGuDrawArray` exclusively. Don't reintroduce Gum.
- **`GU_CLIP_PLANES` must stay enabled.** Disabling it silently drops all 3D
  primitives even though no user clip planes are defined.
- **Viewport / scissor are hardcoded to 480×272** ignoring engine input —
  `Renderer` passes the editor's scene-tab viewport which can be narrower.
- **`Makefile_PSP` does not generate `.d` files** — header changes don't
  invalidate stale `.o` files. After editing any engine header that affects
  PSP-relevant structs, manually `rm -f *.o` in the project root before
  rebuilding, or you get ABI-skew crashes inside `Factory_*::Create`.
- **No GL→PSP NDC fudge needed.** `glm::perspective` and `ScePspFMatrix4`
  layout match — direct `memcpy` works.

## Files of interest

| Path | Purpose |
| --- | --- |
| `Source/ComPolyphaseBuildTargetPsp.cpp` | The editor-side addon DLL: build target descriptor + `GetCompileCommand` + `PostPackage` + `RunInEmulator` callbacks |
| `Runtime/PSP/Main_PSP.cpp` | PSP entry point (`PSP_MODULE_INFO`, exit callback, calls `GameMain`) |
| `Runtime/PSP/System_PSP.cpp` | `SYS_*` implementations |
| `Runtime/PSP/Graphics_PSPGU/Graphics_PSPGU.cpp` | `GFX_*` implementations |
| `Runtime/PSP/Graphics_PSPGU/PSPGUTypes.h` | Vertex struct layouts + flag constants |
| `Runtime/PSP/Graphics_PSPGU/PSPGUUtils.{h,cpp}` | glm ↔ PSP type conversion + vertex repack |
| `Runtime/PSP/SystemTypes_Platform.h` | PSP typedefs surfaced through Variant 2 bridge |
| `Runtime/PSP/InputTypes_Platform.h` | PSP input types (stub) |
| `Runtime/PSP/AudioTypes_Platform.h` | Empty — internal to `Audio_PSP.cpp` |
| `Runtime/PSP/NetworkTypes_Platform.h` | `SocketHandle = int32_t` |

## References

- [PSPDEV — Basic Programs](https://pspdev.github.io/basic_programs.html)
- [pspsdk source](https://github.com/pspdev/pspsdk)
- [PPSSPP](https://www.ppsspp.org)
