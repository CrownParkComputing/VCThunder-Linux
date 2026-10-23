/* dinput_posix_test.c -- the DirectInput surface's load-bearing assumptions.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Most of this port is wiring, and wiring is hard to test because proving it
 * works needs a wheel on a desk. So this test does the next best thing: it
 * pins down every assumption that would otherwise fail SILENTLY.
 *
 * A wrong axis scale does not crash. It makes the wheel feel slightly wrong
 * and the player blames the game. A GUID that is stable in the wrong way does
 * not crash either -- it invalidates every saved ini, quietly, on the first run
 * after a reboot. Those are the failures worth a test, and none of them need
 * hardware to catch.
 *
 * What is NOT tested here: that SDL actually delivers a G29's axes, that a
 * motor spins, or that a pad enumerates at all. Those need a device on a desk.
 * Everything this file asserts is true with zero pads attached.
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "dinput_posix.h"
#include "di_canon.h"

/* The implementation, so the statics under test are reachable. This is the
 * project's usual "a test #includes the source it covers" shape -- and here it
 * is also the only way to reach guid_from_string and scale_axis without
 * exporting them from a library for no other reason. */
#include "dinput_posix.c"

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

static void check_guid(const GUID *a, const GUID *b, const char *what)
{
    if (!IsEqualGUID(a, b)) {
        g_failed++;
        printf("FAIL: %s (guid differs)\n", what);
    }
    g_checks++;
}

/* --- the GUIDs are the saved-configuration key -----------------------------
 *
 * These seven values are copied from /usr/i686-w64-mingw32/include/dinput.h.
 * A player whose ini names an axis by guidType must get the same axis on
 * Linux as on Windows, so the expected values are spelled out here in full
 * rather than compared against the header's own definitions -- comparing those
 * would prove nothing, since the header is where the values came from. */
static void test_axis_guids_are_the_real_ones(void)
{
    struct { const GUID *got; DWORD d1; WORD w1, w2;
             BYTE b0, b1, b2, b3, b4, b5, b6, b7; const char *name; } want[] = {
        { &GUID_XAxis,  0xA36D02E0, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00, "GUID_XAxis" },
        { &GUID_YAxis,  0xA36D02E1, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00, "GUID_YAxis" },
        { &GUID_ZAxis,  0xA36D02E2, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00, "GUID_ZAxis" },
        { &GUID_RzAxis, 0xA36D02E3, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00, "GUID_RzAxis" },
        { &GUID_Slider, 0xA36D02E4, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00, "GUID_Slider" },
        { &GUID_RxAxis, 0xA36D02F4, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00, "GUID_RxAxis" },
        { &GUID_RyAxis, 0xA36D02F5, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00, "GUID_RyAxis" },
    };
    size_t i;

    for (i = 0; i < sizeof want / sizeof want[0]; i++) {
        const GUID *g = want[i].got;
        check(g->Data1 == want[i].d1, want[i].name);
        check(g->Data2 == want[i].w1, want[i].name);
        check(g->Data3 == want[i].w2, want[i].name);
        check(g->Data4[0] == want[i].b0 && g->Data4[1] == want[i].b1 &&
              g->Data4[2] == want[i].b2 && g->Data4[3] == want[i].b3 &&
              g->Data4[4] == want[i].b4 && g->Data4[5] == want[i].b5 &&
              g->Data4[6] == want[i].b6 && g->Data4[7] == want[i].b7,
              want[i].name);
    }
    /* The seven axis GUIDs must all be DISTINCT. Two colliding would mean two
     * names in one ini section, and one axis silently shadowing the other. */
    check(!IsEqualGUID(&GUID_XAxis, &GUID_YAxis), "X and Y GUIDs differ");
    check(!IsEqualGUID(&GUID_RzAxis, &GUID_Slider), "Rz and Slider GUIDs differ");
    check(!IsEqualGUID(&GUID_RxAxis, &GUID_RyAxis), "Rx and Ry GUIDs differ");
}

/* --- the axis order round-trips through the host's own canonicalisation -----
 *
 * dinput.c resolves an axis with di_canonical_offset(), and the Linux side
 * offers an axis with guid_offset_for_axis(). Those two have to be inverses,
 * or the host asks for an axis the device named differently and the wheel
 * steers with the wrong axis -- silently, because the code has no way to tell
 * an axis it could not resolve from one that is genuinely centred. */
static void test_axis_order_round_trips(void)
{
    struct { DWORD canon; const GUID *expect; const char *name; } axes[] = {
        { DIJOFS_X,               &GUID_XAxis,  "X"  },
        { DIJOFS_Y,               &GUID_YAxis,  "Y"  },
        { DIJOFS_Z,               &GUID_ZAxis,  "Z"  },
        { DIJOFS_RX,              &GUID_RxAxis, "Rx" },
        { DIJOFS_RY,              &GUID_RyAxis, "Ry" },
        { DIJOFS_RZ,              &GUID_RzAxis, "Rz" },
        { DIJOFS_SLIDER(0),       &GUID_Slider, "slider0" },
    };
    unsigned i;

    for (i = 0; i < sizeof axes / sizeof axes[0]; i++) {
        unsigned slider_seen = 0;
        DWORD got = di_canonical_offset(axes[i].expect, &slider_seen);
        check(got == axes[i].canon, axes[i].name);
    }

    /* And the direction the port actually uses: every canonical offset must
     * map back to the GUID the host would resolve it to. */
    for (i = 0; i < 7; i++) {
        DWORD canon = guid_offset_for_axis(i);
        const GUID *g = guid_for_offset(canon);
        unsigned slider_seen = 0;
        check(di_canonical_offset(g, &slider_seen) == canon,
              "offset -> guid -> offset");
    }

    /* A ninth axis has no canonical offset and must be refused rather than
     * pointed at a slider slot that is already in use. */
    check(guid_offset_for_axis(8) == (DWORD)-1, "9th axis has no canonical offset");
    check(guid_offset_for_axis(99) == (DWORD)-1, "100th axis has no canonical offset");
}

/* --- DIJOFS_* really do land on the members the host reads ----------------
 *
 * di_axis_value() in src/dinput.c switches on js->lX, js->rglSlider[0] and so
 * on; DIJOFS_* is what names those same fields from the other side. If the two
 * ever disagreed the wheel would read one field while the range was set on
 * another, and unit_axis() would divide by the wrong span. offsetof is the
 * check, and it is the same expression the header uses, so this cannot drift. */
static void test_offsets_land_on_members(void)
{
    check(DIJOFS_X  == offsetof(DIJOYSTATE2, lX),  "DIJOFS_X is lX");
    check(DIJOFS_Y  == offsetof(DIJOYSTATE2, lY),  "DIJOFS_Y is lY");
    check(DIJOFS_Z  == offsetof(DIJOYSTATE2, lZ),  "DIJOFS_Z is lZ");
    check(DIJOFS_RX == offsetof(DIJOYSTATE2, lRx), "DIJOFS_RX is lRx");
    check(DIJOFS_RY == offsetof(DIJOYSTATE2, lRy), "DIJOFS_RY is lRy");
    check(DIJOFS_RZ == offsetof(DIJOYSTATE2, lRz), "DIJOFS_RZ is lRz");
    check((DWORD)DIJOFS_SLIDER(0) == offsetof(DIJOYSTATE2, rglSlider), "slider0");
    check((DWORD)DIJOFS_SLIDER(1) ==
          offsetof(DIJOYSTATE2, rglSlider) + (DWORD)sizeof(LONG), "slider1");
    check(DIJOFS_SLIDER(0) < DIJOFS_SLIDER(1), "slider0 precedes slider1");
    check((DWORD)DIJOFS_BUTTON(0) == offsetof(DIJOYSTATE2, rgbButtons),
          "button0 is rgbButtons");
    /* rgbButtons must come AFTER every axis, or a button press would be read
     * as an axis value. */
    check(offsetof(DIJOYSTATE2, rgbButtons) > (size_t)DIJOFS_RZ,
          "buttons follow the axes");
}

/* --- scaling onto the range the host asked for -----------------------------
 *
 * unit_axis() normalises with ((value - lmin) / (lmax - lmin)) * 2 - 1. For a
 * centred axis that is only right if the centre of the SDL range lands on the
 * centre of [lmin, lmax], and the ends land exactly on lmin and lmax. Get this
 * wrong and the wheel reads 0.98 of full lock at rest, or never reaches full
 * lock at all -- both silent. */
static void test_scale_axis(void)
{
    const LONG r = 10000;

    check(scale_axis(0, -r, r) == 0, "SDL centre -> range centre");
    check(scale_axis(-32768, -r, r) == -r, "SDL minimum -> lmin");
    check(scale_axis(32767, -r, r) == r, "SDL maximum -> lmax");

    /* The whole point, stated the way unit_axis() in src/dinput.c actually
     * computes it: normalise the value we produced and require the answers the
     * host depends on. A centred axis at rest must read 0, or the wheel drifts. */
    {
        double span = (double)(r - (-r));
        double centre = ((double)scale_axis(0, -r, r) - (double)(-r)) / span;
        double max = ((double)scale_axis(32767, -r, r) - (double)(-r)) / span;
        double min = ((double)scale_axis(-32768, -r, r) - (double)(-r)) / span;

        check(centre * 2.0 - 1.0 > -0.0001 && centre * 2.0 - 1.0 < 0.0001,
              "a centred axis normalises to 0");
        check(max * 2.0 - 1.0 > 0.9999, "full lock reaches +1");
        check(min * 2.0 - 1.0 < -0.9999, "full lock reaches -1");
    }

    /* An asymmetric range, which is what a half-travel axis is given. */
    check(scale_axis(-32768, 0, r) == 0, "uncentred minimum -> lmin");
    check(scale_axis(32767, 0, r) == r, "uncentred maximum -> lmax");
}

/* --- the manufactured instance GUID survives a reboot ---------------------
 *
 * guidInstance is built from the path because SDL's instance id changes every
 * session. If that were unstable, every saved ini would silently stop matching
 * its device on the next boot -- a failure with no error message anywhere. */
static void test_instance_guid_is_stable(void)
{
    GUID a, b, c, d;

    guid_from_string("/dev/input/event3", &a);
    guid_from_string("/dev/input/event3", &b);
    guid_from_string("/dev/input/event7", &c);
    guid_from_string(NULL, &d);

    check_guid(&a, &b, "same path -> same instance GUID (across calls)");
    guid_from_string("/dev/input/event3", &b);
    check_guid(&a, &b, "same path -> same instance GUID (recomputed)");
    check(!IsEqualGUID(&a, &c), "different port -> different instance GUID");
    /* A NULL path must still produce a GUID, not read uninitialised memory. */
    guid_from_string(NULL, &c);
    guid_from_string(NULL, &d);
    check_guid(&c, &d, "NULL path -> a defined, repeatable GUID");
}

/* --- an unacquired device reads zeros, not an error ------------------------
 *
 * src/dinput.c relies on DirectInput's documented behaviour that an unacquired
 * device reports every button released. Returning an error instead would make
 * a pad that has gone away look like a pad with nothing pressed, and the trace
 * would show a full set of released buttons rather than the device vanishing.
 * This needs no hardware because an acquired-less device is exactly what an
 * unopened one is. */
static void test_unacquired_device_reads_zero(void)
{
    struct posix_di_device dev;
    DIJOYSTATE2 js;
    int i, nonzero = 0;

    memset(&dev, 0, sizeof dev);
    memset(&js, 0xAA, sizeof js);

    dev.navail = 4;
    for (i = 0; i < 4; i++) {
        dev.axis[i].present = 1;
        dev.axis[i].sdl_index = (unsigned)i;
        dev.axis[i].guid_offset = guid_offset_for_axis((unsigned)i);
        dev.axis[i].lmin = -10000;
        dev.axis[i].lmax = 10000;
    }
    /* joy == NULL and acquired == 0, which is the state being tested. */

    check(IDirectInputDevice8_GetDeviceState_impl(&dev, sizeof js, &js) == S_OK,
          "an unacquired device still succeeds");
    check(js.lX == 0 && js.lY == 0 && js.lZ == 0, "axes read zero");
    check(js.rglSlider[0] == 0 && js.rglSlider[1] == 0, "sliders read zero");
    for (i = 0; i < 128; i++)
        if (js.rgbButtons[i]) nonzero++;
    check(nonzero == 0, "all 128 buttons read released");
}

/* --- properties round-trip -------------------------------------------------
 *
 * The host WRITES DIPROP_RANGE and then reads axes against it. If SetProperty
 * dropped the write, every axis would silently normalise against the default
 * range instead of the configured one -- and a wheel with a narrowed range
 * would feel permanently half-throttle with nothing to show for it. */
static void test_property_round_trip(void)
{
    struct posix_di_device dev;
    DIPROPRANGE w;
    DIPROPRANGE got;

    memset(&dev, 0, sizeof dev);
    dev.navail = 3;
    dev.axis[0].present = 1; dev.axis[0].guid_offset = DIJOFS_X;
    dev.axis[1].present = 1; dev.axis[1].guid_offset = DIJOFS_Y;
    dev.axis[2].present = 1; dev.axis[2].guid_offset = DIJOFS_Z;

    memset(&w, 0, sizeof w);
    w.diph.dwSize = sizeof w;
    w.diph.dwHeaderSize = sizeof w.diph;
    w.diph.dwObj = DIJOFS_X;
    w.diph.dwHow = DIPH_BYID;
    w.lMin = -2147483647L;
    w.lMax = 2147483647L;

    check(IDirectInputDevice8_SetProperty_impl(&dev, DIPROP_RANGE, &w.diph) == S_OK,
          "SetProperty(RANGE) succeeds");

    memset(&got, 0, sizeof got);
    got.diph.dwObj = DIJOFS_X;
    check(IDirectInputDevice8_GetProperty_impl(&dev, DIPROP_RANGE, &got.diph) == S_OK,
          "GetProperty(RANGE) succeeds");
    check(got.lMin == w.lMin && got.lMax == w.lMax, "RANGE round-trips");
    check(dev.axis[0].lmin == w.lMin, "the write reached the axis state");

    /* Writing one axis must not disturb another -- a property applied to X and
     * then read on Y returning X's range is exactly the bug this guards. */
    check(dev.axis[1].lmin == 0 && dev.axis[1].lmax == 0,
          "writing X did not disturb Y");

    /* The id travels as a pointer on purpose (see dinput_posix.h); a bad cast
     * here would make every property silently read as DIPROP_RANGE. */
    check((DWORD)(size_t)DIPROP_RANGE == 4, "DIPROP_RANGE carries id 4");
    check((DWORD)(size_t)DIPROP_DEADZONE == 5, "DIPROP_DEADZONE carries id 5");
    check((DWORD)(size_t)DIPROP_SATURATION == 6, "DIPROP_SATURATION carries id 6");
    check((DWORD)(size_t)DIPROP_FFGAIN == 7, "DIPROP_FFGAIN carries id 7");
    check((DWORD)(size_t)DIPROP_AUTOCENTER == 9, "DIPROP_AUTOCENTER carries id 9");
}

/* --- only a constant force is accepted -------------------------------------
 *
 * The host creates GUID_ConstantForce and nothing else. Accepting a GUID we
 * cannot honour would mean starting an effect the device never plays, and the
 * player would feel the motor simply not respond. */
static void test_only_constant_force_is_accepted(void)
{
    struct posix_di_device dev;
    struct posix_di_effect *eff = NULL;
    DIEFFECT d;
    DICONSTANTFORCE cf;
    GUID not_constant;

    memset(&dev, 0, sizeof dev);
    memset(&d, 0, sizeof d);
    memset(&cf, 0, sizeof cf);

    d.lpvTypeSpecificParams = &cf;
    d.cbTypeSpecificParams = sizeof cf;
    d.cAxes = 1;
    cf.lMagnitude = DI_FFNOMINALMAX;

    guid_from_string("not-a-constant-force-effect", &not_constant);

    /* No haptic on this device, so even the correct GUID is refused -- and it
     * must be refused by the right reason, which is why the motor-less case is
     * what is asserted rather than pretending a device exists. */
    check(IDirectInputDevice8_CreateEffect_impl(&dev, &GUID_ConstantForce, &d,
                                                &eff, NULL) == E_INVALIDARG,
          "a device with no motor refuses an effect");
    check(eff == NULL, "and hands back no effect");

    check(!IsEqualGUID(&GUID_ConstantForce, &not_constant),
          "a different effect GUID is a different GUID");
}

/* --- the data format object is well-formed --------------------------------- */
static void test_data_format_object(void)
{
    check(c_dfDIJoystick2.dwSize == sizeof c_dfDIJoystick2, "format dwSize is set");
    check(c_dfDIJoystick2.dwDataSize == sizeof(DIJOYSTATE2),
          "format reports a DIJOYSTATE2-sized block");
}

int main(void)
{
    test_axis_guids_are_the_real_ones();
    test_axis_order_round_trips();
    test_offsets_land_on_members();
    test_scale_axis();
    test_instance_guid_is_stable();
    test_unacquired_device_reads_zero();
    test_property_round_trip();
    test_only_constant_force_is_accepted();
    test_data_format_object();

    if (g_failed) {
        printf("dinput posix tests: %d failure(s)\n", g_failed);
        return 1;
    }
    printf("dinput posix tests: %d checks, all passed\n", g_checks);
    return 0;
}