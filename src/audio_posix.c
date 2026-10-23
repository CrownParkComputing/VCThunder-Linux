/* audio_posix.c -- no audio on Linux yet.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The shared audio transport on Windows is WASAPI (src/audio.c), which does not
 * build here. Until a native backend exists (SDL audio / ALSA / PipeWire), the
 * shared surface is stubbed so the host links and the game runs silently:
 * audio_ready() answers 0, so main.c skips audio_install() and the title runs
 * without sound. The title-specific mixer (audio_offroad.c) still compiles and
 * its installer is simply never called.
 */
#include "vcthunder.h"

void audio_hold_this_thread(int on) { (void)on; }
int  audio_mask_depth(void) { return 0; }
void audio_mask_unwind(int depth) { (void)depth; }
unsigned audio_held_commands(void) { return 0; }
int  audio_held(void) { return 0; }

int audio_preload(unsigned dcs_us, unsigned volume_percent,
                  const char *routing, unsigned balance)
{
    (void)dcs_us; (void)volume_percent; (void)routing; (void)balance;
    return 0;                        /* no audio backend */
}

int audio_ready(void) { return 0; }

void audio_route_buses(float front, float seat, float *left, float *right)
{
    if (left)
        *left = front;
    if (right)
        *right = seat;
}

int audio_install(void) { return 0; }
int audio_install_passthrough(void) { return 0; }
void audio_shutdown(void) {}
void audio_report_dcs_state(void) {}
