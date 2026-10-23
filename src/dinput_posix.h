/* dinput_posix.h -- the DirectInput surface src/dinput.c and src/input_probe.c
 * are written against, on Linux.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * There is no DirectInput on Linux, and this is not a wrapper around one: it
 * is the vocabulary. The two host files are unchanged upstream sources that
 * speak DirectInput, and they speak it in two ways that matter:
 *
 *   1. The __wrapper macros -- IDirectInputDevice8_Acquire(dev) and the rest.
 *      On Windows these expand to `((dev)->lpVtbl->Acquire(dev))`, reaching
 *      through a COM vtable. Here they expand to plain function calls against
 *      an ordinary struct, which is the same trick src/posix_compat.h uses for
 *      WritePrivateProfileStringA. The project never touches lpVtbl itself, so
 *      there is no COM vtable to emulate and no reference counting to keep
 *      straight.
 *
 *   2. The GUIDs. di_canon.h resolves an axis by guidType, and those GUIDs
 *      become the axis NAMES, which are then WRITTEN TO THE INI. A player's
 *      saved configuration names axes by these values. So they are not
 *      placeholders: every GUID below is copied verbatim from
 *      /usr/i686-w64-mingw32/include/dinput.h, because a plausible-looking
 *      invented value would type-check, run, and silently renumber every
 *      saved axis in every existing config. If one is ever changed here, every
 *      config written by the Windows build becomes unreadable.
 *
 * The layouts are the real ones for the same reason. DIJOFS_* are offsetof()
 * into DIJOYSTATE2, so an axis's canonical offset and the member the host
 * reads it from cannot drift apart: both come from one struct definition.
 *
 * THE PROPERTY IDS ARE THE ONE PLACE THIS IS A SIMPLIFICATION, AND IT IS
 * DELIBERATE. On Windows MAKEDIPROP(n) is `(*(const GUID *)(n))` -- the
 * numeric property id IS a pointer to a GUID. Here it is the same shape, an
 * integer id wearing a pointer's clothes, because the project passes
 * DIPROP_RANGE and friends straight through to SetProperty/GetProperty and
 * never dereferences or compares them. The implementation reads the id back
 * out of the pointer. If some future caller ever MAKES a GUID from these,
 * this stops working -- which is why the typedef is named to give it away.
 */
#ifndef VCTH_DINPUT_POSIX_H
#define VCTH_DINPUT_POSIX_H


#include <stddef.h>

#include "posix_compat.h"

/* --- object handles ------------------------------------------------------
 *
 * On Windows these are COM interface pointers. Here each is one plain struct
 * defined in dinput_posix.c, and callers only ever pass the pointer around --
 * they never look inside, so an opaque forward declaration is enough and the
 * layout stays the implementation's business. */
typedef struct posix_di          IDirectInput8,          *LPDIRECTINPUT8A;
typedef struct posix_di_device   IDirectInputDevice8,    *LPDIRECTINPUTDEVICE8A;
typedef struct posix_di_effect   IDirectInputEffect,     *LPDIRECTINPUTEFFECT;

/* The A/IID spelled without the Windows decoration the project uses. */
#define LPDIRECTINPUT8           LPDIRECTINPUT8A
#define LPDIRECTINPUTDEVICE8     LPDIRECTINPUTDEVICE8A

/* COM reference-to-pointer spellings.
 *
 * REFIID is spelled as a GUID pointer, not an IID pointer, and that is a
 * deliberate divergence from the Windows headers. DirectInput's riid parameter
 * is matched against a GUID value in practice -- the host passes
 * &IID_IDirectInput8A, which DEFINE_GUID declares as a GUID, not an IID -- so
 * declaring it as REFIID would make the one call site an incompatible-pointer
 * error that Windows tolerates and Linux would not. */
typedef const GUID *REFGUID;
typedef const GUID *REFIID;

/* --- enumerations --------------------------------------------------------- */

/* A device identity. The host stores these GUIDs in the ini as the stable name
 * of a device, so guidInstance and guidProduct must both be real and stable
 * across runs on the same machine. */
typedef struct DIDEVICEINSTANCEA {
    DWORD       dwSize;
    GUID        guidInstance;
    GUID        guidProduct;
    DWORD       dwDevType;
    CHAR        tszInstanceName[MAX_PATH];
    CHAR        tszProductName[MAX_PATH];
    GUID        guidFFDriver;
    WORD        wUsagePage;
    WORD        wUsage;
} DIDEVICEINSTANCEA, *LPDIDEVICEINSTANCEA;

#define LPCDIDEVICEINSTANCEA const DIDEVICEINSTANCEA *

/* One axis, button or POV on a device.
 *
 * dwOfs is the offset into the DIJOYSTATE2 the object's data lands at, and
 * di_canon.h deliberately IGNORES it in favour of guidType: on a G29, a G923
 * and a Pro DD the driver's reported dwOfs is refused and the GUID-derived
 * one accepted. Both are recorded (slot->ffb_ofs and slot->ffb_canon) because
 * the difference between them is the bug this header exists to keep fixed. */
typedef struct DIDEVICEOBJECTINSTANCEA {
    DWORD       dwSize;
    GUID        guidType;
    DWORD       dwOfs;
    DWORD       dwType;
    DWORD       dwFlags;
    CHAR        tszName[MAX_PATH];
    DWORD       dwFFMaxForce;
    DWORD       dwFFForceResolution;
    WORD        wCollectionNumber;
    WORD        wDesignatorIndex;
    WORD        wUsagePage;
    WORD        wUsage;
} DIDEVICEOBJECTINSTANCEA, *LPDIDEVICEOBJECTINSTANCEA;

#define LPCDIDEVICEOBJECTINSTANCEA const DIDEVICEOBJECTINSTANCEA *

/* What a device can do, as the host asks it. */
typedef struct DIDEVCAPS {
    DWORD       dwSize;
    DWORD       dwFlags;
    DWORD       dwDevType;
    DWORD       dwAxes;
    DWORD       dwButtons;
    DWORD       dwPOVs;
    DWORD       dwFFSamplePeriod;
    DWORD       dwFFMinTimeResolution;
    DWORD       dwFirmwareRevision;
    DWORD       dwHardwareRevision;
    DWORD       dwFFDriverVersion;
} DIDEVCAPS, *LPDIDEVCAPS;

typedef BOOL (CALLBACK *LPDIENUMDEVICECALLBACKA)(LPCDIDEVICEINSTANCEA, LPVOID);
typedef BOOL (CALLBACK *LPDIENUMDEVICEOBJECTCALLBACKA)(LPCDIDEVICEOBJECTINSTANCEA,
                                                      LPVOID);

/* --- the state block ------------------------------------------------------
 *
 * DIJOYSTATE2 in full, including the velocity/acceleration/force tails this
 * project never reads. They are here because the struct is what fixes the
 * offsets: dropping them would move nothing (they all follow rgbButtons) but
 * would make the definition a lie about the layout it is defining, and the
 * cost of being wrong here is a silently mis-mapped wheel. */
typedef struct DIJOYSTATE2 {
    LONG        lX, lY, lZ;
    LONG        lRx, lRy, lRz;
    LONG        rglSlider[2];
    DWORD       rgdwPOV[4];
    BYTE        rgbButtons[128];
    LONG        lVX, lVY, lVZ;
    LONG        lVRx, lVRy, lVRz;
    LONG        rglVSlider[2];
    LONG        lAX, lAY, lAZ;
    LONG        lARx, lARy, lARz;
    LONG        rglASlider[2];
    LONG        lFX, lFY, lFZ;
    LONG        lFRx, lFRy, lFRz;
    LONG        rglFSlider[2];
} DIJOYSTATE2, *LPDIJOYSTATE2;

/* DIJOFS_* are offsetof into the struct above, so the offset a property names
 * and the member the host reads are the same fact stated twice. On Windows
 * they are offsetof into DIJOYSTATE (the v1 struct); the fields they name are
 * at identical offsets in both, and naming DIJOYSTATE2 is what keeps them
 * tied to the block this port actually fills. */
#define DIJOFS_X          offsetof(DIJOYSTATE2, lX)
#define DIJOFS_Y          offsetof(DIJOYSTATE2, lY)
#define DIJOFS_Z          offsetof(DIJOYSTATE2, lZ)
#define DIJOFS_RX         offsetof(DIJOYSTATE2, lRx)
#define DIJOFS_RY         offsetof(DIJOYSTATE2, lRy)
#define DIJOFS_RZ         offsetof(DIJOYSTATE2, lRz)
#define DIJOFS_SLIDER(n)  (offsetof(DIJOYSTATE2, rglSlider) + \
                           ((n) * (DWORD)sizeof(LONG)))
#define DIJOFS_POV(n)     (offsetof(DIJOYSTATE2, rgdwPOV) + \
                           ((n) * (DWORD)sizeof(DWORD)))
#define DIJOFS_BUTTON(n)  (offsetof(DIJOYSTATE2, rgbButtons) + (n))

/* --- properties ----------------------------------------------------------- */

/* dwSize is the size of the WHOLE struct the header introduces (DIPROPRANGE,
 * DIPROPDWORD), not of the header. The host sets it to sizeof the outer struct
 * on every property call, so a header that omitted it would not fail to
 * compile here -- it would silently mis-size every property written. */
typedef struct DIPROPHEADER {
    DWORD       dwSize;
    DWORD       dwHeaderSize;
    DWORD       dwObj;
    DWORD       dwHow;
} DIPROPHEADER, *LPDIPROPHEADER;

#define LPCDIPROPHEADER const DIPROPHEADER *

typedef struct DIPROPDWORD {
    DIPROPHEADER diph;
    DWORD        dwData;
} DIPROPDWORD, *LPDIPROPDWORD;

typedef struct DIPROPRANGE {
    DIPROPHEADER diph;
    LONG         lMin;
    LONG         lMax;
} DIPROPRANGE, *LPDIPROPRANGE;

/* A property id carried as a pointer. See the note at the top of this file:
 * this mirrors the Win32 ABI, and the value is read back out rather than
 * dereferenced. The name says "not a GUID" so nobody writes code that assumes
 * otherwise. */
typedef const void *REFPROPID;

#define MAKEDIPROP(prop)   ((REFPROPID)(size_t)(prop))

#define DIPROP_RANGE        MAKEDIPROP(4)
#define DIPROP_DEADZONE     MAKEDIPROP(5)
#define DIPROP_SATURATION   MAKEDIPROP(6)
#define DIPROP_FFGAIN       MAKEDIPROP(7)
#define DIPROP_AUTOCENTER   MAKEDIPROP(9)

#define DIPROPAUTOCENTER_OFF 0
#define DIPROPAUTOCENTER_ON  1

/* What a property request addresses: the device itself, or one object on it. */
#define DIPH_DEVICE     0
#define DIPH_BYOFFSET   1
#define DIPH_BYID       2
#define DIPH_BYUSAGE    3

/* --- force feedback -------------------------------------------------------
 *
 * ConstantForce only. That is not a shortcut: the host itself only ever
 * creates a constant force (src/dinput.c:756, GUID_ConstantForce), and
 * DICONSTANTFORCE is a single LONG. Conditional, ramp, sine and envelope
 * effects are a much larger surface with nothing in this codebase asking for
 * them. */
typedef struct DICONSTANTFORCE {
    LONG         lMagnitude;
} DICONSTANTFORCE, *LPDICONSTANTFORCE;

/* The effect description, in DirectInput's own order and with its own member
 * names -- rgdwAxes, not lpAxes, and lpvTypeSpecificParams, not
 * lpTypeSpecificParams. The host names all three fields when it builds a
 * constant force, so guessing the layout here would not fail to compile; it
 * would write a magnitude into the wrong field and produce a dead motor.
 *
 * DIEFF_CARTESIAN with one axis and a direction is the only shape used. */
typedef struct DIEFFECT {
    DWORD        dwSize;
    DWORD        dwFlags;
    DWORD        dwDuration;
    DWORD        dwSamplePeriod;
    DWORD        dwGain;
    DWORD        dwTriggerButton;
    DWORD        dwTriggerRepeatInterval;
    DWORD        cAxes;
    LPDWORD      rgdwAxes;
    LPLONG       rglDirection;
    void        *lpEnvelope;         /* DIEFFECTENVELOPE*, unused here */
    DWORD        cbTypeSpecificParams;
    LPVOID       lpvTypeSpecificParams;
    DWORD        dwStartDelay;
} DIEFFECT, *LPDIEFFECT;

#define LPCDIEFFECT const DIEFFECT *

#define DIEFF_OBJECTIDS          0x00000001
#define DIEFF_OBJECTOFFSETS      0x00000002
#define DIEFF_CARTESIAN          0x00000010

#define DIEP_START               0x20000000
#define DIEP_TYPESPECIFICPARAMS  0x00000100

/* The host scales a -1..1 float to this before handing it over. It is also
 * the ceiling a real constant force is clamped to, so it is a real constant
 * and not a placeholder. */
#define DI_FFNOMINALMAX          10000

/* --- the rest of the vocabulary ------------------------------------------- */

#define DIRECTINPUT_VERSION      0x0800

#define DI_OK                    S_OK
#define DIERR_INVALIDPARAM       E_INVALIDARG
#define DIERR_UNSUPPORTED        ((HRESULT)0x80004001L)
#define DIERR_NOERROR            S_OK

#define DIENUM_STOP              0
#define DIENUM_CONTINUE          1

#define DI8DEVCLASS_GAMECTRL     4

#define DIEDFL_ALLDEVICES        0x00000000
#define DIEDFL_ATTACHEDONLY      0x00000001
#define DIEDFL_FORCEFEEDBACK     0x00000100

#define DIDFT_AXIS               0x00000003

#define DIDC_FORCEFEEDBACK       0x00000100
#define DIDOBIFFECT              0x00000001
#define DIDOBCAPS                0x00000002
#define DIDOBCOMPOSITION         0x00000004

#define DIDOI_FFACTUATOR         0x00000001

#define DISCL_EXCLUSIVE          0x00000001
#define DISCL_NONEXCLUSIVE       0x00000002
#define DISCL_FOREGROUND         0x00000004
#define DISCL_BACKGROUND         0x00000008

#define DIEB_NOTRIGGER           0xFFFFFFFF

/* The data format object. Its CONTENTS are never read by anything: the host
 * passes &c_dfDIJoystick2 to SetDataFormat and moves on. What SetDataFormat
 * actually establishes is "this device reports in DIJOYSTATE2 terms", and that
 * is decided by the device, not by the format blob. */
typedef struct CdfJoystick2 {
    DWORD       dwSize;
    DWORD       dwObjSize;
    DWORD       dwFlags;
    DWORD       dwDataSize;
    DWORD       dwNumObjs;
} CdfJoystick2;

extern CdfJoystick2 c_dfDIJoystick2;

/* --- GUIDs, verbatim from the DirectInput headers --------------------------
 *
 * Do not "tidy" these. They are the saved-configuration key. */
#define DEFINE_GUID_NAME(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }

DEFINE_GUID_NAME(GUID_XAxis,     0xA36D02E0, 0xC9F3, 0x11CF,
                 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID_NAME(GUID_YAxis,     0xA36D02E1, 0xC9F3, 0x11CF,
                 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID_NAME(GUID_ZAxis,     0xA36D02E2, 0xC9F3, 0x11CF,
                 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID_NAME(GUID_RzAxis,    0xA36D02E3, 0xC9F3, 0x11CF,
                 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID_NAME(GUID_Slider,    0xA36D02E4, 0xC9F3, 0x11CF,
                 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID_NAME(GUID_RxAxis,    0xA36D02F4, 0xC9F3, 0x11CF,
                 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID_NAME(GUID_RyAxis,    0xA36D02F5, 0xC9F3, 0x11CF,
                 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);

DEFINE_GUID_NAME(GUID_ConstantForce, 0x13541C20, 0x8E33, 0x11D0,
                 0x9A, 0xD0, 0x00, 0xA0, 0xC9, 0xA0, 0x6E, 0x35);

DEFINE_GUID_NAME(IID_IDirectInput8A,  0xBF798030, 0x483A, 0x4DA2,
                 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00);

/* --- the implementation --------------------------------------------------- */

/* Every entry point below is reached through a macro rather than called
 * directly, mirroring how the Windows headers spell them. The macros are
 * below the declarations so the names cannot drift apart: a change to a
 * signature has to be made in one place, and the compiler checks both. */

/* DirectInput8Create. The host keeps this as a FUNCTION POINTER (it resolves
 * the real one out of dinput8.dll on Windows) and on Linux assigns
 * posix_di_create straight into that same slot. So this signature has to
 * match the export exactly -- not approximately. The two unused trailing
 * parameters are the aggregate-out and the unknown, which the real entry point
 * takes and this port has no use for.
 *
 * The iid is accepted and ignored beyond a sanity check: there is exactly one
 * DirectInput 8 ANSI interface to hand back, so the check exists to catch a
 * WIDE variant arriving here, which would mean an interface this port never
 * produces. */
HRESULT WINAPI posix_di_create(HINSTANCE self, DWORD version, REFIID iid,
                        void **out, void *unk);

HRESULT IDirectInput8_EnumDevices_impl(LPDIRECTINPUT8A self, DWORD devtype,
                                       LPDIENUMDEVICECALLBACKA cb, LPVOID ctx,
                                       DWORD flags);
HRESULT IDirectInput8_CreateDevice_impl(LPDIRECTINPUT8A self, REFGUID guid,
                                        LPDIRECTINPUTDEVICE8A *out,
                                        LPDIRECTINPUTDEVICE8A *dup);
HRESULT IDirectInput8_Release_impl(LPDIRECTINPUT8A self);

HRESULT IDirectInputDevice8_SetDataFormat_impl(LPDIRECTINPUTDEVICE8A self,
                                               LPCVOID format);
HRESULT IDirectInputDevice8_SetCooperativeLevel_impl(LPDIRECTINPUTDEVICE8A self,
                                                     HWND hwnd, DWORD flags);
HRESULT IDirectInputDevice8_Acquire_impl(LPDIRECTINPUTDEVICE8A self);
HRESULT IDirectInputDevice8_Unacquire_impl(LPDIRECTINPUTDEVICE8A self);
HRESULT IDirectInputDevice8_Poll_impl(LPDIRECTINPUTDEVICE8A self);
HRESULT IDirectInputDevice8_GetDeviceState_impl(LPDIRECTINPUTDEVICE8A self,
                                                 DWORD size, LPVOID state);
HRESULT IDirectInputDevice8_GetCapabilities_impl(LPDIRECTINPUTDEVICE8A self,
                                                 LPDIDEVCAPS caps);
HRESULT IDirectInputDevice8_EnumObjects_impl(LPDIRECTINPUTDEVICE8A self,
                                              LPDIENUMDEVICEOBJECTCALLBACKA cb,
                                              LPVOID ctx, DWORD flags);
HRESULT IDirectInputDevice8_GetProperty_impl(LPDIRECTINPUTDEVICE8A self,
                                             REFPROPID prop, LPDIPROPHEADER out);
HRESULT IDirectInputDevice8_SetProperty_impl(LPDIRECTINPUTDEVICE8A self,
                                             REFPROPID prop,
                                             LPCDIPROPHEADER value);
/* The fifth argument is DirectInput's "give me the same effect object again"
 * out-parameter. The host passes NULL for it; it is here because dropping a
 * parameter from a signature the real header declares is how a wrapper starts
 * silently discarding arguments. */
HRESULT IDirectInputDevice8_CreateEffect_impl(LPDIRECTINPUTDEVICE8A self,
                                              REFGUID kind, LPCDIEFFECT desc,
                                              LPDIRECTINPUTEFFECT *out,
                                              LPDIRECTINPUTEFFECT *dup);
HRESULT IDirectInputDevice8_Release_impl(LPDIRECTINPUTDEVICE8A self);

HRESULT IDirectInputEffect_Start_impl(LPDIRECTINPUTEFFECT self, DWORD flags,
                                      DWORD count);
HRESULT IDirectInputEffect_Stop_impl(LPDIRECTINPUTEFFECT self);
HRESULT IDirectInputEffect_SetParameters_impl(LPDIRECTINPUTEFFECT self,
                                              LPCDIEFFECT desc, DWORD flags);
HRESULT IDirectInputEffect_Release_impl(LPDIRECTINPUTEFFECT self);

/* --- the __wrapper macros -------------------------------------------------
 *
 * Same names, same argument order as the Windows headers, so an unmodified
 * dinput.c compiles against either. The `_impl` suffix on every target is
 * deliberate: it makes it impossible to confuse the macro with the function,
 * and greppable if one is ever missing. */
#define IDirectInput8_EnumDevices(self, dt, cb, ctx, fl) \
    IDirectInput8_EnumDevices_impl((self), (dt), (cb), (ctx), (fl))
#define IDirectInput8_CreateDevice(self, guid, out, dup) \
    IDirectInput8_CreateDevice_impl((self), (guid), (out), (dup))
#define IDirectInput8_Release(self) \
    IDirectInput8_Release_impl(self)

#define IDirectInputDevice8_SetDataFormat(self, fmt) \
    IDirectInputDevice8_SetDataFormat_impl((self), (fmt))
#define IDirectInputDevice8_SetCooperativeLevel(self, hwnd, fl) \
    IDirectInputDevice8_SetCooperativeLevel_impl((self), (hwnd), (fl))
#define IDirectInputDevice8_Acquire(self) \
    IDirectInputDevice8_Acquire_impl(self)
#define IDirectInputDevice8_Unacquire(self) \
    IDirectInputDevice8_Unacquire_impl(self)
#define IDirectInputDevice8_Poll(self) \
    IDirectInputDevice8_Poll_impl(self)
#define IDirectInputDevice8_GetDeviceState(self, sz, st) \
    IDirectInputDevice8_GetDeviceState_impl((self), (sz), (st))
#define IDirectInputDevice8_GetCapabilities(self, c) \
    IDirectInputDevice8_GetCapabilities_impl((self), (c))
#define IDirectInputDevice8_EnumObjects(self, cb, ctx, fl) \
    IDirectInputDevice8_EnumObjects_impl((self), (cb), (ctx), (fl))
#define IDirectInputDevice8_GetProperty(self, p, out) \
    IDirectInputDevice8_GetProperty_impl((self), (p), (out))
#define IDirectInputDevice8_SetProperty(self, p, v) \
    IDirectInputDevice8_SetProperty_impl((self), (p), (v))
#define IDirectInputDevice8_CreateEffect(self, kind, desc, out, dup) \
    IDirectInputDevice8_CreateEffect_impl((self), (kind), (desc), (out), (dup))
#define IDirectInputDevice8_Release(self) \
    IDirectInputDevice8_Release_impl(self)

#define IDirectInputEffect_Start(self, fl, cnt) \
    IDirectInputEffect_Start_impl((self), (fl), (cnt))
#define IDirectInputEffect_Stop(self) \
    IDirectInputEffect_Stop_impl(self)
#define IDirectInputEffect_SetParameters(self, d, fl) \
    IDirectInputEffect_SetParameters_impl((self), (d), (fl))
#define IDirectInputEffect_Release(self) \
    IDirectInputEffect_Release_impl(self)

#endif /* VCTH_DINPUT_POSIX_H */
