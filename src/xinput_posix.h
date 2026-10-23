/* xinput_posix.h -- XInput and the WinMM joystick API, on Linux.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The companion to dinput_posix.h, and the same deal: src/input_probe.c and
 * src/io_host.c are unchanged upstream sources that speak XInput and WinMM, and
 * this declares the vocabulary they are written against so they compile here.
 *
 * WHY THIS EXISTS AT ALL, given dinput_posix.h already covers wheels and pads:
 * the three are read side by side and deliberately kept apart by upstream.
 * DirectInput is the configured wheel; XInput is the fallback for a pad the HID
 * rule (xinput_hid.h) says to read through XInput instead; WinMM is a third
 * path for a plain joystick. Collapsing them would change which device a player
 * ends up holding, which is not this port's business.
 *
 * AND WHY SDL3 IS THE RIGHT BACKING FOR XINPUT SPECIFICALLY: SDL's gamepad
 * abstraction was designed to mirror XInput -- four buttons named A/B/X/Y, a
 * D-pad, two shoulders, two thumbs, two triggers, a -32768..32767 stick range,
 * and two rumble motors. So XInputGetState maps onto SDL gamepad almost one to
 * one, and XINPUT_STATE's dwPacketNumber has a direct analogue in SDL's own.
 * That is not true of the WinMM half, which is addressed by raw joystick index;
 * see the divergence note at the bottom of this header.
 *
 * The structs are the real ones, field for field, because these files write
 * named fields and a plausible substitute would compile and then read the wrong
 * bytes. The values are taken from /usr/i686-w64-mingw32/include/{xinput,
 * joystickapi,mmsyscom}.h rather than from memory.
 */
#ifndef VCTH_XINPUT_POSIX_H
#define VCTH_XINPUT_POSIX_H


#include "posix_compat.h"

/* Win32's "the same pointer type as UINT, but for a handle-like value". The
 * joystick API takes device ids as UINT_PTR; on 32-bit Linux that is the same
 * size as UINT, which is why no separate width is needed here. */
typedef unsigned int UINT_PTR;
typedef unsigned int UINT;

/* --- WinMM result codes ---------------------------------------------------- */

typedef UINT MMRESULT;

#define MMSYSERR_BASE        0
#define MMSYSERR_NOERROR     0
#define MMSYSERR_NODRIVER    (MMSYSERR_BASE + 6)
#define MMSYSERR_BADDEVICEID (MMSYSERR_BASE + 2)
#define JOYERR_NOERROR       0
#define JOYERR_PARMS         0x00000010u   /* a bad parameter was passed */
#define JOYERR_NOERROR2      0

/* --- the WinMM joystick API ------------------------------------------------ */

#define MAXPNAMELEN              32
#define MAX_JOYSTICKOEMVXDNAME   260

/* Which axes a read is asked for. The host asks for JOY_RETURNALL, and the
 * values are spelled out because that macro is an OR of eight of them. */
#define JOY_RETURNX        0x00000001u
#define JOY_RETURNY        0x00000002u
#define JOY_RETURNZ        0x00000004u
#define JOY_RETURNR        0x00000008u
#define JOY_RETURNU        0x00000010u
#define JOY_RETURNV        0x00000020u
#define JOY_RETURNPOV      0x00000040u
#define JOY_RETURNBUTTONS  0x00000080u
#define JOY_RETURNRAWDATA  0x00000100u
#define JOY_RETURNPOVCTS   0x00000200u
#define JOY_RETURNCENTERED 0x00000400u
#define JOY_RETURNALL      (JOY_RETURNX | JOY_RETURNY | JOY_RETURNZ |    \
                            JOY_RETURNR | JOY_RETURNU | JOY_RETURNV |  \
                            JOY_RETURNPOV | JOY_RETURNBUTTONS)

/* A hat that is not pushed, in WinMM's own hundredths-of-a-degree encoding.
 * Not 0: 0 is JOY_POVFORWARD, i.e. a hat hard north. */
#define JOY_POVCENTERED   ((WORD)-1)
#define JOY_POVFORWARD    0
#define JOY_POVRIGHT      9000
#define JOY_POVBACKWARD   18000
#define JOY_POVLEFT       27000

#define JOYCAPS_HASZ      0x0001
#define JOYCAPS_HASR      0x0002
#define JOYCAPS_HASU      0x0004
#define JOYCAPS_HASV      0x0008
#define JOYCAPS_HASPOV    0x0010
#define JOYCAPS_POV4DIR   0x0020
#define JOYCAPS_POVCTS    0x0040

typedef struct tagJOYCAPSA {
    WORD        wMid;
    WORD        wPid;
    CHAR        szPname[MAXPNAMELEN];
    UINT        wXmin;
    UINT        wXmax;
    UINT        wYmin;
    UINT        wYmax;
    UINT        wZmin;
    UINT        wZmax;
    UINT        wNumButtons;
    UINT        wPeriodMin;
    UINT        wPeriodMax;
    UINT        wRmin;
    UINT        wRmax;
    UINT        wUmin;
    UINT        wUmax;
    UINT        wVmin;
    UINT        wVmax;
    UINT        wCaps;
    UINT        wMaxAxes;
    UINT        wNumAxes;
    UINT        wMaxButtons;
    CHAR        szRegKey[MAXPNAMELEN];
    CHAR        szOEMVxD[MAX_JOYSTICKOEMVXDNAME];
} JOYCAPSA, *PJOYCAPSA, *LPJOYCAPSA;

typedef struct joyinfoex_tag {
    DWORD       dwSize;
    DWORD       dwFlags;
    DWORD       dwXpos;
    DWORD       dwYpos;
    DWORD       dwZpos;
    DWORD       dwRpos;
    DWORD       dwUpos;
    DWORD       dwVpos;
    DWORD       dwButtons;
    DWORD       dwButtonNumber;
    DWORD       dwPOV;
    DWORD       dwReserved1;
    DWORD       dwReserved2;
} JOYINFOEX, *PJOYINFOEX, *LPJOYINFOEX;

/* --- XInput ---------------------------------------------------------------- */

typedef struct _XINPUT_GAMEPAD {
    WORD        wButtons;
    BYTE        bLeftTrigger;
    BYTE        bRightTrigger;
    SHORT       sThumbLX;
    SHORT       sThumbLY;
    SHORT       sThumbRX;
    SHORT       sThumbRY;
} XINPUT_GAMEPAD, *PXINPUT_GAMEPAD;

typedef struct _XINPUT_STATE {
    DWORD           dwPacketNumber;
    XINPUT_GAMEPAD  Gamepad;
} XINPUT_STATE, *PXINPUT_STATE;

typedef struct _XINPUT_VIBRATION {
    WORD        wLeftMotorSpeed;
    WORD        wRightMotorSpeed;
} XINPUT_VIBRATION, *PXINPUT_VIBRATION;

/* The button mask is a WORD with HOLES in it -- 0x0010..0x0200 is taken and
 * 0x0400/0x0800 are unused, because the face buttons live at 0x1000. These
 * values are the saved-configuration-adjacent kind of constant: a user's ini
 * names buttons through them, so they are copied, not reconstructed. */
#define XINPUT_GAMEPAD_DPAD_UP          0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN        0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT        0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT       0x0008
#define XINPUT_GAMEPAD_START            0x0010
#define XINPUT_GAMEPAD_BACK             0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB       0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB      0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER    0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER   0x0200
#define XINPUT_GAMEPAD_A                0x1000
#define XINPUT_GAMEPAD_B                0x2000
#define XINPUT_GAMEPAD_X                0x4000
#define XINPUT_GAMEPAD_Y                0x8000

#define XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE    7849
#define XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE   8689
#define XINPUT_GAMEPAD_TRIGGER_THRESHOLD      30

/* XInputGetState and XInputSetState both return 0 for success, or
 * ERROR_DEVICE_NOT_CONNECTED. They do NOT return an MMRESULT -- a detail worth
 * keeping, because the two families are otherwise easy to conflate. */
#define XINPUT_DEVICE_NOT_CONNECTED  1167u

/* XInput supports four users, and that is the whole enumeration range the
 * host walks. Not a guess about how many pads exist: XInput's limit is four,
 * and enumerating past it would invent devices. */
#define XUSER_MAX_COUNT          4

/* ERROR_SUCCESS, which XInputGetState is compared against, is a Win32 general
 * code and already lives in posix_compat.h. Not redefined here. */

/* --- the implementation ---------------------------------------------------- */

/* XInputGetState / XInputSetState, by name, so the host's own function-pointer
 * indirection (it resolves xinput1_4.dll on Windows) has something to point at
 * here and stays unchanged. */
DWORD WINAPI XInputGetState(DWORD user_index, XINPUT_STATE *state);
DWORD WINAPI XInputSetState(DWORD user_index, XINPUT_VIBRATION *vibration);

/* The WinMM pair, called by name for the same reason. */
MMRESULT WINAPI joyGetPosEx(UINT joy_id, LPJOYINFOEX info);
MMRESULT WINAPI joyGetDevCapsA(UINT_PTR joy_id, LPJOYCAPSA caps, UINT caps_bytes);
UINT WINAPI joyGetNumDevs(void);

/* KNOWN DIVERGENCE, WinMM addressing. The WinMM joystick API numbers devices
 * itself, from 1, in an order Windows decided. SDL numbers its own joysticks
 * from 0 in a different order. There is no mapping between them that holds
 * across machines, so here the WinMM id is taken as a zero-based SDL joystick
 * index and 0 is reserved for "no such device" the way WinMM's 0 means invalid.
 *
 * That is a divergence, and it is the honest one: it means a WinMM slot id
 * saved in a player's ini does not select the same physical device on Linux.
 * The XInput path -- which is the one the HID rule actually sends pads down --
 * is unaffected, because XInput's user index IS a player index and SDL's is
 * too. */
#define WINMM_ID_INVALID ((UINT_PTR)-1)

#endif /* VCTH_XINPUT_POSIX_H */
