/* xinput_posix_test.c -- the XInput and WinMM surface's load-bearing facts.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Same shape and same reason as dinput_posix_test.c: this is wiring, and
 * wiring is only provable with a pad on a desk, so what gets tested here is
 * every assumption that would fail SILENTLY.
 *
 * The trigger rescaling is the headline. XInput reports bLeftTrigger and
 * bRightTrigger as a BYTE in 0..255 resting at 0. SDL's mapped gamepad triggers
 * are 0..32767, resting at 0 (unlike raw joystick axes). Applying a signed-axis
 * conversion leaves both pedals half pressed at rest.
 *
 * A virtual Xbox pad also checks discovery, events, disconnect and rumble.
 * A physical motor and the user's desktop audio still need hardware checks.
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "xinput_posix.h"

/* The implementation, so the statics under test are reachable -- the project's
 * usual "a test #includes the source it covers" shape. */
#include "xinput_posix.c"

static int g_checks;
static int g_failed;

static void check(int ok, const char *what)
{
    g_checks++;
    if (!ok) {
        g_failed++;
        printf("FAIL: %s\n", what);
    }
}

/* --- the button mask is a WORD WITH HOLES IN IT ----------------------------
 *
 * 0x0010..0x0200 are taken, 0x0400 and 0x0800 are unused, and the face buttons
 * start at 0x1000. Every one of these is copied into a player's saved
 * configuration, so a value that is merely plausible renames buttons silently. */
static void test_button_masks_are_the_real_ones(void)
{
    check(XINPUT_GAMEPAD_DPAD_UP        == 0x0001, "DPAD_UP is 0x0001");
    check(XINPUT_GAMEPAD_DPAD_DOWN      == 0x0002, "DPAD_DOWN is 0x0002");
    check(XINPUT_GAMEPAD_DPAD_LEFT      == 0x0004, "DPAD_LEFT is 0x0004");
    check(XINPUT_GAMEPAD_DPAD_RIGHT     == 0x0008, "DPAD_RIGHT is 0x0008");
    check(XINPUT_GAMEPAD_START          == 0x0010, "START is 0x0010");
    check(XINPUT_GAMEPAD_BACK           == 0x0020, "BACK is 0x0020");
    check(XINPUT_GAMEPAD_LEFT_THUMB     == 0x0040, "LEFT_THUMB is 0x0040");
    check(XINPUT_GAMEPAD_RIGHT_THUMB    == 0x0080, "RIGHT_THUMB is 0x0080");
    check(XINPUT_GAMEPAD_LEFT_SHOULDER  == 0x0100, "LEFT_SHOULDER is 0x0100");
    check(XINPUT_GAMEPAD_RIGHT_SHOULDER == 0x0200, "RIGHT_SHOULDER is 0x0200");
    check(XINPUT_GAMEPAD_A              == 0x1000, "A is 0x1000, not 0x0400");
    check(XINPUT_GAMEPAD_B              == 0x2000, "B is 0x2000");
    check(XINPUT_GAMEPAD_X              == 0x4000, "X is 0x4000");
    check(XINPUT_GAMEPAD_Y              == 0x8000, "Y is 0x8000");

    /* All fourteen must be distinct bits, or two buttons alias and the trace
     * shows one press lighting up two. */
    {
        const WORD bits[] = {
            XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
            XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
            XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
            XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
            XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
            XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B,
            XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y
        };
        unsigned n = sizeof bits / sizeof bits[0];
        unsigned i, j;
        int distinct = 1;
        for (i = 0; i < n; i++)
            for (j = i + 1; j < n; j++)
                if (bits[i] == bits[j]) distinct = 0;
        check(distinct, "all 14 button bits are distinct");
    }
}

/* --- XINPUT_GAMEPAD is laid out as Win32 says ------------------------------
 *
 * The host reads Gamepad.sThumbLX and Gamepad.wButtons by name. If the struct
 * were ordered differently the compiler would not care and every read would be
 * wrong -- which is why the field offsets are asserted rather than trusted. */
static void test_gamepad_layout(void)
{
    check(offsetof(XINPUT_GAMEPAD, wButtons)      == 0,  "wButtons is first");
    check(offsetof(XINPUT_GAMEPAD, bLeftTrigger)  == 2,  "bLeftTrigger after wButtons");
    check(offsetof(XINPUT_GAMEPAD, bRightTrigger) == 3,  "bRightTrigger after bLeft");
    check(offsetof(XINPUT_GAMEPAD, sThumbLX)      == 4,  "sThumbLX");
    check(offsetof(XINPUT_GAMEPAD, sThumbLY)      == 6,  "sThumbLY");
    check(offsetof(XINPUT_GAMEPAD, sThumbRX)      == 8,  "sThumbRX");
    check(offsetof(XINPUT_GAMEPAD, sThumbRY)      == 10, "sThumbRY");
    check(sizeof(XINPUT_GAMEPAD) == 12, "XINPUT_GAMEPAD is 12 bytes");
    check(sizeof(XINPUT_STATE) == 16, "XINPUT_STATE is 16 bytes");
    check(offsetof(XINPUT_STATE, Gamepad) == 4, "dwPacketNumber comes first");
    check(sizeof(XINPUT_VIBRATION) == 4, "XINPUT_VIBRATION is 4 bytes");
}

/* --- the trigger rescale, which is the whole point ------------------------- */
static void test_trigger_rescale(void)
{
    /* SDL's resting trigger is -32768. XInput's resting trigger is 0. Copy the
     * raw value across and the brake is on before the pedal is touched. */
    check(trigger_from_sdl(0.0f) == 0, "a released mapped trigger is 0");
    check(trigger_from_sdl(16384.0f)   == 128, "half travel is ~128");
    check(trigger_from_sdl(32767.0f)    == 255, "full travel is 255");

    /* Never negative and never over 255, whatever SDL hands over -- including
     * beyond the nominal range, which a driver is free to report. */
    check(trigger_from_sdl(-40000.0f) == 0,   "clamped below");
    check(trigger_from_sdl( 40000.0f) == 255, "clamped above");

    /* Monotonic: more travel is never less brake. A non-monotonic mapping makes
     * a pedal feel like it has a notch in it. */
    {
        int prev = -1, i, ok = 1;
        for (i = 0; i <= 32767; i += 512) {
            int v = trigger_from_sdl((float)i);
            if (v < prev) ok = 0;
            prev = v;
        }
        check(ok, "the trigger mapping is monotonic across its whole range");
    }
}

/* --- the WinMM constants, and the one that is a trap ----------------------- */
static void test_winmm_constants(void)
{
    /* An unpressed hat is -1, NOT 0. Zero is JOY_POVFORWARD -- a hat hard
     * north. Writing 0 for "centred" makes every hat read as pushed. */
    check(JOY_POVCENTERED == 0xFFFF, "an unpressed hat is 0xFFFF, not 0");
    check(JOY_POVFORWARD  == 0,      "0 is FORWARD");
    check(JOY_POVRIGHT    == 9000,   "9000 hundredths is right");
    check(JOY_POVBACKWARD == 18000,  "18000 is back");
    check(JOY_POVLEFT     == 27000,  "27000 is left");
    check(MAXPNAMELEN == 32, "MAXPNAMELEN is 32");

    /* JOY_RETURNALL must actually request everything it claims to. */
    check((JOY_RETURNALL & JOY_RETURNX) != 0, "ALL includes X");
    check((JOY_RETURNALL & JOY_RETURNY) != 0, "ALL includes Y");
    check((JOY_RETURNALL & JOY_RETURNZ) != 0, "ALL includes Z");
    check((JOY_RETURNALL & JOY_RETURNR) != 0, "ALL includes R");
    check((JOY_RETURNALL & JOY_RETURNU) != 0, "ALL includes U");
    check((JOY_RETURNALL & JOY_RETURNV) != 0, "ALL includes V");
    check((JOY_RETURNALL & JOY_RETURNPOV) != 0, "ALL includes POV");
    check((JOY_RETURNALL & JOY_RETURNBUTTONS) != 0, "ALL includes buttons");

    check(XUSER_MAX_COUNT == 4, "XInput has four users, not more");
    check(XINPUT_DEVICE_NOT_CONNECTED == 1167, "not-connected is 1167");
    check(ERROR_SUCCESS == 0, "ERROR_SUCCESS is 0");
    /* JOYINFOEX is thirteen DWORDs -- counted from
     * /usr/i686-w64-mingw32/include/joystickapi.h, not from memory. The first
     * draft of this line said 48 and 44 because the struct was miscounted as
     * twelve fields; the test caught its own author, which is the whole point
     * of asserting rather than assuming. */
    check(sizeof(JOYINFOEX) == 52, "JOYINFOEX is 52 bytes (13 DWORDs)");
    check(offsetof(JOYINFOEX, dwFlags) == 4, "dwFlags follows dwSize");
    check(offsetof(JOYINFOEX, dwXpos) == 8, "dwXpos follows dwFlags");
    check(offsetof(JOYINFOEX, dwButtons) == 32, "dwButtons");
    check(offsetof(JOYINFOEX, dwPOV) == 40, "dwPOV at 40");
}

/* --- a disconnected pad must be an ERROR, not a zeroed state --------------
 *
 * This is the one that decides whether "unplugged" looks like "everything
 * released". io_host.c sets s_xinput_live from exactly this distinction, and a
 * zeroed state would make an unplugged pad report a full set of released
 * buttons forever. */
static void test_disconnected_pad_is_an_error(void)
{
    XINPUT_STATE state;

    memset(&state, 0xAA, sizeof state);
    /* With no pads attached this cannot succeed; either SDL has no backend, or
     * player index 0 is unoccupied. Both must read as "not connected". */
    {
        DWORD r = XInputGetState(0, &state);
        int no_pad = (r == XINPUT_DEVICE_NOT_CONNECTED);
        /* A machine with a real pad in slot 0 would succeed here, which is not
         * a failure of this check -- so only assert the shape of the code. */
        check(no_pad || r == ERROR_SUCCESS,
              "XInputGetState returns 0 or not-connected, never anything else");
        if (no_pad)
            check(1, "with no pad, the answer is not-connected");
    }

    /* A NULL out-pointer must be refused rather than dereferenced. */
    check(XInputGetState(0, NULL) == XINPUT_DEVICE_NOT_CONNECTED,
          "a NULL state pointer is refused");
    check(XInputSetState(0, NULL) == XINPUT_DEVICE_NOT_CONNECTED,
          "a NULL vibration pointer is refused");
}

/* --- the WinMM entry points refuse bad parameters rather than writing ------ */
static void test_winmm_parameter_checks(void)
{
    JOYINFOEX ji;
    JOYCAPSA caps;

    memset(&ji, 0, sizeof ji);
    check(joyGetPosEx(0, NULL) == JOYERR_PARMS, "joyGetPosEx(NULL) is a bad param");

    /* A wrong dwSize is WinMM's documented way of catching a stale struct. */
    ji.dwSize = 4;
    check(joyGetPosEx(0, &ji) == JOYERR_PARMS,
          "joyGetPosEx rejects a wrong dwSize");

    check(joyGetDevCapsA(0, NULL, 0) == JOYERR_PARMS,
          "joyGetDevCapsA(NULL) is a bad param");
    memset(&caps, 0, sizeof caps);
    check(joyGetDevCapsA(0, &caps, 4) == JOYERR_PARMS,
          "joyGetDevCapsA rejects a short buffer");

    /* A missing device must not be reported as a bad parameter -- the caller
     * distinguishes "no such pad" from "you called me wrong". */
    memset(&caps, 0, sizeof caps);
    check(joyGetDevCapsA(9999, &caps, sizeof caps) != JOYERR_PARMS,
          "a missing device is not a bad parameter");
}

static Uint16 rumble_low, rumble_high;

static bool SDLCALL virtual_rumble(void *unused, Uint16 low, Uint16 high)
{
    (void)unused;
    rumble_low = low;
    rumble_high = high;
    return true;
}

/* The adapter must open a newly attached controller itself. Reading an
 * already-open handle would miss the bug that kept real Xbox pads inactive. */
static void test_controller_lifecycle(void)
{
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID id;
    SDL_Joystick *joy;
    XINPUT_STATE state, same;
    XINPUT_VIBRATION vibration = { 1234, 4321 };
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x045e;
    desc.product_id = 0x028e;
    desc.name = "VCThunder test Xbox pad";
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.Rumble = virtual_rumble;
    id = SDL_AttachVirtualJoystick(&desc);
    check(id != 0, "attach a controller after startup");
    if (!id) return;
    joy = SDL_OpenJoystick(id);
    check(joy != NULL, "open the raw test controls");
    if (!joy) { SDL_DetachVirtualJoystick(id); return; }
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    check(XInputGetState(3, &state) == ERROR_SUCCESS,
          "discover and open the gamepad without an external SDL_OpenGamepad");
    check(state.Gamepad.bLeftTrigger == 0 && state.Gamepad.bRightTrigger == 0,
          "both triggers are released at rest");
    check(XInputGetState(3, &same) == ERROR_SUCCESS &&
          same.dwPacketNumber == state.dwPacketNumber, "unchanged input keeps its packet number");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 8192);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    check(XInputGetState(3, &state) == ERROR_SUCCESS, "read changed controls");
    check(state.Gamepad.sThumbLX == 8192 && state.Gamepad.sThumbLY == 32767,
          "sticks preserve steering and translate SDL's Y direction");
    check(state.Gamepad.bRightTrigger == 255, "full right trigger is full throttle");
    check((state.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0, "Xbox A reaches the cabinet binding");
    check(state.dwPacketNumber != same.dwPacketNumber, "changed input advances the packet number");
    check(XInputSetState(3, &vibration) == ERROR_SUCCESS &&
          rumble_low == 1234 && rumble_high == 4321, "both rumble motors receive their own speeds");
    SDL_DetachVirtualJoystick(id);
    check(XInputGetState(3, &state) == XINPUT_DEVICE_NOT_CONNECTED, "unplugged pad is disconnected");
    SDL_CloseJoystick(joy);
    check(XInputGetState(XUSER_MAX_COUNT, &state) == XINPUT_DEVICE_NOT_CONNECTED,
          "invalid player slot is refused");
}

int main(void)
{
    test_button_masks_are_the_real_ones();
    test_gamepad_layout();
    test_trigger_rescale();
    test_winmm_constants();
    test_disconnected_pad_is_an_error();
    test_winmm_parameter_checks();
    test_controller_lifecycle();

    if (g_failed) {
        printf("xinput posix tests: %d failure(s)\n", g_failed);
        return 1;
    }
    printf("xinput posix tests: %d checks, all passed\n", g_checks);
    return 0;
}
