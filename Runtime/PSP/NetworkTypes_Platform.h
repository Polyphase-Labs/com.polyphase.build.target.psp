/**
 * @file NetworkTypes_Platform.h
 * @brief PSP platform extension for the engine's `NetworkTypes.h` fork.
 *
 * Stub. The real PSP port can opt into `sceNetAdhoc` / `sceNetInet` and
 * add proper socket headers; for now PSP networking is treated as "not
 * present" by mapping SocketHandle to int32 (matching all the other
 * Unix-like platforms).
 */

#pragma once
#include <stdint.h>

typedef int32_t SocketHandle;
