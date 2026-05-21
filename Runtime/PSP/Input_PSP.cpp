/**
 * @file Input_PSP.cpp
 * @brief PSP controller input via sceCtrl. Maps the PSP's one gamepad
 *        (single analog stick, face/shoulder/d-pad/system buttons) into the
 *        engine's standard GamepadState slot 0.
 *
 * Face-button mapping uses Xbox-position convention to match engine intent
 * for GAMEPAD_A/B/X/Y (A = bottom of diamond, B = right, X = left, Y = top):
 *   CROSS    -> GAMEPAD_A   (bottom of PSP face diamond)
 *   CIRCLE   -> GAMEPAD_B   (right)
 *   SQUARE   -> GAMEPAD_X   (left)
 *   TRIANGLE -> GAMEPAD_Y   (top)
 *
 * This means CROSS is "confirm" and CIRCLE is "cancel" -- the Western PSP
 * convention. JP/PSN-Japan games typically swap these, but that's a game-side
 * preference toggle, not a platform mapping concern.
 *
 * PSP has no right stick and no analog triggers; L/R are digital buttons. We
 * still populate the corresponding engine axes so script code that reads
 * `GAMEPAD_AXIS_LTRIGGER` etc. gets sensible 0/1 values from the L/R buttons.
 *
 * sceCtrlPeekBufferPositive (non-blocking) is used instead of
 * sceCtrlReadBufferPositive (blocks until next VBlank) — the engine already
 * VSyncs in the render loop and a second blocking call here would halve the
 * effective input rate to 30 Hz.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Input/Input.h"
#include "Input/InputUtils.h"
#include "Engine.h"
#include "Log.h"
#include "Maths.h"

#include <pspctrl.h>

void INP_Initialize()
{
    // Cycle=0 means sampling is driven by VBlank; ANALOG mode populates Lx/Ly.
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    InputState& input = GetEngineState()->mInput;
    input.mGamepads[0].mType = GamepadType::Standard;
    input.mGamepads[0].mConnected = true;
    input.mNumControllers = 1;

    InputInit();
    LogDebug("Input_PSP: sceCtrl analog mode initialised");
}

void INP_Shutdown()
{
    InputShutdown();
}

void INP_Update()
{
    InputAdvanceFrame();

    InputState& input = GetEngineState()->mInput;
    GamepadState& gp = input.mGamepads[0];

    SceCtrlData pad;
    const int read = sceCtrlPeekBufferPositive(&pad, 1);
    gp.mConnected = (read > 0);

    if (read > 0)
    {
        const unsigned int b = pad.Buttons;

        // Face buttons (Xbox-position mapping — see file comment)
        gp.mButtons[GAMEPAD_A] = (b & PSP_CTRL_CROSS)    ? 1 : 0;
        gp.mButtons[GAMEPAD_B] = (b & PSP_CTRL_CIRCLE)   ? 1 : 0;
        gp.mButtons[GAMEPAD_X] = (b & PSP_CTRL_SQUARE)   ? 1 : 0;
        gp.mButtons[GAMEPAD_Y] = (b & PSP_CTRL_TRIANGLE) ? 1 : 0;

        // Shoulders (digital only on PSP — no analog trigger hardware)
        gp.mButtons[GAMEPAD_L1] = (b & PSP_CTRL_LTRIGGER) ? 1 : 0;
        gp.mButtons[GAMEPAD_R1] = (b & PSP_CTRL_RTRIGGER) ? 1 : 0;
        gp.mButtons[GAMEPAD_L2] = 0;
        gp.mButtons[GAMEPAD_R2] = 0;

        // D-pad
        gp.mButtons[GAMEPAD_UP]    = (b & PSP_CTRL_UP)    ? 1 : 0;
        gp.mButtons[GAMEPAD_DOWN]  = (b & PSP_CTRL_DOWN)  ? 1 : 0;
        gp.mButtons[GAMEPAD_LEFT]  = (b & PSP_CTRL_LEFT)  ? 1 : 0;
        gp.mButtons[GAMEPAD_RIGHT] = (b & PSP_CTRL_RIGHT) ? 1 : 0;

        // System buttons. HOME on PSP firmware-2.0+ usually triggers the
        // OS overlay before we see it, but pass it through anyway so games
        // CAN react when the kernel lets us (e.g. PSPSDK-trapped HOME).
        gp.mButtons[GAMEPAD_START]  = (b & PSP_CTRL_START)  ? 1 : 0;
        gp.mButtons[GAMEPAD_SELECT] = (b & PSP_CTRL_SELECT) ? 1 : 0;
        gp.mButtons[GAMEPAD_HOME]   = (b & PSP_CTRL_HOME)   ? 1 : 0;

        // PSP has no R3/L3 stick-click. Leave the THUMBL/THUMBR buttons
        // and right-stick virtual buttons at zero so scripts don't read
        // stale values from a previous frame.
        gp.mButtons[GAMEPAD_THUMBL] = 0;
        gp.mButtons[GAMEPAD_THUMBR] = 0;
        gp.mButtons[GAMEPAD_R_LEFT]  = 0;
        gp.mButtons[GAMEPAD_R_RIGHT] = 0;
        gp.mButtons[GAMEPAD_R_UP]    = 0;
        gp.mButtons[GAMEPAD_R_DOWN]  = 0;

        // Analog stick. PSP reports 0..255 with center ~128. Lx grows
        // rightward; Ly grows DOWNWARD (screen-y convention) but engine
        // convention is +Y = up, so we invert Y.
        //
        // Deadzone: PSP analog sticks drift heavily — typical units at rest
        // sit anywhere from 100..160 raw, well above any single fixed
        // centre. Without clamping rest values to zero, the drift wobbles
        // around the engine's 0.5 threshold for the virtual
        // GAMEPAD_L_UP / L_DOWN / L_LEFT / L_RIGHT buttons (set in
        // InputUtils::InputPostUpdate), and every wobble counts as another
        // "just down" transition. Button widget nav reads
        // `IsJustDown(L_UP) || IsJustDown(UP)`, so a single d-pad press
        // ends up navigating 2-3 entries instead of 1. A 0.30 deadzone
        // (~38 raw units) zeros out the vast majority of drift while still
        // letting a deliberate push register past the 0.5 virtual-button
        // threshold.
        constexpr float kAnalogDeadzone = 0.30f;
        auto applyDeadzone = [](float v) -> float {
            if (v >  kAnalogDeadzone) return (v - kAnalogDeadzone) / (1.0f - kAnalogDeadzone);
            if (v < -kAnalogDeadzone) return (v + kAnalogDeadzone) / (1.0f - kAnalogDeadzone);
            return 0.0f;
        };

        const float ax = (float(pad.Lx) - 128.0f) / 128.0f;
        const float ay = (float(pad.Ly) - 128.0f) / 128.0f;
        gp.mAxes[GAMEPAD_AXIS_LTHUMB_X] = glm::clamp(applyDeadzone( ax), -1.0f, 1.0f);
        gp.mAxes[GAMEPAD_AXIS_LTHUMB_Y] = glm::clamp(applyDeadzone(-ay), -1.0f, 1.0f);

        // No right stick on PSP hardware.
        gp.mAxes[GAMEPAD_AXIS_RTHUMB_X] = 0.0f;
        gp.mAxes[GAMEPAD_AXIS_RTHUMB_Y] = 0.0f;

        // No analog triggers on PSP either — surface the L/R digital state
        // as 0/1 trigger axes so script code that reads triggers gets a
        // sensible value rather than always-zero.
        gp.mAxes[GAMEPAD_AXIS_LTRIGGER] = (b & PSP_CTRL_LTRIGGER) ? 1.0f : 0.0f;
        gp.mAxes[GAMEPAD_AXIS_RTRIGGER] = (b & PSP_CTRL_RTRIGGER) ? 1.0f : 0.0f;
    }

    InputPostUpdate();
}

// PSP has no cursor / mouse / soft keyboard concept exposed by the engine's
// interface, but the symbols are required for link.
void INP_SetCursorPos(int32_t /*x*/, int32_t /*y*/) {}
void INP_ShowCursor(bool /*show*/) {}
void INP_LockCursor(bool /*lock*/) {}
void INP_TrapCursor(bool /*trap*/) {}
void INP_TrapCursorToRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}

const char* INP_ShowSoftKeyboard(bool /*show*/) { return nullptr; }
bool INP_IsSoftKeyboardShown() { return false; }

#endif // POLYPHASE_PLATFORM_ADDON
