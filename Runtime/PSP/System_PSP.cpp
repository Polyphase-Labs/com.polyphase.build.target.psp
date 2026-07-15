/**
 * @file System_PSP.cpp
 * @brief PSP-side implementation of the engine's SYS_* surface.
 *
 * Mirrors the shape of Engine/Source/System/3DS/System_3DS.cpp — both target
 * a fixed-screen handheld with a Unix-like SDK and small RAM budget. PSP
 * specifics:
 *
 *   - File I/O is sceIo* (no POSIX <stdio.h> file paths — must be ms0:/...
 *     or disc0:/... etc. on real hardware).
 *   - Threads are SceUID handles via sceKernelCreateThread.
 *   - "Mutexes" are binary semaphores (sceKernelCreateSema(1)).
 *   - Time is sceKernelGetSystemTimeWide (µs).
 *   - No window concepts (480×272 fixed); window/clipboard/dialog SYS_ are
 *     no-op stubs.
 *
 * Built only when POLYPHASE_PLATFORM_ADDON is defined (the addon-routed
 * build path). Engine source compiled for built-in targets never sees this
 * translation unit.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "System/System.h"
#include "Engine.h"
#include "Stream.h"
#include "Log.h"
#include "Utilities.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <psprtc.h>
#include <psppower.h>
#include <pspge.h>

#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/time.h>
#include <sys/stat.h>

// PSP has no <unistd.h>-style chdir; sceIo handles file paths directly.

// Folder under ms0:/PSP/GAME/ that holds this build's EBOOT.PBP and assets.
// Normally injected by Makefile_PSP from the editor's PSP_GAME_DIR=<project>
// make-line variable (which itself comes from ctx->projectName), so each
// project installs to its own slot. The literal POLYPHASE fallback only
// fires if someone runs `make -f Makefile_PSP` by hand without passing
// PSP_GAME_DIR — kept so manual builds still produce something workable.
#ifndef POLYPHASE_PSP_GAME_DIR
#define POLYPHASE_PSP_GAME_DIR "POLYPHASE"
#endif

// Static state — minimal. The engine's EngineState owns most lifecycle data;
// this layer only needs a few flags and the dir-iteration thunk state.
static bool sInitialized = false;

// =========================================================================
// Lifecycle
// =========================================================================

void SYS_Initialize()
{
    if (sInitialized) return;

    // Default clocks. Polyphase's 3DS path tunes clocks for Old vs New 3DS;
    // PSP has a single PSP-1000 tier — 333/166 is the standard homebrew
    // setting. Users with battery-life-sensitive titles can reduce later.
    scePowerSetClockFrequency(333, 333, 166);

    // PSP's pspDebugScreen is a tty-style overlay used for the boot phase
    // (before the GU renderer takes over). Stays addressable post-render
    // for log spillage to the screen on fatal errors.
    pspDebugScreenInit();

    sInitialized = true;
    LogDebug("System_PSP: initialised (CPU 333MHz / GPU 166MHz)");
}

void SYS_Shutdown()
{
    // The PSP exit pathway is callback-driven (sceKernelExitGame fires from
    // the home-button callback we register in Main_PSP.cpp). Nothing to do
    // here — sceGuTerm is in Graphics_PSPGU::GFX_Shutdown.
    sInitialized = false;
}

void SYS_Update()
{
    // Drain HID-style polling so home/menu/power callbacks have a chance to
    // run. The engine ticks update every frame; this is the cheap bridge
    // back into PSP's callback model.
    sceKernelCheckCallback();
}

// =========================================================================
// Paths
// =========================================================================

std::string SYS_GetExecutablePath()
{
    // PSP has no concept of "executable path" beyond the EBOOT.PBP location.
    // The official launcher passes the path as argv[0] to main; we don't have
    // access here. Return the conventional XMB location.
    return "ms0:/PSP/GAME/" POLYPHASE_PSP_GAME_DIR "/EBOOT.PBP";
}

std::string SYS_GetPolyphasePath()
{
    // Resolve the asset root once and cache it. Two packaging layouts:
    //   - EBOOT.PBP on a memory stick: assets sit alongside the EBOOT under
    //     ms0:/PSP/GAME/<PSP_GAME_DIR>/ (the historical layout — folder name
    //     is the project name, set by the editor at compile time).
    //   - Bootable UMD ISO ("Build to ISO"): the executable lives at
    //     disc0:/PSP_GAME/SYSDIR/EBOOT.BIN and the assets are at the disc root,
    //     so when that file exists we resolve assets against disc0:/.
    // Save data and the boot log keep their own ms0: paths (a disc is read-only).
    static std::string sBase;
    if (sBase.empty())
    {
        SceIoStat st;
        // ISO build: the executable and all assets live together in
        // disc0:/PSP_GAME/SYSDIR/ (that folder is also the runtime cwd, so the
        // engine's cwd-relative asset loads resolve there too). Memory-stick
        // PBP builds keep the historical ms0: location.
        if (sceIoGetstat("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN", &st) >= 0)
            sBase = "disc0:/PSP_GAME/SYSDIR/";
        else
            sBase = "ms0:/PSP/GAME/" POLYPHASE_PSP_GAME_DIR "/";
    }
    return sBase;
}

std::string SYS_GetCurrentDirectoryPath()
{
    char buf[256] = {0};
    if (sceIoGetstat("./", nullptr) >= 0)
    {
        // PSP supports relative paths against the EBOOT dir; just return ".".
        return "./";
    }
    return "ms0:/PSP/GAME/" POLYPHASE_PSP_GAME_DIR "/";
}

std::string SYS_GetAbsolutePath(const std::string& relativePath)
{
    // No CWD-relative resolution on PSP — paths are already absolute (ms0:/, disc0:/).
    if (relativePath.size() >= 4 && relativePath.compare(0, 4, "ms0:") == 0) return relativePath;
    if (relativePath.size() >= 6 && relativePath.compare(0, 6, "disc0:") == 0) return relativePath;
    return SYS_GetPolyphasePath() + relativePath;
}

void SYS_ExplorerOpenDirectory(const std::string& /*dirPath*/) {}
void SYS_OpenFileWithDefaultApp(const std::string& /*filePath*/) {}
void SYS_SetWorkingDirectory(const std::string& /*dirPath*/) {}

// =========================================================================
// File I/O
// =========================================================================

bool SYS_DoesFileExist(const char* path, bool /*isAsset*/)
{
    if (path == nullptr) return false;
    SceIoStat stat;
    return sceIoGetstat(path, &stat) >= 0;
}

void SYS_AcquireFileData(const char* path, bool isAsset, int32_t maxSize,
                         char*& outData, uint32_t& outSize)
{
    outData = nullptr;
    outSize = 0;
    if (path == nullptr) return;

    // Embedded asset lookup is engine-side via SYS_LookupEmbeddedRawAsset (not
    // exposed in our header set; engine's loader handles that path). When we
    // get here, treat the request as a real on-disk read.
    (void)isAsset;

    const SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
    {
        LogWarning("SYS_AcquireFileData: sceIoOpen failed for '%s' (rc=0x%08X)", path, (unsigned)fd);
        return;
    }

    const SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);

    uint32_t actualSize = (uint32_t)size;
    if (maxSize > 0 && actualSize > (uint32_t)maxSize) actualSize = (uint32_t)maxSize;

    outData = (char*)malloc(actualSize);
    if (outData == nullptr)
    {
        sceIoClose(fd);
        LogError("SYS_AcquireFileData: malloc(%u) failed for '%s'", actualSize, path);
        return;
    }

    const int read = sceIoRead(fd, outData, actualSize);
    sceIoClose(fd);

    if (read < 0)
    {
        free(outData);
        outData = nullptr;
        LogError("SYS_AcquireFileData: sceIoRead failed for '%s' (rc=0x%08X)", path, (unsigned)read);
        return;
    }
    outSize = (uint32_t)read;
}

void SYS_ReleaseFileData(char* data)
{
    free(data);
}

bool SYS_CreateDirectory(const char* dirPath)
{
    if (dirPath == nullptr) return false;
    return sceIoMkdir(dirPath, 0777) >= 0;
}

void SYS_RemoveDirectory(const char* dirPath)
{
    if (dirPath == nullptr) return;
    sceIoRmdir(dirPath);
}

void SYS_OpenDirectory(const std::string& dirPath, DirEntry& outDirEntry)
{
    outDirEntry.mValid = false;
    outDirEntry.mDirHandle = sceIoDopen(dirPath.c_str());
    if (outDirEntry.mDirHandle < 0) return;

    strncpy(outDirEntry.mDirectoryPath, dirPath.c_str(), MAX_PATH_SIZE);
    outDirEntry.mDirectoryPath[MAX_PATH_SIZE] = '\0';
    outDirEntry.mValid = true;

    // Prime the first entry so callers get a stable invariant: after Open,
    // mFilename either holds the first valid name or mValid is false.
    SYS_IterateDirectory(outDirEntry);
}

void SYS_IterateDirectory(DirEntry& dirEntry)
{
    if (!dirEntry.mValid || dirEntry.mDirHandle < 0)
    {
        dirEntry.mValid = false;
        return;
    }

    const int rc = sceIoDread(dirEntry.mDirHandle, &dirEntry.mLastDirent);
    if (rc <= 0)
    {
        // Either error (<0) or end-of-directory (0). Either way, done.
        dirEntry.mValid = false;
        return;
    }

    strncpy(dirEntry.mFilename, dirEntry.mLastDirent.d_name, MAX_PATH_SIZE);
    dirEntry.mFilename[MAX_PATH_SIZE] = '\0';
    dirEntry.mDirectory = FIO_S_ISDIR(dirEntry.mLastDirent.d_stat.st_mode);
}

void SYS_CloseDirectory(DirEntry& dirEntry)
{
    if (dirEntry.mDirHandle >= 0)
    {
        sceIoDclose(dirEntry.mDirHandle);
        dirEntry.mDirHandle = -1;
    }
    dirEntry.mValid = false;
}

// Returns true if the file was fully copied. Matches the engine's SYS_CopyFile
// contract (System.h) so packaging can detect a failed copy instead of silently
// shipping a broken build.
bool SYS_CopyFile(const char* sourcePath, const char* destPath)
{
    if (sourcePath == nullptr || destPath == nullptr) return false;

    const SceUID src = sceIoOpen(sourcePath, PSP_O_RDONLY, 0);
    if (src < 0) return false;

    const SceUID dst = sceIoOpen(destPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (dst < 0) { sceIoClose(src); return false; }

    bool copyOk = true;
    char buf[4096];
    int read = 0;
    while ((read = sceIoRead(src, buf, sizeof(buf))) > 0)
    {
        if (sceIoWrite(dst, buf, read) != read) { copyOk = false; break; }
    }
    if (read < 0) copyOk = false;   // read error mid-copy

    sceIoClose(src);
    sceIoClose(dst);
    return copyOk;
}

void SYS_CopyDirectory(const char* /*sourceDir*/, const char* /*destDir*/) {}
bool SYS_CopyDirectoryRecursive(const std::string& /*sourceDir*/, const std::string& /*destDir*/) { return false; }
void SYS_MoveDirectory(const char* sourceDir, const char* destDir)
{
    if (sourceDir && destDir) sceIoRename(sourceDir, destDir);
}
void SYS_MoveFile(const char* sourcePath, const char* destPath)
{
    if (sourcePath && destPath) sceIoRename(sourcePath, destPath);
}
void SYS_RemoveFile(const char* path)
{
    if (path) sceIoRemove(path);
}
bool SYS_Rename(const char* oldPath, const char* newPath)
{
    if (oldPath == nullptr || newPath == nullptr) return false;
    return sceIoRename(oldPath, newPath) >= 0;
}

std::vector<std::string> SYS_OpenFileDialog() { return {}; }
std::string SYS_SaveFileDialog() { return ""; }
std::string SYS_SelectFolderDialog() { return ""; }

std::string SYS_GetFileName(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

// =========================================================================
// Threading
// =========================================================================

namespace
{
    // PSP's sceKernelCreateThread takes a C entry returning `int` and accepts
    // a single `argp` buffer (not a void*). We adapt the engine's
    // ThreadFuncFP signature (ThreadFuncRet(*)(void*)) by stashing the
    // function pointer + arg in a heap struct, then trampolining through this
    // entry.
    struct PspThreadShim
    {
        ThreadFuncFP mFunc;
        void*        mArg;
    };

    int PspThreadEntry(SceSize argSize, void* argp)
    {
        if (argSize < sizeof(PspThreadShim*) || argp == nullptr) return 0;
        PspThreadShim* shim = *(PspThreadShim**)argp;
        if (shim == nullptr || shim->mFunc == nullptr) { delete shim; return 0; }
        const ThreadFuncRet rc = shim->mFunc(shim->mArg);
        delete shim;
        return (int)rc;
    }
}

ThreadObject* SYS_CreateThread(ThreadFuncFP func, void* arg)
{
    if (func == nullptr) return nullptr;

    PspThreadShim* shim = new PspThreadShim{ func, arg };

    // Standard homebrew thread settings: priority 0x18 (mid-user), 16 KB
    // stack, attr USER. Matches what most PSP homebrew uses; the engine's
    // worker threads (asset loader, audio mixer) don't need anything fancier.
    const SceUID th = sceKernelCreateThread("polyphase_thread", &PspThreadEntry,
                                            0x18, 0x4000, PSP_THREAD_ATTR_USER, nullptr);
    if (th < 0)
    {
        delete shim;
        LogError("SYS_CreateThread: sceKernelCreateThread failed (rc=0x%08X)", (unsigned)th);
        return nullptr;
    }

    // The shim pointer is passed by-pointer through argp so the entry can
    // dereference + free it.
    PspThreadShim* shimSlot = shim;
    if (sceKernelStartThread(th, sizeof(PspThreadShim*), &shimSlot) < 0)
    {
        sceKernelDeleteThread(th);
        delete shim;
        return nullptr;
    }

    ThreadObject* out = new ThreadObject;
    *out = th;
    return out;
}

void SYS_JoinThread(ThreadObject* thread)
{
    if (thread == nullptr) return;
    sceKernelWaitThreadEnd(*thread, nullptr);
}

void SYS_DestroyThread(ThreadObject* thread)
{
    if (thread == nullptr) return;
    if (*thread >= 0) sceKernelDeleteThread(*thread);
    delete thread;
}

MutexObject* SYS_CreateMutex()
{
    const SceUID sema = sceKernelCreateSema("polyphase_mtx", 0, 1, 1, nullptr);
    if (sema < 0)
    {
        LogError("SYS_CreateMutex: sceKernelCreateSema failed (rc=0x%08X)", (unsigned)sema);
        return nullptr;
    }
    MutexObject* out = new MutexObject;
    *out = sema;
    return out;
}

void SYS_LockMutex(MutexObject* mutex)
{
    if (mutex == nullptr || *mutex < 0) return;
    sceKernelWaitSema(*mutex, 1, nullptr);
}

void SYS_UnlockMutex(MutexObject* mutex)
{
    if (mutex == nullptr || *mutex < 0) return;
    sceKernelSignalSema(*mutex, 1);
}

void SYS_DestroyMutex(MutexObject* mutex)
{
    if (mutex == nullptr) return;
    if (*mutex >= 0) sceKernelDeleteSema(*mutex);
    delete mutex;
}

void SYS_Sleep(uint32_t milliseconds)
{
    sceKernelDelayThread(milliseconds * 1000);
}

// =========================================================================
// Time
// =========================================================================

uint64_t SYS_GetTimeMicroseconds()
{
    SceKernelSysClock clock;
    sceKernelGetSystemTime(&clock);
    return ((uint64_t)clock.hi << 32) | clock.low;
}

// =========================================================================
// Process exec — not applicable on PSP. SYS_Exec is the per-platform
// implementation; SystemUtils.cpp provides ExecCommon and SYS_ExecFull
// (both stubbed under #if defined(POLYPHASE_PLATFORM_ADDON) so they don't
// reach for popen/fork/WIFEXITED). Just supply SYS_Exec here as a no-op.
// =========================================================================

void SYS_Exec(const char* /*cmd*/, std::string* output)
{
    if (output) output->clear();
}

// =========================================================================
// Memory
// =========================================================================

void* SYS_AlignedMalloc(uint32_t size, uint32_t alignment)
{
    return memalign(alignment, size);
}

void SYS_AlignedFree(void* pointer)
{
    free(pointer);
}

std::vector<MemoryStat> SYS_GetMemoryStats()
{
    std::vector<MemoryStat> stats;

    MemoryStat mainRam;
    mainRam.mName = "MainRAM";
    mainRam.mBytesFree = sceKernelTotalFreeMemSize();
    // PSP-1000 user RAM is 24MB; PSP-2000+ is 32MB. We can't trivially detect
    // model from userland — assume worst case for the budget.
    mainRam.mBytesAllocated = (24u * 1024u * 1024u) - mainRam.mBytesFree;
    stats.push_back(mainRam);

    MemoryStat vram;
    vram.mName = "VRAM";
    // sceGeEdramGetSize gives the GE memory ceiling; the renderer's allocator
    // tracks how much it has consumed. Phase 1 has no allocator yet, so we
    // report 0 used.
    vram.mBytesAllocated = 0;
    vram.mBytesFree = sceGeEdramGetSize();
    stats.push_back(vram);

    return stats;
}

float SYS_GetRAMUsage()    { auto s = SYS_GetMemoryStats(); return s.empty() ? 0.0f : (float)s[0].mBytesAllocated; }
float SYS_GetVRAMUsage()   { auto s = SYS_GetMemoryStats(); return s.size() < 2 ? 0.0f : (float)s[1].mBytesAllocated; }
float SYS_GetRAM1Usage()   { return SYS_GetRAMUsage(); }
float SYS_GetRAM2Usage()   { return 0.0f; }
float SYS_GetCPUUsage()    { return 0.0f; }
float SYS_GetTotalRAM()    { return (float)(24u * 1024u * 1024u); }
float SYS_GetTotalVRAM()   { return (float)sceGeEdramGetSize(); }
float SYS_GetTotalRAM1()   { return SYS_GetTotalRAM(); }
float SYS_GetTotalRAM2()   { return 0.0f; }

// =========================================================================
// Save data — files under ms0:/PSP/SAVEDATA/<GAMEID>/<saveName>.
//
// The <GAMEID> path component must match `psp.discId` in the build profile
// (defaults to POLY00001, baked into PARAM.SFO via mksfoex at PostPackage
// time). If you change the disc ID in the profile, change `kSaveGameId`
// below to match — otherwise saves go to a different folder than the one
// the firmware's save-data utility shows for your game.
//
// We skip the official per-save subdir + PARAM.SFO + ICON0.PNG layout that
// the XMB's "Saved Data Utility" expects — that integration needs sceUtilitySavedataInitStart
// and a dialog UI flow. For an engine doing free-form `SYS_WriteSave("foo.sav")`
// the simpler flat layout under one shared folder works on every PSP and
// PPSSPP without dialog interruption. Games that want true XMB integration
// can call sceUtility* themselves.
//
// File I/O goes through stdio (Stream::ReadFile/WriteFile use fopen) —
// PSPSDK's newlib maps "ms0:/..." paths transparently to the PSP IO API, so
// the same code that runs on Linux works here.
// =========================================================================

namespace
{
    constexpr const char* kSaveGameId = "POLY00001";

    inline std::string SavePath(const char* saveName)
    {
        std::string p = "ms0:/PSP/SAVEDATA/";
        p += kSaveGameId;
        p += "/";
        p += saveName;
        return p;
    }

    inline void EnsureSaveDirExists()
    {
        // sceIoMkdir is single-level only — walk the tree. EEXIST returns a
        // negative rc but is harmless; the only failure that matters is the
        // final mkdir, and the subsequent fopen will error visibly anyway.
        sceIoMkdir("ms0:/PSP", 0777);
        sceIoMkdir("ms0:/PSP/SAVEDATA", 0777);
        std::string base = "ms0:/PSP/SAVEDATA/";
        base += kSaveGameId;
        sceIoMkdir(base.c_str(), 0777);
    }
}

bool SYS_ReadSave(const char* saveName, Stream& outStream)
{
    if (saveName == nullptr) return false;
    if (!SYS_DoesSaveExist(saveName))
    {
        LogWarning("SYS_ReadSave: '%s' does not exist", saveName);
        return false;
    }
    const std::string path = SavePath(saveName);
    outStream.ReadFile(path.c_str(), /*isAsset=*/false);
    return outStream.GetSize() > 0;
}

bool SYS_WriteSave(const char* saveName, Stream& stream)
{
    if (saveName == nullptr) return false;
    EnsureSaveDirExists();
    const std::string path = SavePath(saveName);
    const bool ok = stream.WriteFile(path.c_str());
    if (ok)
    {
        LogDebug("Save written: %s (%u bytes) -> %s",
                 saveName, (unsigned)stream.GetSize(), path.c_str());
    }
    else
    {
        LogError("SYS_WriteSave: failed to write '%s'", path.c_str());
    }
    return ok;
}

bool SYS_DoesSaveExist(const char* saveName)
{
    if (saveName == nullptr) return false;
    const SceUID fd = sceIoOpen(SavePath(saveName).c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    sceIoClose(fd);
    return true;
}

bool SYS_DeleteSave(const char* saveName)
{
    if (saveName == nullptr) return false;
    const int rc = sceIoRemove(SavePath(saveName).c_str());
    return rc >= 0;
}

void SYS_UnmountMemoryCard()
{
    // PSP firmware auto-manages the memory stick lifecycle; nothing to do.
    // (Unmount-on-eject would need sceMsCmRegisterMSInsertEjectCallback +
    // a dedicated thread — out of scope; the typical homebrew flow just
    // refuses to launch if the stick is missing.)
}

// =========================================================================
// Clipboard — N/A
// =========================================================================

void SYS_SetClipboardText(const std::string& /*str*/) {}
std::string SYS_GetClipboardText() { return ""; }

// =========================================================================
// Logging / assertions / dialogs
// =========================================================================

// File-backed log path. On PPSSPP this lands under the memstick directory
// (typically `<User>/Documents/PPSSPP/memstick/PSP/GAME/<PSP_GAME_DIR>/polyphase.log`
// on Windows hosts) — readable from the host filesystem while the game runs.
// On real PSP hardware it lives on the physical memory stick, same path.
//
// We deliberately don't keep a long-lived SceUID open: sceIo file handles
// don't survive across thread context switches well, and Polyphase's
// LogDebug calls happen from many threads (asset loader, audio, main).
// Per-call open/append/close is slower but bulletproof.
static const char* sLogFilePath = "ms0:/PSP/GAME/" POLYPHASE_PSP_GAME_DIR "/polyphase.log";

// Ensure the dir tree leading to sLogFilePath exists. sceIoMkdir is single-
// level only, so we walk the path and call it once per segment. EEXIST is
// silently tolerated (return is < 0 in that case, but the file open still
// succeeds afterwards).
//
// Called once per boot — sub-microsecond cost. Idempotent.
static void Psp_EnsureLogDir()
{
    sceIoMkdir("ms0:/PSP", 0777);
    sceIoMkdir("ms0:/PSP/GAME", 0777);
    sceIoMkdir("ms0:/PSP/GAME/" POLYPHASE_PSP_GAME_DIR, 0777);
}

void Psp_AppendLogLineRaw(const char* line)
{
    if (line == nullptr) return;

    // Always go to stdout as well (PPSSPP's HLE log captures this — visible
    // in PPSSPP's Debug → Log Output if the user enables the HLE category).
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);

    // Cheap idempotent dir-tree creation. Without this the very first
    // boot from a freshly-launched EBOOT.PBP fails because PPSSPP launched
    // the binary directly from the project's Packaged/ dir without ever
    // installing it to ms0:/PSP/GAME/, so the parent tree is empty.
    Psp_EnsureLogDir();

    const SceUID fd = sceIoOpen(sLogFilePath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, line, strlen(line));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
}

void SYS_Log(LogSeverity severity, const char* format, va_list arg)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, arg);

    const char* sevTag = (severity == LogSeverity::Error)   ? "[E] "
                       : (severity == LogSeverity::Warning) ? "[W] "
                       :                                       "[D] ";

    char line[1100];
    std::snprintf(line, sizeof(line), "%s%s", sevTag, buf);
    Psp_AppendLogLineRaw(line);
}

void SYS_Assert(const char* exprString, const char* fileString, uint32_t lineNumber)
{
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenPrintf("ASSERT: %s\n  %s:%u\n", exprString, fileString, (unsigned)lineNumber);
    printf("ASSERT: %s at %s:%u\n", exprString, fileString, (unsigned)lineNumber);
    fflush(stdout);
    sceKernelExitGame();
}

void SYS_Alert(const char* message)
{
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenPrintf("ALERT: %s\n", message);
    printf("ALERT: %s\n", message);
    fflush(stdout);
}

void SYS_UpdateConsole() {}

int32_t SYS_GetPlatformTier()
{
    // Single PSP tier. Future: detect PSP-1000 vs 2000+ via sceKernelDevkitVersion.
    return 0;
}

// =========================================================================
// Window — all no-ops on PSP (fixed 480×272 screen).
// =========================================================================

void SYS_SetWindowTitle(const char* /*title*/) {}
void SYS_SetWindowIcon(const char* /*iconPath*/) {}
bool SYS_DoesWindowHaveFocus() { return true; }
void SYS_SetScreenOrientation(ScreenOrientation /*orientation*/) {}
ScreenOrientation SYS_GetScreenOrientation() { return ScreenOrientation::Landscape; }
void SYS_SetFullscreen(bool /*fullscreen*/) {}
bool SYS_IsFullscreen() { return true; }
void SYS_SetWindowRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}
void SYS_GetWindowRect(int32_t& outX, int32_t& outY, int32_t& outWidth, int32_t& outHeight)
{
    outX = 0;
    outY = 0;
    outWidth  = 480;
    outHeight = 272;
}
bool SYS_IsWindowMaximized() { return true; }
void SYS_MaximizeWindow() {}

#endif // POLYPHASE_PLATFORM_ADDON
