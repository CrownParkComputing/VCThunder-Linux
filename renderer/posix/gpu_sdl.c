/* gpu_sdl.c -- SDL3 presentation for the CPU rasteriser on POSIX.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * renderer/README.md names this seam and says a port replaces win32/present.c.
 * This is that replacement for the CPU path, and it mirrors that file's
 * decisions rather than inventing parallel ones: present only the RASTER (the
 * surface's pad columns are not the game's picture and have no business on
 * screen or in an aspect ratio), extend the nearest drawn texel across margin
 * the game never drew, hand the geometry to core's vgl_present_layout() so the
 * fit and bezel policy stays one implementation, honour the application's swap
 * interval as a deadline rather than as a Sleep(), and report the presentation
 * frame rate in the title bar.
 *
 * What it deliberately does NOT do is implement vgl_gpu_*. renderer/posix/
 * gpu_null.c keeps refusing those, so device.c stays on the CPU reference
 * rasteriser -- the path the golden conformance hash was taken from. That split
 * is why this file can exist without disturbing the hash: presentation happens
 * after rasterisation, in vgl_buffer_swap(), and only reads the finished frame.
 *
 * BEZEL ART IS NOT DRAWN HERE YET. vgl_present_layout() is called with
 * ready == 0, which is the contract's way of saying "this presenter cannot draw
 * the art": core then fits the picture to the window and logs once. Pretending
 * to be ready would silently drop the art's rim instead of reporting it.
 *
 * CALLING SDL FROM HERE needs one flag no other file in this library uses. The
 * 69 Glide entry points are reached through call_stdcall's hand-written asm,
 * which leaves the stack 4 mod 16, and every core/ file is compiled
 * -mpreferred-stack-boundary=2 to match that. SDL3 is an ordinary
 * 16-byte-aligned ELF library and requires the ABI's alignment at every call.
 * So this file is compiled -mpreferred-stack-boundary=4
 * -mincoming-stack-boundary=2: the incoming half tells GCC the entry stack is
 * NOT the 16 the ELF default assumes, so it realigns in the prologue
 * (`and $-16,%esp`) and the calls out to SDL are aligned. Simply dropping the
 * =2 flag -- which is exactly right for src/'s SDL-facing files -- is NOT
 * enough here, because those are entered from ordinary aligned C while this is
 * entered from the misaligned harness path. Getting it backwards is a silent
 * segfault inside SDL, so tools/build-linux-conformance.sh asserts the
 * realignment is actually in the emitted code.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <SDL3/SDL.h>

#include "../core/vgl.h"
#include "../core/vcglide.h"
#include "../core/layout.h"
#include "overlay.h"

static SDL_Window  *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture *s_texture;
static int       s_tex_w, s_tex_h;
static uint32_t *s_rgb;                 /* cropped XRGB8888 raster for upload */
static int       s_w, s_h;              /* the SURFACE */
static int       s_rx, s_ry, s_rw, s_rh;/* the RASTER inside it, presented */
static int       s_vx0, s_vy0, s_vx1, s_vy1; /* drawn part, raster-relative */
static int       s_scale;
static int       s_refresh_hz;
static int       s_owns_window;
static int       s_quit_requested;     /* the window was asked to close */
static int (*s_overlay)(void *, int, int);
static void (*s_overlay_event)(const void *);
void __attribute__((stdcall)) vglSetOverlay(int (*draw)(void *, int, int))
{
    s_overlay = draw;
}
void __attribute__((stdcall)) vglSetOverlayEventHandler(void (*handler)(const void *))
{ s_overlay_event = handler; }
int vgl_overlay_draw(void *renderer, int width, int height)
{ return s_overlay ? s_overlay(renderer, width, height) : 0; }
void vgl_overlay_event(const void *event)
{ if (s_overlay_event) s_overlay_event(event); }

/* The surface's pad is not copied, so the upload buffer only has to hold the
 * largest raster it can be asked for, which is the surface itself. */

/* ---- pacing ------------------------------------------------------------
 * grBufferSwap(0) is deliberately immediate: Hydro owns its gameplay cadence
 * and a global display wait causes slow motion when a frame misses the
 * deadline. A positive interval is a different request -- both operator
 * renderers pass 1 precisely because they expect a 60 Hz retrace to bound an
 * otherwise unpaced loop. Keep that request in the presenter.
 */
static Uint64 s_pace_freq;
static Uint64 s_pace_next;
static int s_pace_interval;

/* A viewing aid, not a cadence. The conformance capture is eight frames and a
 * CPU replay of it is over in a tenth of a second, which is not something a
 * person can look at; VCGLIDE_PRESENT_HOLD_MS holds each presented frame for a
 * moment so `make view-linux` is watchable. Zero, the default, holds nothing --
 * and the game's own cadence stays owned by swap_interval above, because this
 * file cannot tell a replay from a running title and must not try.
 *
 * It lives here rather than in glide-replay.c so the Windows tool and the shared
 * capture path are untouched, and it is applied AFTER the frame is on screen:
 * device.c pumps the window immediately after this returns, so a held frame is
 * still a window that answers its close button. */
static int s_hold_ms;

static void pace_reset(void)
{
    s_pace_next = 0;
    s_pace_interval = 0;
}

static void wait_swap_interval(int interval)
{
    Uint64 now, period, remain;

    if (interval <= 0) {
        pace_reset();
        return;
    }
    if (interval > 255)
        interval = 255;
    if (!s_pace_freq)
        s_pace_freq = SDL_GetPerformanceFrequency();
    if (!s_pace_freq)
        return;
    now = SDL_GetPerformanceCounter();
    period = (s_pace_freq * (Uint64)interval) /
             (Uint64)(s_refresh_hz > 0 ? s_refresh_hz : 60);
    if (period < 1)
        period = 1;

    if (!s_pace_next || s_pace_interval != interval ||
        (now > s_pace_next && now - s_pace_next > s_pace_freq)) {
        s_pace_next = now + period;
        s_pace_interval = interval;
    } else {
        while (s_pace_next <= now)
            s_pace_next += period;
    }

    for (;;) {
        now = SDL_GetPerformanceCounter();
        if (now >= s_pace_next)
            break;
        remain = s_pace_next - now;
        /* Sleep for the coarse part and spin through the last few
         * milliseconds, so SDL_Delay's granularity cannot accumulate drift. */
        if (remain > s_pace_freq / 250)
            SDL_Delay(1);
    }
    s_pace_next += period;
}

/* ---- raster geometry ---------------------------------------------------- */

/* The host can name the raster at open time because it saw the game's original
 * resolution before substituting a legal surface. Same hint, same clamping
 * rule as win32/present.c: one fact, one rule, even though the two files never
 * run in the same process. */
static int raster_hint(const char *name, int surface_extent)
{
    const char *value = getenv(name);
    char *end;
    long v;

    if (!value || !*value)
        return surface_extent;
    v = strtol(value, &end, 10);
    if (*end || v <= 0 || v > surface_extent) {
        vgl_log(0, "present: ignoring invalid %s='%s' for surface extent %d",
                name, value, surface_extent);
        return surface_extent;
    }
    return (int)v;
}

/* The picture's rectangle for this window, using core's shared policy. The
 * canvas is only meaningful with bezel art, which this presenter cannot draw,
 * so it is taken and discarded. */
static void destination_rect(int cw, int ch, vgl_rect_t *out)
{
    vgl_rect_t canvas;
    int fullscreen = 0;

    /* SDL3's fullscreen window IS the borderless desktop-resolution kind
     * win32/present.c recognised from WS_POPUP without WS_CAPTION, and a non-
     * NULL fullscreen mode is the current answer for it. (Asking for the old
     * SDL_WINDOW_FULLSCREEN_DESKTOP flag here would build against 3.4 only by
     * accepting a deprecated spelling.) */
    if (s_window)
        fullscreen = SDL_GetWindowFullscreenMode(s_window) != NULL;
    /* ready == 0: no bezel art from this presenter. See the header comment. */
    vgl_present_layout(fullscreen, s_rw, s_rh, cw, ch, 0, out, &canvas);
}

/* Extend nearest drawn texels across the undrawn top/right raster margin only
 * during presentation. Framebuffer readback remains unchanged -- the same
 * deliberate asymmetry as win32/present.c. */
static void extend_undrawn_edges(void)
{
    int x, y;

    if (s_vx0 >= s_vx1 || s_vy0 >= s_vy1)
        return;
    for (y = 0; y < s_vy0; y++)
        memcpy(s_rgb + (size_t)y * s_rw,
               s_rgb + (size_t)s_vy0 * s_rw, (size_t)s_rw * 4);
    for (y = s_vy1; y < s_rh; y++)
        memcpy(s_rgb + (size_t)y * s_rw,
               s_rgb + (size_t)(s_vy1 - 1) * s_rw, (size_t)s_rw * 4);
    for (y = 0; y < s_rh; y++) {
        uint32_t *row = s_rgb + (size_t)y * s_rw;
        for (x = 0; x < s_vx0; x++)
            row[x] = row[s_vx0];
        for (x = s_vx1; x < s_rw; x++)
            row[x] = row[s_vx1 - 1];
    }
}

/* Report one-second wall-clock presentation FPS in the title bar. */
static void title_fps(void)
{
    static Uint64 freq, t0;
    static unsigned frames;
    Uint64 now;
    char buf[96];
    double secs;

    if (!s_owns_window || !s_window)
        return;                         /* the caller's window is the caller's */
    if (!freq) {
        freq = SDL_GetPerformanceFrequency();
        t0 = SDL_GetPerformanceCounter();
        return;
    }
    frames++;
    now = SDL_GetPerformanceCounter();
    if (now - t0 < freq)
        return;
    secs = (double)(now - t0) / (double)freq;
    vgl_window_title(buf, sizeof buf, (double)frames / secs);
    SDL_SetWindowTitle(s_window, buf);
    t0 = now;
    frames = 0;
}

/* ---- the seam ----------------------------------------------------------- */

int vgl_present_open(void *caller_window, int w, int h, int scale,
                     int refresh_hz)
{
    char name[96];
    int pw, ph;

    vgl_present_close();
    if (w <= 0 || h <= 0)
        return 0;

    s_w = w;
    s_h = h;
    s_scale = scale > 0 ? scale : 1;
    s_refresh_hz = refresh_hz > 0 ? refresh_hz : 60;
    s_quit_requested = 0;
    pace_reset();
    s_rx = s_ry = 0;
    s_rw = raster_hint("VCGLIDE_RASTER_WIDTH", w);
    s_rh = raster_hint("VCGLIDE_RASTER_HEIGHT", h);
    s_vx0 = s_vy0 = 0; s_vx1 = s_rw; s_vy1 = s_rh;
    /* Not raster_hint(): that one answers "the whole surface" when a variable is
     * unset, which is the right default for a raster and a ten-second pause for
     * a hold. An unset hold means no hold. */
    {
        const char *hv = getenv("VCGLIDE_PRESENT_HOLD_MS");
        char *hend;
        long hv_n = hv ? strtol(hv, &hend, 10) : 0;

        s_hold_ms = (hv && !*hend && hv_n > 0 && hv_n <= 10000) ? (int)hv_n : 0;
    }

    s_rgb = (uint32_t *)calloc((size_t)w * (size_t)h, 4);
    if (!s_rgb)
        return 0;

    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        vgl_log(0, "present: SDL video init failed (%s); rendering continues "
                   "and nothing is displayed", SDL_GetError());
        free(s_rgb);
        s_rgb = NULL;
        return 0;
    }

    pw = s_rw * s_scale;
    ph = s_rh * s_scale;
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;
    vgl_window_title(name, sizeof name, 0.0);   /* no frame rate yet */
    s_window = caller_window ? (SDL_Window *)caller_window : SDL_CreateWindow(name, pw, ph, 0);
    if (!s_window) {
        vgl_log(0, "present: no window (%s); rendering continues and nothing "
                   "is displayed", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        free(s_rgb);
        s_rgb = NULL;
        return 0;
    }
    s_owns_window = !caller_window;

    /* Scale/upload the completed CPU raster on the GPU when possible. Falling
     * back to software keeps systems without a 32-bit OpenGL driver usable.
     * An explicit SDL driver selection remains authoritative. */
    if (SDL_GetHint(SDL_HINT_RENDER_DRIVER))
        s_renderer = SDL_CreateRenderer(s_window, NULL);
    else {
        s_renderer = SDL_CreateRenderer(s_window, "opengl");
        if (!s_renderer) s_renderer = SDL_CreateRenderer(s_window, "software");
    }
    if (!s_renderer) {
        vgl_log(0, "present: no SDL renderer (%s); nothing is displayed",
                SDL_GetError());
        if (s_owns_window)
            SDL_DestroyWindow(s_window);
        s_window = NULL;
        s_owns_window = 0;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        free(s_rgb);
        s_rgb = NULL;
        return 0;
    }

    vgl_log(0, "present: %dx%d XRGB8888 through SDL3 %s on window %p "
               "(raster %dx%d at %dx, surface %dx%d, edge extension "
               "L%d T%d R%d B%d)", w, h,
            SDL_GetRendererName(s_renderer), (void *)s_window,
            s_rw, s_rh, s_scale, s_w, s_h,
            s_vx0, s_vy0, s_rw - s_vx1, s_rh - s_vy1);
    return 1;
}

void vgl_present_raster(int x, int y, int w, int h,
                        int valid_x0, int valid_y0,
                        int valid_x1, int valid_y1)
{
    if (w <= 0 || h <= 0)
        return;
    valid_x0 -= x; valid_x1 -= x;
    valid_y0 -= y; valid_y1 -= y;
    if (valid_x0 < 0) valid_x0 = 0;
    if (valid_x0 > w) valid_x0 = w;
    if (valid_y0 < 0) valid_y0 = 0;
    if (valid_y0 > h) valid_y0 = h;
    if (valid_x1 < 0) valid_x1 = 0;
    if (valid_x1 > w) valid_x1 = w;
    if (valid_y1 < 0) valid_y1 = 0;
    if (valid_y1 > h) valid_y1 = h;
    if (x == s_rx && y == s_ry && w == s_rw && h == s_rh &&
        valid_x0 == s_vx0 && valid_y0 == s_vy0 &&
        valid_x1 == s_vx1 && valid_y1 == s_vy1)
        return;
    s_rx = x; s_ry = y; s_rw = w; s_rh = h;
    s_vx0 = valid_x0; s_vy0 = valid_y0;
    s_vx1 = valid_x1; s_vy1 = valid_y1;
    /* The window is sized at the raster and left alone after that, as on
     * Win32: the host sizes it after every open (src/window.c). A raster change
     * only costs a new texture, which the frame path notices. */
}

void vgl_present_frame(const uint32_t *buf, int w, int h, int swap_interval)
{
    vgl_rect_t pic;
    SDL_FRect src, dst;
    SDL_Rect area;
    int cw = 0, ch = 0;
    int y;

    if (!s_renderer || !s_rgb || !buf || w != s_w || h != s_h)
        return;
    /* A window the user closed is not redrawn: vgl_present_pump() has already
     * taken it down, and these two are the only guard the closed case needs. */
    if (!s_window)
        return;
    if (s_rw <= 0 || s_rh <= 0 || s_rx < 0 || s_ry < 0 ||
        s_rx + s_rw > w || s_ry + s_rh > h)
        return;
    wait_swap_interval(swap_interval);

    /* Copy out only the raster. */
    for (y = 0; y < s_rh; y++) {
        const uint32_t *row = buf + (size_t)(s_ry + y) * (size_t)w + (size_t)s_rx;
        uint32_t *out = s_rgb + (size_t)y * (size_t)s_rw;
        memcpy(out, row, (size_t)s_rw * 4);
    }
    extend_undrawn_edges();

    if (!s_texture || s_tex_w != s_rw || s_tex_h != s_rh) {
        if (s_texture)
            SDL_DestroyTexture(s_texture);
        /* XRGB8888 is what the rasteriser produces: 0x00RRGGBB in a native
         * uint32, which on little-endian is the byte order SDL calls
         * XRGB8888/BGRX32. No swizzle, so what is displayed is bit-for-bit
         * what the conformance hash was taken over. */
        s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_XRGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      s_rw, s_rh);
        s_tex_w = s_tex_h = 0;
        if (!s_texture) {
            vgl_log(0, "present: cannot make a %dx%d texture (%s)",
                    s_rw, s_rh, SDL_GetError());
            return;
        }
        s_tex_w = s_rw;
        s_tex_h = s_rh;
    }

    area.x = 0; area.y = 0; area.w = s_rw; area.h = s_rh;
    if (!SDL_UpdateTexture(s_texture, &area, s_rgb, s_rw * 4)) {
        VGL_LOG_ONCE("present: SDL_UpdateTexture failed (%s)", SDL_GetError());
        return;
    }

    if (!SDL_GetWindowSizeInPixels(s_window, &cw, &ch) || cw <= 0 || ch <= 0)
        return;
    destination_rect(cw, ch, &pic);

    int menu_open;
    do {
    SDL_GetWindowSizeInPixels(s_window, &cw, &ch);
    destination_rect(cw, ch, &pic);
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);
    src.x = 0.0f; src.y = 0.0f;
    src.w = (float)s_rw; src.h = (float)s_rh;
    dst.x = (float)pic.x; dst.y = (float)pic.y;
    dst.w = (float)pic.w; dst.h = (float)pic.h;
    if (!SDL_RenderTexture(s_renderer, s_texture, &src, &dst)) {
        VGL_LOG_ONCE("present: SDL_RenderTexture failed (%s)", SDL_GetError());
        return;
    }
    menu_open = s_overlay ? s_overlay(s_renderer, cw, ch) : 0;
    SDL_RenderPresent(s_renderer);
    if (menu_open) SDL_Delay(16);
    } while (menu_open);
    if (s_hold_ms > 0)
        SDL_Delay((Uint32)s_hold_ms);
    title_fps();
}

/* Pump the provider-owned window from the swap path. */
void vgl_present_pump(void)
{
    SDL_Event e;

    if (!s_owns_window || !s_window)
        return;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT ||
            (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
             e.window.windowID == SDL_GetWindowID(s_window))) {
            /* Win32 answers this by re-posting WM_QUIT and returning, because
             * there the host owns a message loop that will pick it up and call
             * ExitProcess after flushing NVRAM. Linux has no such loop yet --
             * that is the windowing tier, and it is blocked on the loader -- so
             * there is nobody to receive a re-posted signal.
             *
             * Measured, not assumed: putting an SDL_EVENT_QUIT back on the
             * queue after a close request had been dequeued does not survive.
             * SDL_PushEvent() reports success and SDL_HasEvent() reports the
             * event, but a following SDL_PollEvent() never returns it (SDL3
             * 3.4.18, dummy video driver). A mechanism that cannot be observed
             * is worse than none, because it looks implemented.
             *
             * So the request is honoured directly instead: the window this
             * presenter owns is taken down, which is what clicking its close
             * box means, and every frame after that is a no-op because there is
             * no surface to draw into. When the windowing tier gives the host a
             * pump of its own, THAT is where the flush-then-exit signal belongs;
             * this flag is the seam it will read. */
            s_quit_requested = 1;
            vgl_log(0, "present: close requested on the provider's window; "
                       "presentation stops (the host's exit path is the "
                       "windowing tier, not this presenter)");
            vgl_present_close();
            return;
        }
    }
}

void vgl_present_close(void)
{
    if (s_overlay && s_renderer) s_overlay(s_renderer, 0, 0);
    if (s_texture)
        SDL_DestroyTexture(s_texture);
    if (s_renderer)
        SDL_DestroyRenderer(s_renderer);
    if (s_owns_window && s_window) {
        SDL_DestroyWindow(s_window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    s_texture = NULL;
    s_renderer = NULL;
    s_window = NULL;
    s_tex_w = s_tex_h = 0;
    s_owns_window = 0;
    s_refresh_hz = 0;
    pace_reset();
    free(s_rgb);
    s_rgb = NULL;
}
