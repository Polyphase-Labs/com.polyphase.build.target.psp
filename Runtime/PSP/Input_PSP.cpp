/**
 * @file Input_PSP.cpp
 * @brief Phase-1 stub for PSP input. Engine compiles + boots without touching
 *        real input. Phase 5 replaces this with real sceCtrl polling.
 *
 * The platform-specific INP_* functions are the only hard requirement here —
 * the platform-agnostic ones (INP_IsKeyDown, INP_SetGamepadButton, etc.)
 * are implemented inside Engine/Source/Input/InputManager.cpp using shared
 * state, and don't need a per-platform translation unit.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Input/Input.h"
#include "Log.h"

void INP_Initialize() { LogDebug("Input_PSP: stub init"); }
void INP_Shutdown()   {}
void INP_Update()     {}

void INP_SetCursorPos(int32_t /*x*/, int32_t /*y*/) {}
void INP_ShowCursor(bool /*show*/) {}
void INP_LockCursor(bool /*lock*/) {}
void INP_TrapCursor(bool /*trap*/) {}
void INP_TrapCursorToRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}

const char* INP_ShowSoftKeyboard(bool /*show*/) { return nullptr; }
bool INP_IsSoftKeyboardShown() { return false; }

#endif // POLYPHASE_PLATFORM_ADDON
