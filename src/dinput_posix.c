/* dinput_posix.c -- the DirectInput surface on SDL3, for 32-bit Linux.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Implements what src/dinput_posix.h declares. The shape of the problem is
 * unusual: DirectInput enumerates a device's axis OBJECTS and addresses their
 * data by a GUID, while SDL presents a gamepad as a fixed set of abstract
 * controls (or, at the joystick layer, an ordered list of raw axes). This file
 * is the translation, and the interesting part is that the translation has to
 * be LOSSLESS ENOUGH that the host's existing axis-canonicalisation fix keeps
 * working.
 *
 * What the host does with it: src/di_canon.h resolves an axis by guidType
 * rather than by the offset the driver reported, because on a G29, a G923 and a
 * Pro DD the reported offset is refused and the GUID-derived one accepted. So
 * the GUIDs this file synthesises are not cosmetic -- they are the axis
 * identity the whole configuration is written against, and a user's saved ini
 * names axes by them.
 *
 * THE AXIS ORDERING IS A DELIBERATE CHOICE, and it is the one thing here that
 * cannot be derived from the hardware. Linux evdev reports a racing wheel's
 * axes in whatever order its driver emits them, which is not a standard anyone
 * controls. So raw axis N is offered as the Nth DirectInput axis in the
 * canonical order (X, Y, Z, Rx, Ry, Rz, then sliders) -- the same order
 * DirectInput itself would use for a device that does report cleanly -- and if
 * a particular wheel disagrees, the ini's [analog] remap is the thing that
 * corrects it. That remap already exists and is what a player uses on Windows
 * to fix exactly this class of mismatch, so a wrong-but-fixable assignment here
 * lands the user in the same place Windows does rather than somewhere new.
 *
 * SCALING IS THE OTHER SUBTLE PART. unit_axis() in src/dinput.c normalises an
 * axis against the range the DEVICE REPORTS -- the lMin/lMax the host itself
 * wrote with DIPROP_RANGE -- not against SDL's -32768..32767. So this file
 * remembers that range per axis and scales into it, rather than dumping SDL's
 * raw numbers into DIJOYSTATE2 and letting the normalisation divide by the
 * wrong span.
 *
 * KNOWN DIVERGENCE, uncentred axes. A throttle or brake that rests at one end
 * is mapped linearly onto [lMin, lMax], which puts its rest position at the
 * midpoint. DirectInput on Windows distinguishes these with the axis's
 * physical properties, and nothing here can: SDL reports one number per axis
 * with no description of what the ends mean. A centred axis -- steering, and
 * every stick -- is unaffected and exact.
 */
#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

#include "dinput_posix.h"

/* DIJOYSTATE2 has two slider slots and DirectInput has no canonical offset for
 * a third. di_canon.h returns DI_CANON_NONE for it, which is the correct
 * answer: answering with a second slider's offset would point a force effect
 * at an axis it is not driving. */
#define DI_POSIX_MAX_AXES 8

CdfJoystick2 c_dfDIJoystick2 = { sizeof(CdfJoystick2),
                                 sizeof(DIJOYSTATE2),
                                 0,
                                 sizeof(DIJOYSTATE2),
                                 0 };

/* --- objects -------------------------------------------------------------- */

typedef struct axis_state {
    int      present;
    unsigned sdl_index;      /* which SDL axis feeds this one */
    DWORD    guid_offset;    /* canonical DIJOFS_* this was offered as */
    LONG     lmin, lmax;     /* the range the host asked for */
    LONG     deadzone;
    LONG     saturation;
    int      is_ffb_actuator;
} axis_state;

struct posix_di {
    SDL_JoystickID *ids;
    unsigned        count;
    int             sdl_started;
};

struct posix_di_device {
    SDL_JoystickID id;
    SDL_Joystick  *joy;
    SDL_Haptic    *haptic;
    int            acquired;
    int            ffb;
    unsigned       navail;
    unsigned       nbuttons;
    axis_state     axis[DI_POSIX_MAX_AXES];
    DWORD          ffb_gain;
    DWORD          autocenter;
};

struct posix_di_effect {
    SDL_HapticEffect effect;
    int              started;
};

/* --- helpers -------------------------------------------------------------- */

/* FNV-1a, folded into 16 bytes. Used where SDL has no stable identity of the
 * right shape and one has to be manufactured that survives a reboot -- see the
 * guidInstance note below. */
static void guid_from_string(const char *s, GUID *out)
{
    unsigned char *p = (unsigned char *)out;
    unsigned h = 2166136261u;
    int i;

    memset(out, 0, sizeof *out);
    if (!s)
        s = "";
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    /* Sixteen bytes of one well-mixed hash. This is an identity, not a
     * checksum -- it is never verified against anything, only compared. */
    for (i = 0; i < 16; i++) {
        h ^= h >> 13;
        h *= 2654435761u;
        p[i] = (unsigned char)(h >> 16);
    }
}

/* Which canonical DirectInput axis raw axis N is offered as.
 *
 * The order is the canonical one and is documented above: it is a convention,
 * not a measurement, and the ini's [analog] remap is what corrects a wheel that
 * disagrees. Returning DI_CANON_NONE past the eighth axis stops di_canon.h
 * pointing it at a slider slot that is already taken. */
static DWORD guid_offset_for_axis(unsigned n)
{
    switch (n) {
    case 0:  return DIJOFS_X;
    case 1:  return DIJOFS_Y;
    case 2:  return DIJOFS_Z;
    case 3:  return DIJOFS_RX;
    case 4:  return DIJOFS_RY;
    case 5:  return DIJOFS_RZ;
    case 6:  return (DWORD)DIJOFS_SLIDER(0);
    case 7:  return (DWORD)DIJOFS_SLIDER(1);
    default: return (DWORD)-1;
    }
}

/* The GUID that goes with a canonical offset -- the inverse of what
 * di_canonical_offset() computes, so an axis offered at DIJOFS_RX resolves
 * back to GUID_RxAxis and the host's own fix finds it. */
static const GUID *guid_for_offset(DWORD ofs)
{
    static const GUID none = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

    if (ofs == DIJOFS_X)  return &GUID_XAxis;
    if (ofs == DIJOFS_Y)  return &GUID_YAxis;
    if (ofs == DIJOFS_Z)  return &GUID_ZAxis;
    if (ofs == DIJOFS_RX) return &GUID_RxAxis;
    if (ofs == DIJOFS_RY) return &GUID_RyAxis;
    if (ofs == DIJOFS_RZ) return &GUID_RzAxis;
    if (ofs == (DWORD)DIJOFS_SLIDER(0)) return &GUID_Slider;
    if (ofs == (DWORD)DIJOFS_SLIDER(1)) return &GUID_Slider;
    return &none;
}

static axis_state *axis_by_offset(struct posix_di_device *dev, DWORD ofs,
                                  unsigned enumeration_index)
{
    /* diph.dwObj addresses an axis by its OBJECT ID (dinput.c sets it to
     * obj->dwType, the DirectInput type code), and the host also writes
     * offsets. Accept either: match the canonical offset when it is one this
     * device offers, and fall back to the enumeration index, which is the same
     * thing for every axis a real wheel has. */
    unsigned i;

    for (i = 0; i < dev->navail && i < DI_POSIX_MAX_AXES; i++)
        if (dev->axis[i].present && dev->axis[i].guid_offset == ofs)
            return &dev->axis[i];
    (void)enumeration_index;
    for (i = 0; i < dev->navail && i < DI_POSIX_MAX_AXES; i++)
        if (dev->axis[i].present)
            return &dev->axis[i];
    return NULL;
}

/* Scale SDL's -32768..32767 onto the axis's declared range. This is the step
 * that makes unit_axis() in the host come out right; see the file header. */
static LONG scale_axis(Sint16 raw, LONG lmin, LONG lmax)
{
    double span = (double)(lmax - lmin);
    double t = ((double)raw + 32768.0) / 65535.0;

    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return (LONG)((double)lmin + t * span);
}

/* --- DirectInput8Create ---------------------------------------------------- */

HRESULT WINAPI posix_di_create(HINSTANCE self, DWORD version, REFIID iid,
                        void **out, void *unk)
{
    struct posix_di *di;

    (void)self;
    (void)unk;

    /* The only interface this port produces is the ANSI DirectInput 8 one.
     * Rejecting a wide-variant iid here is not defensive noise: it would mean
     * a caller is about to use WCHAR interfaces this implementation never
     * builds, and returning a handle that cannot satisfy them would corrupt
     * data instead of failing. */
    if (iid && !IsEqualGUID(iid, &IID_IDirectInput8A))
        return E_INVALIDARG;
    if (version != DIRECTINPUT_VERSION)
        return E_INVALIDARG;
    if (!out)
        return E_INVALIDARG;

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK |
                           SDL_INIT_HAPTIC)) {
        *out = NULL;
        return E_FAIL;
    }

    di = calloc(1, sizeof *di);
    if (!di) {
        *out = NULL;
        return E_OUTOFMEMORY;
    }
    di->sdl_started = 1;
    *out = di;
    return S_OK;
}

/* --- IDirectInput8 --------------------------------------------------------- */

/* Fill in one device's identity. guidProduct is the device MODEL, taken from
 * SDL's own joystick GUID bytes -- that is real hardware identity and is stable
 * across reboots. guidInstance is where that specific unit is plugged in, and
 * has to be manufactured: SDL's instance id is per-session and would give every
 * boot a different device identity, which would silently invalidate every
 * saved ini. The path is the closest stable thing SDL offers. */
static void fill_instance(DIDEVICEINSTANCEA *inst, SDL_JoystickID id)
{
    SDL_GUID guid;
    const char *path;
    const char *name;

    memset(inst, 0, sizeof *inst);
    inst->dwSize = sizeof *inst;
    inst->dwDevType = DI8DEVCLASS_GAMECTRL;
    inst->wUsagePage = 0x01;          /* generic desktop controls */
    inst->wUsage = 0x04;              /* joystick */

    guid = SDL_GetJoystickGUIDForID(id);
    memcpy(&inst->guidProduct, guid.data, sizeof inst->guidProduct);

    path = SDL_GetJoystickPathForID(id);
    name = SDL_GetJoystickNameForID(id);
    if (!path)
        path = name;
    guid_from_string(path, &inst->guidInstance);

    snprintf(inst->tszInstanceName, sizeof inst->tszInstanceName, "%s",
             path ? path : "");
    snprintf(inst->tszProductName, sizeof inst->tszProductName, "%s",
             name ? name : "");
}

/* Whether this device has a motor. SDL has no cheap query for it -- the
 * haptics subsystem enumerates independently of the joystick list -- so the
 * joystick is opened, asked, and closed again. That is the price of answering
 * DIEDFL_FORCEFEEDBACK honestly; the alternative is to report every pad as
 * having a motor, which would put a non-FFB device into the force-feedback
 * path and then silently give the player no force. */
static int device_has_ffb(SDL_JoystickID id)
{
    SDL_Joystick *joy = SDL_OpenJoystick(id);
    int ffb;

    if (!joy)
        return 0;
    ffb = SDL_OpenHapticFromJoystick(joy) != NULL;
    SDL_CloseJoystick(joy);
    return ffb;
}

HRESULT IDirectInput8_EnumDevices_impl(LPDIRECTINPUT8A self, DWORD devtype,
                                       LPDIENUMDEVICECALLBACKA cb, LPVOID ctx,
                                       DWORD flags)
{
    SDL_JoystickID *ids;
    int count = 0, i;

    if (!self || !cb)
        return E_INVALIDARG;
    if (devtype != DI8DEVCLASS_GAMECTRL)
        return S_OK;

    SDL_UpdateGamepads();
    ids = SDL_GetJoysticks(&count);
    if (!ids)
        return S_OK;                 /* no devices is not an error */

    for (i = 0; i < count; i++) {
        DIDEVICEINSTANCEA inst;
        BOOL keep;

        /* The host enumerates twice: once for everything, then again with
         * DIEDFL_FORCEFEEDBACK to learn which of those have a motor. The
         * second pass must be a SUBSET of the first -- enum_ffb_cb() matches
         * by guidInstance against what the first pass recorded, so reporting a
         * device here that the first pass did not would mark a device that
         * does not exist. */
        if ((flags & DIEDFL_FORCEFEEDBACK) && !device_has_ffb(ids[i]))
            continue;

        fill_instance(&inst, ids[i]);
        keep = cb(&inst, ctx);
        if (!keep)
            break;
    }
    SDL_free(ids);
    return S_OK;
}

/* Find the joystick whose manufactured instance GUID matches. The host passes
 * back exactly the guidInstance fill_instance() handed it, so recomputing each
 * path and comparing is the reliable way round -- SDL offers no reverse GUID
 * lookup, and an instance id is per-session and would not survive a reboot
 * anyway. */
static SDL_JoystickID find_by_instance_guid(REFGUID want)
{
    SDL_JoystickID *ids;
    int count = 0, i;

    SDL_UpdateGamepads();
    ids = SDL_GetJoysticks(&count);
    if (!ids)
        return 0;
    for (i = 0; i < count; i++) {
        DIDEVICEINSTANCEA inst;
        fill_instance(&inst, ids[i]);
        if (IsEqualGUID(&inst.guidInstance, want)) {
            SDL_JoystickID id = ids[i];
            SDL_free(ids);
            return id;
        }
    }
    SDL_free(ids);
    return 0;
}

HRESULT IDirectInput8_CreateDevice_impl(LPDIRECTINPUT8A self, REFGUID guid,
                                        LPDIRECTINPUTDEVICE8A *out,
                                        LPDIRECTINPUTDEVICE8A *dup)
{
    struct posix_di_device *dev;
    SDL_JoystickID id;

    (void)self;
    (void)dup;
    if (!guid || !out)
        return E_INVALIDARG;
    *out = NULL;

    id = find_by_instance_guid(guid);
    if (!id)
        return E_INVALIDARG;

    dev = calloc(1, sizeof *dev);
    if (!dev)
        return E_OUTOFMEMORY;
    dev->id = id;
    dev->joy = SDL_OpenJoystick(id);
    if (!dev->joy) {
        free(dev);
        return E_FAIL;
    }
    dev->haptic = SDL_OpenHapticFromJoystick(dev->joy);
    dev->ffb = dev->haptic != NULL;
    dev->ffb_gain = DI_FFNOMINALMAX;
    dev->autocenter = DIPROPAUTOCENTER_ON;
    dev->navail = SDL_GetNumJoystickAxes(dev->joy);
    dev->nbuttons = SDL_GetNumJoystickButtons(dev->joy);
    if (dev->navail > DI_POSIX_MAX_AXES)
        dev->navail = DI_POSIX_MAX_AXES;
    *out = dev;
    return S_OK;
}

HRESULT IDirectInput8_Release_impl(LPDIRECTINPUT8A self)
{
    if (!self)
        return E_INVALIDARG;
    free(self->ids);
    free(self);
    return S_OK;
}

/* --- IDirectInputDevice8 --------------------------------------------------- */

HRESULT IDirectInputDevice8_SetDataFormat_impl(LPDIRECTINPUTDEVICE8A self,
                                               LPCVOID format)
{
    /* Accepted and ignored. The format blob is never read; what this call
     * actually establishes on Windows is "report in DIJOYSTATE2 terms", and
     * that is decided by the device here, which always fills DIJOYSTATE2. */
    (void)self;
    (void)format;
    return S_OK;
}

HRESULT IDirectInputDevice8_SetCooperativeLevel_impl(LPDIRECTINPUTDEVICE8A self,
                                                     HWND hwnd, DWORD flags)
{
    /* Succeeds unconditionally, including for DISCL_EXCLUSIVE.
     *
     * That is not a shortcut: there is nothing to be exclusive OF. SDL's
     * gamepad and joystick backends open their own descriptors, and no SDL
     * entry point offers exclusive access, so refusing the exclusive request
     * would only push the host onto the non-exclusive path it already takes
     * whenever the exclusive one fails. The consequence is stated in
     * src/dinput.c's create_owner_window(): on Linux a pad can be held by two
     * programs at once. */
    (void)self;
    (void)hwnd;
    (void)flags;
    return S_OK;
}

HRESULT IDirectInputDevice8_Acquire_impl(LPDIRECTINPUTDEVICE8A self)
{
    if (!self)
        return E_INVALIDARG;
    if (!self->joy)
        return E_FAIL;
    self->acquired = 1;
    return S_OK;
}

HRESULT IDirectInputDevice8_Unacquire_impl(LPDIRECTINPUTDEVICE8A self)
{
    if (!self)
        return E_INVALIDARG;
    self->acquired = 0;
    return S_OK;
}

HRESULT IDirectInputDevice8_Poll_impl(LPDIRECTINPUTDEVICE8A self)
{
    if (!self)
        return E_INVALIDARG;
    SDL_UpdateGamepads();
    return self->joy ? S_OK : E_FAIL;
}

HRESULT IDirectInputDevice8_GetDeviceState_impl(LPDIRECTINPUTDEVICE8A self,
                                                 DWORD size, LPVOID state)
{
    DIJOYSTATE2 *js = (DIJOYSTATE2 *)state;
    unsigned i;

    if (!self || !js)
        return E_INVALIDARG;
    if (size < sizeof *js)
        return E_INVALIDARG;
    /* An unacquired DirectInput device reads every button as released rather
     * than failing. The host relies on that to distinguish "no device" from
     * "device went away", so reproduce it rather than reporting an error. */
    if (!self->acquired || !self->joy) {
        memset(js, 0, sizeof *js);
        return S_OK;
    }

    SDL_UpdateGamepads();

    memset(js, 0, sizeof *js);
    for (i = 0; i < self->navail; i++) {
        const axis_state *ax = &self->axis[i];
        Sint16 raw;
        LONG value;

        if (!ax->present || ax->guid_offset == (DWORD)-1)
            continue;
        raw = SDL_GetJoystickAxis(self->joy, (int)ax->sdl_index);
        value = scale_axis(raw, ax->lmin, ax->lmax);
        switch (ax->guid_offset) {
        case DIJOFS_X:  js->lX = value;  break;
        case DIJOFS_Y:  js->lY = value;  break;
        case DIJOFS_Z:  js->lZ = value;  break;
        case DIJOFS_RX: js->lRx = value; break;
        case DIJOFS_RY: js->lRy = value; break;
        case DIJOFS_RZ: js->lRz = value; break;
        default:
            if (ax->guid_offset == (DWORD)DIJOFS_SLIDER(0))
                js->rglSlider[0] = value;
            else if (ax->guid_offset == (DWORD)DIJOFS_SLIDER(1))
                js->rglSlider[1] = value;
            break;
        }
    }

    for (i = 0; i < self->nbuttons && i < sizeof js->rgbButtons; i++)
        js->rgbButtons[i] = SDL_GetJoystickButton(self->joy, (int)i) ? 0x80 : 0x00;

    /* An unpressed hat is DIJOYSTATE2's documented -1 (0xFFFFFFFF), not 0 --
     * pov_digital() in the host distinguishes the two, and 0 would read as a
     * hat hard over north. */
    for (i = 0; i < 4; i++)
        js->rgdwPOV[i] = 0xFFFFFFFFu;

    return S_OK;
}

HRESULT IDirectInputDevice8_GetCapabilities_impl(LPDIRECTINPUTDEVICE8A self,
                                                 LPDIDEVCAPS caps)
{
    if (!self || !caps)
        return E_INVALIDARG;
    memset(caps, 0, sizeof *caps);
    caps->dwSize = sizeof *caps;
    caps->dwDevType = DI8DEVCLASS_GAMECTRL;
    caps->dwAxes = self->navail;
    caps->dwButtons = self->nbuttons;
    caps->dwPOVs = 0;
    caps->dwFlags = self->ffb ? DIDC_FORCEFEEDBACK : 0;
    return S_OK;
}

HRESULT IDirectInputDevice8_EnumObjects_impl(LPDIRECTINPUTDEVICE8A self,
                                              LPDIENUMDEVICEOBJECTCALLBACKA cb,
                                              LPVOID ctx, DWORD flags)
{
    unsigned i;
    unsigned slider_seen = 0;

    if (!self || !cb)
        return E_INVALIDARG;

    /* Axis objects only. The host asks for DIDFT_AXIS and nothing else, and
     * buttons have no GUID-addressed data for it to resolve. */
    if ((flags & DIDFT_AXIS) != DIDFT_AXIS)
        return S_OK;

    for (i = 0; i < self->navail; i++) {
        DIDEVICEOBJECTINSTANCEA obj;
        DWORD canon = guid_offset_for_axis(i);
        const GUID *g;
        BOOL keep;

        memset(&obj, 0, sizeof obj);
        obj.dwSize = sizeof obj;
        obj.dwOfs = canon == (DWORD)-1 ? 0 : canon;
        obj.dwType = DIDFT_AXIS;
        obj.dwFlags = DIDFT_AXIS;
        obj.wUsagePage = 0x01;
        obj.wUsage = 0x04;

        g = guid_for_offset(canon);
        obj.guidType = *g;

        /* A wheel's motor is DirectInput's FFACTUATOR axis. Convention puts it
         * on Z, and that is what the host's own correction assumes: a G29 and a
         * Pro DD both refuse Z for the actuator and accept the canonical
         * offset instead. Marking the same axis here keeps that fix live on
         * Linux rather than silently bypassed. */
        if (self->ffb && i == 2) {
            obj.dwFlags |= DIDOI_FFACTUATOR;
            obj.dwFFMaxForce = DI_FFNOMINALMAX;
        }

        self->axis[i].present = 1;
        self->axis[i].sdl_index = i;
        self->axis[i].guid_offset = canon;
        if (self->axis[i].lmax == 0 && self->axis[i].lmin == 0) {
            /* Until the host writes a range, a centred DirectInput axis. The
             * host always writes one before reading; this only affects a
             * device enumerated but never configured. */
            self->axis[i].lmin = -DI_FFNOMINALMAX;
            self->axis[i].lmax = DI_FFNOMINALMAX;
        }
        (void)slider_seen;

        snprintf(obj.tszName, sizeof obj.tszName, "Axis%u", i);

        keep = cb(&obj, ctx);
        if (!keep)
            break;
    }
    return S_OK;
}

HRESULT IDirectInputDevice8_GetProperty_impl(LPDIRECTINPUTDEVICE8A self,
                                             REFPROPID prop, LPDIPROPHEADER out)
{
    DWORD id = (DWORD)(size_t)prop;
    axis_state *ax;
    DIPROPDWORD *dw = (DIPROPDWORD *)out;
    DIPROPRANGE *range = (DIPROPRANGE *)out;

    if (!self || !out)
        return E_INVALIDARG;

    ax = axis_by_offset(self, out->dwObj, 0);
    if (!ax)
        return E_INVALIDARG;

    switch (id) {
    case 4:                            /* DIPROP_RANGE */
        range->lMin = ax->lmin;
        range->lMax = ax->lmax;
        return S_OK;
    case 5:                            /* DIPROP_DEADZONE */
        dw->dwData = (DWORD)ax->deadzone;
        return S_OK;
    case 6:                            /* DIPROP_SATURATION */
        dw->dwData = (DWORD)ax->saturation;
        return S_OK;
    case 7:                            /* DIPROP_FFGAIN */
        dw->dwData = self->ffb_gain;
        return S_OK;
    case 9:                            /* DIPROP_AUTOCENTER */
        dw->dwData = self->autocenter;
        return S_OK;
    default:
        return E_INVALIDARG;
    }
}

HRESULT IDirectInputDevice8_SetProperty_impl(LPDIRECTINPUTDEVICE8A self,
                                             REFPROPID prop,
                                             LPCDIPROPHEADER value)
{
    DWORD id = (DWORD)(size_t)prop;
    const DIPROPDWORD *dw = (const DIPROPDWORD *)value;
    const DIPROPRANGE *range = (const DIPROPRANGE *)value;
    axis_state *ax;

    if (!self || !value)
        return E_INVALIDARG;
    ax = axis_by_offset(self, value->dwObj, 0);

    switch (id) {
    case 4:                            /* DIPROP_RANGE */
        if (!ax)
            return E_INVALIDARG;
        ax->lmin = range->lMin;
        ax->lmax = range->lMax;
        return S_OK;
    case 5:                            /* DIPROP_DEADZONE */
        if (ax) ax->deadzone = (LONG)dw->dwData;
        return S_OK;
    case 6:                            /* DIPROP_SATURATION */
        if (ax) ax->saturation = (LONG)dw->dwData;
        return S_OK;
    case 7:                            /* DIPROP_FFGAIN */
        self->ffb_gain = dw->dwData;
        return S_OK;
    case 9:                            /* DIPROP_AUTOCENTER */
        self->autocenter = dw->dwData;
        return S_OK;
    default:
        return E_INVALIDARG;
    }
}

HRESULT IDirectInputDevice8_CreateEffect_impl(LPDIRECTINPUTDEVICE8A self,
                                              REFGUID kind, LPCDIEFFECT desc,
                                              LPDIRECTINPUTEFFECT *out,
                                              LPDIRECTINPUTEFFECT *dup)
{
    struct posix_di_effect *eff;

    (void)dup;
    if (!self || !kind || !desc || !out)
        return E_INVALIDARG;
    *out = NULL;

    /* Only ConstantForce. The host creates nothing else (src/dinput.c passes
     * GUID_ConstantForce and nothing else), so this is the whole effect
     * surface rather than a shortcut. */
    if (!IsEqualGUID(kind, &GUID_ConstantForce))
        return E_INVALIDARG;
    if (!self->haptic)
        return E_INVALIDARG;           /* the device has no motor */

    eff = calloc(1, sizeof *eff);
    if (!eff)
        return E_OUTOFMEMORY;

    /* SDL discriminates the union on effect.type, so it is set as well as the
     * inner type -- leaving the inner one unset is how an effect runs with no
     * parameters at all. */
    eff->effect.type = SDL_HAPTIC_CONSTANT;
    eff->effect.constant.type = SDL_HAPTIC_CONSTANT;
    eff->effect.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    /* -1..+1 along the motor. The host builds every effect on one axis with
     * one direction, so the other two stay zero rather than being invented. */
    eff->effect.constant.direction.dir[0] = 0;
    eff->effect.constant.direction.dir[1] = 0;
    eff->effect.constant.direction.dir[2] = 0;
    eff->effect.constant.length = SDL_HAPTIC_INFINITY;
    eff->effect.constant.delay = 0;
    eff->effect.constant.button = 0;
    eff->effect.constant.interval = 0;
    eff->effect.constant.level = 0;
    eff->effect.constant.attack_length = 0;
    eff->effect.constant.attack_level = 0;
    eff->effect.constant.fade_length = 0;
    eff->effect.constant.fade_level = 0;

    if (desc->lpvTypeSpecificParams) {
        const DICONSTANTFORCE *cf =
            (const DICONSTANTFORCE *)desc->lpvTypeSpecificParams;
        /* DI_FFNOMINALMAX is DirectInput's full-scale magnitude and the host
         * scales to it, so the gain is applied here and NOT again when the
         * parameters are set -- applying both is the classic way to get a
         * motor that runs at quarter strength. */
        eff->effect.constant.level =
            (Sint16)((double)cf->lMagnitude / DI_FFNOMINALMAX * 32767.0);
    }

    *out = eff;
    return S_OK;
}

HRESULT IDirectInputDevice8_Release_impl(LPDIRECTINPUTDEVICE8A self)
{
    if (!self)
        return E_INVALIDARG;
    if (self->haptic)
        SDL_CloseHaptic(self->haptic);
    if (self->joy)
        SDL_CloseJoystick(self->joy);
    free(self);
    return S_OK;
}

/* --- IDirectInputEffect ---------------------------------------------------- */

HRESULT IDirectInputEffect_Start_impl(LPDIRECTINPUTEFFECT self, DWORD flags,
                                      DWORD count)
{
    (void)flags;
    (void)count;
    if (!self)
        return E_INVALIDARG;
    self->started = 1;
    return S_OK;
}

HRESULT IDirectInputEffect_Stop_impl(LPDIRECTINPUTEFFECT self)
{
    if (!self)
        return E_INVALIDARG;
    self->started = 0;
    return S_OK;
}

HRESULT IDirectInputEffect_SetParameters_impl(LPDIRECTINPUTEFFECT self,
                                              LPCDIEFFECT desc, DWORD flags)
{
    const DICONSTANTFORCE *cf;

    (void)flags;
    if (!self || !desc || !desc->lpvTypeSpecificParams)
        return E_INVALIDARG;
    cf = (const DICONSTANTFORCE *)desc->lpvTypeSpecificParams;
    self->effect.constant.level =
        (Sint16)((double)cf->lMagnitude / DI_FFNOMINALMAX * 32767.0);
    return S_OK;
}

HRESULT IDirectInputEffect_Release_impl(LPDIRECTINPUTEFFECT self)
{
    if (!self)
        return E_INVALIDARG;
    free(self);
    return S_OK;
}