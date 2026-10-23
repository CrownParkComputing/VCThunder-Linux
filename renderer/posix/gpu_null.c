/* gpu_null.c -- no GPU backend on Linux.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * These are the 13 platform symbols renderer/core/ calls and renderer/win32/
 * gpu.c defines as the accelerated raster backend. Until a Vulkan backend exists
 * they refuse, which is the state the renderer already has a defined answer for:
 * device.c only takes the GPU path when vgl_gpu_open() succeeds, and falls back
 * to the CPU rasteriser -- the reference implementation -- when it does not. That
 * fallback is why the conformance hash is meaningful here and would not be if
 * these silently pretended to draw.
 *
 * THE PRESENTATION SYMMETRY IS NOT HERE. vgl_present_* is the seam that puts a
 * finished CPU frame on screen, and it is orthogonal to this one: refusing the
 * GPU path is what keeps the hash comparable, and presenting is what makes that
 * hash visible. renderer/posix/gpu_sdl.c implements those five, so a frame is
 * shown without any accelerated raster being claimed.
 *
 * vgl_par_open/close/rows/workers are NOT here; par_linux.c implements them,
 * because the CPU path genuinely uses them and a stub would change the output
 * rather than merely decline a feature.
 */
#include <stdint.h>

#include "../core/vgl.h"

int vgl_gpu_preload(void)
{
    return 0;                       /* no backend to preload yet */
}

int vgl_gpu_open(void *caller_window, int w, int h, int raster_w, int raster_h,
                 int render_scale, int window_scale, int refresh_hz,
                 int present)
{
    (void)caller_window; (void)w; (void)h; (void)raster_w; (void)raster_h;
    (void)render_scale; (void)window_scale; (void)refresh_hz; (void)present;
    return 0;                       /* refuse: device.c falls back to CPU */
}

void vgl_gpu_close(void) { }
void vgl_gpu_set_raster(int x, int y, int w, int h,
                        int valid_x0, int valid_y0,
                        int valid_x1, int valid_y1)
{
    (void)x; (void)y; (void)w; (void)h;
    (void)valid_x0; (void)valid_y0; (void)valid_x1; (void)valid_y1;
}
void vgl_gpu_clear(uint32_t color, uint32_t depth) { (void)color; (void)depth; }
void vgl_gpu_clear_margins(int x0, int x1, int y0, int y1, int field_x0,
                            int field_x1)
{
    (void)x0; (void)x1; (void)y0; (void)y1; (void)field_x0; (void)field_x1;
}
void vgl_gpu_draw_triangle(const float *a, const float *b, const float *c)
{
    (void)a; (void)b; (void)c;
}
void vgl_gpu_swap(int interval) { (void)interval; }
int  vgl_gpu_sync_color(int buffer, uint32_t *dst, int w, int h)
{
    (void)buffer; (void)dst; (void)w; (void)h;
    return 0;
}
int  vgl_gpu_sync_depth(uint16_t *dst, int w, int h)
{
    (void)dst; (void)w; (void)h;
    return 0;
}
void vgl_gpu_upload_lfb(int buffer, const uint32_t *rgba_masked, int w, int h)
{
    (void)buffer; (void)rgba_masked; (void)w; (void)h;
}
void vgl_gpu_pump(void) { }
int  vgl_gpu_lost(void)
{
    return 0;                       /* never opened, so never lost */
}

/* vgl_present_* is NOT here: renderer/posix/gpu_sdl.c answers it, and refusing
 * presentation is what made the CPU path unwatchable rather than correct.
 *
 * THE ONE EXCEPTION is a machine with no SDL3 at all. Rather than fail an
 * otherwise unrelated conformance run on a missing optional dependency, the build
 * script compiles gpu_null.c with -DVGL_NO_PRESENTER in exactly that case and
 * says so out loud, which puts back the behaviour these symbols had before.
 * It is a refusal, not a silent substitute: nothing is displayed, and the hash
 * is unaffected either way. */
#ifdef VGL_NO_PRESENTER

int vgl_present_open(void *caller_window, int w, int h, int scale,
                     int refresh_hz)
{
    (void)caller_window; (void)w; (void)h; (void)scale; (void)refresh_hz;
    return 0;                       /* headless: nothing to present into */
}
void vgl_present_raster(int x, int y, int w, int h,
                        int valid_x0, int valid_y0,
                        int valid_x1, int valid_y1)
{
    (void)x; (void)y; (void)w; (void)h;
    (void)valid_x0; (void)valid_y0; (void)valid_x1; (void)valid_y1;
}
void vgl_present_frame(const uint32_t *buf, int w, int h, int swap_interval)
{
    (void)buf; (void)w; (void)h; (void)swap_interval;
}
void vgl_present_pump(void) { }
void vgl_present_close(void) { }

#endif /* VGL_NO_PRESENTER */