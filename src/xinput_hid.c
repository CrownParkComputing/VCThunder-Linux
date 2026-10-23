/* xinput_hid.c -- which DirectInput controller is an XInput pad.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * See xinput_hid.h.
 */
#include "posix_compat.h"
#include <stdlib.h>
#include <string.h>

#include "xinput_hid.h"

/* Portable: a pure string test over the RawInput device-name path, which on
 * Windows carries an "IG_" (ignore) marker for devices the driver has marked
 * non-ignorable. Kept outside the _WIN32 guard because the test calls it
 * directly and because it is the part of this file that has no OS in it. */
static int __attribute__((unused)) holds_ig(const char *path)
{
    for (; *path; path++)
        if ((path[0] == 'I' || path[0] == 'i') &&
            (path[1] == 'G' || path[1] == 'g') && path[2] == '_')
            return 1;
    return 0;
}


/* No RawInput here. The equivalent census is evdev/hidraw, and what this
 * table is FOR is worth stating: it tells the launcher which DirectInput
 * devices are really XInput pads, so a pad that appears twice -- once as an
 * XInput device and once as a generic HID one -- is listed once.
 *
 * Returning nothing found is the conservative answer, not a stub pretending.
 * An empty list means no device is identified as XInput, so
 * xinput_hid_is_xinput is false for everything and xinput_hid_hides hides
 * nothing: a device is only ever hidden once it has been positively
 * identified, and failing to identify one leaves it visible. The wrong way
 * round would make a force-feedback pad vanish from the launcher.
 *
 * The Linux census belongs with the input port proper, over /dev/input. */
unsigned xinput_hid_list(xinput_hid_t *out, unsigned max)
{
    (void)out;
    (void)max;
    return 0;
}


int xinput_hid_is_xinput(const xinput_hid_t *list, unsigned count,
                         unsigned long product_data1)
{
    unsigned k;

    for (k = 0; k < count; k++)
        if (list[k].ig &&
            ((unsigned long)list[k].vid | ((unsigned long)list[k].pid << 16))
                == product_data1)
            return 1;
    return 0;
}

int xinput_hid_hides(const xinput_hid_t *list, unsigned count,
                     unsigned long product_data1, int force_feedback)
{
    return !force_feedback &&
           xinput_hid_is_xinput(list, count, product_data1);
}
