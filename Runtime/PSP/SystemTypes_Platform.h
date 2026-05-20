/**
 * @file SystemTypes_Platform.h
 * @brief PSP platform extension for the engine's `SystemTypes.h` fork.
 *
 * Picked up automatically by the engine when `POLYPHASE_PLATFORM_ADDON=1`
 * is defined at compile time. ActionManager generates a bridge header at
 * `<projectDir>/Generated/PolyphasePlatform_SystemTypes.h` that includes
 * this file via absolute path. The Makefile_PSP puts the Generated/ dir
 * on the include path.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

// PSPSDK pulls in the threading + I/O primitives we typedef below.
#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>

// ----- Threading typedefs --------------------------------------------------
// PSPSDK uses opaque SceUID handles for both threads and synchronisation
// primitives. There's no native mutex — we use a binary semaphore via
// sceKernelCreateSema(1) and treat that as the engine's MutexObject.
// Thread entry functions return `int` (NOT void), so THREAD_RETURN macro
// in the engine resolves to `return 0;` (the default branch — we do not
// set POLYPHASE_PLATFORM_ADDON_VOID_THREAD_RETURN).
typedef SceUID ThreadObject;
typedef SceUID MutexObject;
typedef int    ThreadFuncRet;

// ----- DirEntry injection --------------------------------------------------
// PSP file-iteration uses sceIoDopen (returns SceUID for the dir, NOT a
// posix DIR*) and sceIoDread (yields SceIoDirent). System_PSP.cpp stores
// the directory handle and the last-read dirent inside the engine's
// DirEntry struct via this macro.
#define POLYPHASE_PLATFORM_ADDON_DIRENTRY_MEMBERS \
    SceUID       mDirHandle = -1; \
    SceIoDirent  mLastDirent = {};

// ----- SystemState injection -----------------------------------------------
// PSP-specific window/display state. Most "windowing" concepts don't apply
// (PSP has a fixed 480x272 screen). We track the active display thread, a
// quit flag for the main loop, and the current CPU clock so the engine can
// query it via SYS_GetPlatformTier-style helpers.
#define POLYPHASE_PLATFORM_ADDON_SYSTEMSTATE_MEMBERS \
    bool     mQuitRequested = false; \
    bool     mInBackground  = false; \
    int32_t  mCpuClockMhz   = 333; \
    int32_t  mGpuClockMhz   = 166;
