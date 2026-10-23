/* xinput_posix.c -- XInput and the WinMM joystick API, on SDL3.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * See xinput_posix.h for what this is and why the three input paths are kept
 * apart. The short version: SDL's gamepad abstraction mirrors XInput almost
 * one to one, so the XInput half is a near-direct translation; the WinMM half
 * is not, because WinMM addresses devices by its own numbering.
 *
 * SDL gamepad triggers use 0..32767 and XInput uses 0..255. SDL's raw
 * joystick trigger range is different; use the mapped gamepad API here.
 */
#include <SDL3/SDL.h>

#include <string.h>

#include "xinput_posix.h"

/* SDL is initialised by whichever subsystem opened first; these entry points
 * can be reached before or after that, so they ensure the gamepad subsystem
 * rather than assuming it. Refcounted, so a second call is free. */
static int ensure_gamepad(void)
{
    if (SDL_WasInit(SDL_INIT_GAMEPAD) == 0) {
        /* The host applies its own input_background/focus policy. */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK))
            return 0;
    }
    return 1;
}

/* The mapped SDL trigger is zero when released. */
static BYTE trigger_from_sdl(float raw)
{
    double t = (double)raw / 32767.0;

    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return (BYTE)(t * 255.0 + 0.5);
}

/* SDL's D-pad as buttons is four separate lookups; XINPUT_STATE has no hat
 * field at all, so the four bits are the only representation available. */
static WORD buttons_from_sdl(SDL_Gamepad *pad)
{
    WORD b = 0;

    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP))
        b |= XINPUT_GAMEPAD_DPAD_UP;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))
        b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
        b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
        b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START))
        b |= XINPUT_GAMEPAD_START;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK))
        b |= XINPUT_GAMEPAD_BACK;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK))
        b |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK))
        b |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))
        b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))
        b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    /* SDL names the face buttons by POSITION, not by label, because a PlayStation
     * pad puts a cross where an Xbox pad puts a circle and XInput's A/B/X/Y are
     * XInput's names for those positions on the Microsoft layout. SDL_GAMEPAD_
     * BUTTON_SOUTH is the A position -- its own comment says "e.g. Xbox A button"
     * -- so the mapping below is positional, not by printed label. Reading the
     * label instead would swap A and B on every non-Xbox pad. */
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH))
        b |= XINPUT_GAMEPAD_A;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST))
        b |= XINPUT_GAMEPAD_B;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST))
        b |= XINPUT_GAMEPAD_X;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH))
        b |= XINPUT_GAMEPAD_Y;
    return b;
}

/* --- XInput ---------------------------------------------------------------- */

static pthread_mutex_t s_pad_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_Gamepad *s_pads[XUSER_MAX_COUNT];
static XINPUT_GAMEPAD s_previous[XUSER_MAX_COUNT];
static DWORD s_packets[XUSER_MAX_COUNT];

/* Own the open handles, including controllers connected after startup. */
static SDL_Gamepad *gamepad_for_user(DWORD user)
{
    SDL_JoystickID *ids;
    int count, i;
    if (user >= XUSER_MAX_COUNT || !ensure_gamepad())
        return NULL;
    if (s_pads[user] && !SDL_GamepadConnected(s_pads[user])) {
        SDL_CloseGamepad(s_pads[user]);
        s_pads[user] = NULL;
        memset(&s_previous[user], 0, sizeof s_previous[user]);
    }
    if (s_pads[user])
        return s_pads[user];
    ids = SDL_GetGamepads(&count);
    for (i = 0; ids && i < count; i++) {
        unsigned slot;
        int used = 0;
        for (slot = 0; slot < XUSER_MAX_COUNT; slot++)
            if (s_pads[slot] && SDL_GetGamepadID(s_pads[slot]) == ids[i])
                used = 1;
        if (!used) {
            SDL_Gamepad *pad = SDL_OpenGamepad(ids[i]);
            if (pad) {
                SDL_SetGamepadPlayerIndex(pad, (int)user);
                s_pads[user] = pad;
                break;
            }
        }
    }
    SDL_free(ids);
    return s_pads[user];
}

static SHORT stick_y_from_sdl(Sint16 raw)
{
    return raw == -32768 ? 32767 : (SHORT)-raw;
}

DWORD WINAPI XInputGetState(DWORD user_index, XINPUT_STATE *state)
{
    SDL_Gamepad *pad;

    if (!state)
        return XINPUT_DEVICE_NOT_CONNECTED;
    if (!ensure_gamepad())
        return XINPUT_DEVICE_NOT_CONNECTED;

    pthread_mutex_lock(&s_pad_lock);
    pad = gamepad_for_user(user_index);
    if (!pad) {
        pthread_mutex_unlock(&s_pad_lock);
        return XINPUT_DEVICE_NOT_CONNECTED;
    }
    SDL_UpdateGamepads();
    memset(state, 0, sizeof *state);
    state->Gamepad.wButtons       = buttons_from_sdl(pad);
    state->Gamepad.bLeftTrigger   = trigger_from_sdl(
        SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    state->Gamepad.bRightTrigger  = trigger_from_sdl(
        SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    state->Gamepad.sThumbLX = (SHORT)SDL_GetGamepadAxis(pad,
                                                        SDL_GAMEPAD_AXIS_LEFTX);
    state->Gamepad.sThumbLY = stick_y_from_sdl(SDL_GetGamepadAxis(pad,
                                                        SDL_GAMEPAD_AXIS_LEFTY));
    state->Gamepad.sThumbRX = (SHORT)SDL_GetGamepadAxis(pad,
                                                        SDL_GAMEPAD_AXIS_RIGHTX);
    state->Gamepad.sThumbRY = stick_y_from_sdl(SDL_GetGamepadAxis(pad,
                                                        SDL_GAMEPAD_AXIS_RIGHTY));
    if (!s_packets[user_index] || memcmp(&state->Gamepad, &s_previous[user_index],
                                       sizeof state->Gamepad)) {
        s_previous[user_index] = state->Gamepad;
        s_packets[user_index]++;
    }
    state->dwPacketNumber = s_packets[user_index];
    pthread_mutex_unlock(&s_pad_lock);
    return 0;
}

DWORD WINAPI XInputSetState(DWORD user_index, XINPUT_VIBRATION *vibration)
{
    SDL_Gamepad *pad;
    int ok;
    if (!vibration)
        return XINPUT_DEVICE_NOT_CONNECTED;
    pthread_mutex_lock(&s_pad_lock);
    pad = gamepad_for_user(user_index);
    ok = pad && SDL_RumbleGamepad(pad, vibration->wLeftMotorSpeed,
                                  vibration->wRightMotorSpeed, 150);
    pthread_mutex_unlock(&s_pad_lock);
    return ok ? ERROR_SUCCESS : XINPUT_DEVICE_NOT_CONNECTED;
}

/* --- the WinMM joystick API -----------------------------------------------
 *
 * Addressed by SDL joystick index, zero-based. See the divergence note at the
 * bottom of xinput_posix.h: WinMM's own 1-based device numbering has no
 * counterpart here, and inventing a mapping that looked stable would be worse
 * than saying so. */

MMRESULT WINAPI joyGetPosEx(UINT joy_id, LPJOYINFOEX info)
{
    SDL_Joystick *joy;

    if (!info)
        return JOYERR_PARMS;
    if (info->dwSize != sizeof *info)
        return JOYERR_PARMS;

    /* ensure_gamepad() before ANY SDL call, not just the update: these
     * entry points can be the first thing in the process to touch SDL, and
     * SDL_UpdateJoysticks() before SDL_Init is a crash rather than an error. */
    if (!ensure_gamepad())
        return JOYERR_NOERROR + 1;

    SDL_UpdateJoysticks();
    joy = SDL_GetJoystickFromID((SDL_JoystickID)joy_id);
    if (!joy)
        return JOYERR_NOERROR + 1;   /* no such device; not a bad parameter */

    memset(info, 0, sizeof *info);
    info->dwSize = sizeof *info;
    info->dwFlags = info->dwFlags;  /* the caller's requested axes, kept */

    /* WinMM reports an axis as a 0..65535 UNCENTRED position: the driver
     * reports each axis's own minimum and maximum and the value is a fraction
     * of that span, so a centred stick rests at its MIDDLE and reads 32767,
     * not 0. SDL's raw axis is signed about a centre. That is the mapping, and
     * it is not optional -- handing WinMM's callers a signed number would put
     * every centre-detecting axis permanently off. */
    info->dwXpos = (DWORD)(SDL_GetJoystickAxis(joy, 0) + 32768);
    info->dwYpos = (DWORD)(SDL_GetJoystickAxis(joy, 1) + 32768);
    info->dwZpos = 32767u;
    info->dwRpos = 32767u;
    info->dwUpos = 32767u;
    info->dwVpos = 32767u;

    /* WinMM's button word is one bit per button, MSB first. */
    {
        int nb = SDL_GetNumJoystickButtons(joy);
        int i;
        if (nb > 32) nb = 32;
        for (i = 0; i < nb; i++)
            if (SDL_GetJoystickButton(joy, i))
                info->dwButtons |= (DWORD)1 << (31 - i);
    }
    info->dwButtonNumber = (DWORD)SDL_GetNumJoystickButtons(joy);
    info->dwPOV = JOY_POVCENTERED;
    return JOYERR_NOERROR;
}

/* WinMM numbers its devices from 1, so its count is a count and SDL's is a
 * count too; the IDs are the part that does not line up (see the divergence
 * note in the header). */
UINT WINAPI joyGetNumDevs(void)
{
    SDL_JoystickID *ids;
    int n = 0;

    if (!ensure_gamepad())
        return 0;
    SDL_UpdateJoysticks();
    ids = SDL_GetJoysticks(&n);
    if (ids)
        SDL_free(ids);
    return (UINT)n;
}

MMRESULT WINAPI joyGetDevCapsA(UINT_PTR joy_id, LPJOYCAPSA caps, UINT caps_bytes)
{
    SDL_Joystick *joy;

    if (!caps || caps_bytes < sizeof *caps)
        return JOYERR_PARMS;
    if (!ensure_gamepad())
        return JOYERR_NOERROR + 1;

    SDL_UpdateJoysticks();
    joy = SDL_GetJoystickFromID((SDL_JoystickID)joy_id);
    if (!joy)
        return JOYERR_NOERROR + 1;

    memset(caps, 0, sizeof *caps);
    snprintf(caps->szPname, sizeof caps->szPname, "%s",
             SDL_GetJoystickName(joy) ? SDL_GetJoystickName(joy) : "");

    /* WinMM's axis range for a full-scale axis. SDL reports no physical
     * limits, so this is the conventional 0..65535 rather than a measurement,
     * and joyGetPosEx above produces values inside it by construction. */
    caps->wXmin = 0; caps->wXmax = 65535;
    caps->wYmin = 0; caps->wYmax = 65535;
    caps->wZmin = 0; caps->wZmax = 65535;
    caps->wRmin = 0; caps->wRmax = 65535;
    caps->wUmin = 0; caps->wUmax = 65535;
    caps->wVmin = 0; caps->wVmax = 65535;
    caps->wPeriodMin = 0;
    caps->wPeriodMax = 1000;

    caps->wNumAxes = (UINT)SDL_GetNumJoystickAxes(joy);
    caps->wMaxAxes = 8;
    caps->wNumButtons = (UINT)SDL_GetNumJoystickButtons(joy);
    caps->wMaxButtons = (caps->wNumButtons < 32) ? caps->wNumButtons : 32;
    caps->wCaps = JOYCAPS_HASZ | JOYCAPS_HASR | JOYCAPS_HASU | JOYCAPS_HASV |
                  JOYCAPS_HASPOV;
    caps->wMid = (WORD)SDL_GetJoystickVendor(joy);
    caps->wPid = (WORD)SDL_GetJoystickProduct(joy);
    return JOYERR_NOERROR;
}
