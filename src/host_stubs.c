/* host_stubs.c -- optional diagnostics omitted from the native Linux build.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The Windows profiler, interpolation and watchdog use thread-context and
 * write-watch facilities. The native build keeps their optional hooks inert;
 * game loading, input, audio and the shared diagnostic primitives are real.
 */
#include "vcthunder.h"

/* --- diagnostics (profile.c, drawreplay.c, texstate.c, diag.c, watchdog.c) */
void   glide_watchdog_arm(void) {}
void   profile_shutdown(void) {}
void   profile_interleave_tick(void) {}
void   profile_attract_poll(void) {}
void   profile_report(void) {}
void   profile_report_geometry(unsigned frames) { (void)frames; }
void   profile_report_call_times(unsigned frames) { (void)frames; }
void   drawreplay_swapped(uint64_t before_qpc, uint64_t after_qpc)
{
    (void)before_qpc; (void)after_qpc;
}
void   drawreplay_idle(int interval) { (void)interval; }
double drawreplay_idle_ms(double *latest_ms, unsigned *past)
{
    if (latest_ms)
        *latest_ms = 0.0;
    if (past)
        *past = 0;
    return 0.0;
}
void   wd_note_swap(uint64_t swap_qpc) { (void)swap_qpc; }
void   texstate_report_cycles(uint64_t frames) { (void)frames; }
int    texstate_claim(const char *name, void *real, uint32_t va)
{
    (void)name; (void)real; (void)va;
    return 0;                        /* not claimed: use the real path */
}
void   texstate_frame_end(void) {}
void   texstate_report(void) {}
void   texstate_report_lod(uint64_t frames) { (void)frames; }
