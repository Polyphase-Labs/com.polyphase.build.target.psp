/**
 * @file InputTypes_Platform.h
 * @brief PSP platform extension for the engine's `InputTypes.h` fork.
 *
 * Pulls in PSPSDK's controller header so engine code that references PSP
 * input types compiles. The Input_PSP.cpp implementation keeps SceCtrlData
 * internal, but exposing the header here means future engine inline blocks
 * touching PSP-specific types (analog dead-zones, etc.) just work.
 */

#pragma once

#include <stdint.h>
#include <pspctrl.h>
