/**
 * @file AudioTypes_Platform.h
 * @brief PSP platform extension for the engine's `AudioTypes.h` fork.
 *
 * Stub — the engine's `AudioTypes.h` only forks on Windows today (for
 * XAudio2 types). PSP audio runs via sceAudio* APIs; no shared types
 * need to leak into the engine layer at present.
 */

#pragma once
#include <stdint.h>
