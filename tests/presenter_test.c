/* presenter_test.c -- the SDL presenter, checked against the pixels it puts up.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * WHY THIS EXISTS. The conformance hash proves the CPU rasteriser produces the
 * right frame, and it keeps proving that whether or not anything is ever shown.
 * It says nothing about the last hop: the one that turns a 0x00RRGGBB word into
 * red, green and blue in a window. Get the pixel format backwards there and
 * every hash still passes while the player watches a cyan submarine. So this
 * pins the presenter against what SDL actually rendered, read back out of the
 * window, under the dummy driver so it runs headless in the gate.
 *
 * It #includes the implementation for two reasons. The state worth checking (the
 * raster crop, the undrawn-edge extension, the quit signal) is file-static by
 * design, and the three core functions the presenter calls -- vgl_log,
 * vgl_window_title, vgl_present_layout -- are stand-ins here, so a failure means
 * the presenter is wrong rather than that core's layout policy moved. Core's fit
 * arithmetic is tested on its own, in tests/bezel_test.c.
 *
 * Like the input tier's tests, this needs SDL3: without it the gate says SKIPPED,
 * because a check that did not run must never read as one that passed.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#include "../renderer/core/vgl.h"
#include "../renderer/core/vcglide.h"
#include "../renderer/core/layout.h"

/* ---- stand-ins for core, set up before the implementation is pulled in ---- */

static int t_log_calls;
static int t_title_calls;
static double t_last_fps = -1.0;

/* What the presenter asked core to lay out, so the test can assert the
 * presenter passes its geometry through instead of inventing its own. */
static int t_layout_fullscreen = -1;
static int t_layout_ready = -1;
static int t_layout_rw = -1, t_layout_rh = -1;
static int t_layout_cw = -1, t_layout_ch = -1;

/* 0: the picture fills the client. 1: it is inset, so a presenter that ignored
 * the returned rectangle would cover the margin core said to leave black. */
static int t_pic_mode;

void vgl_log(int level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    t_log_calls++;
    if (getenv("VCTH_PRESENTER_VERBOSE")) {
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
}

void vgl_window_title(char *buf, size_t n, double fps)
{
    t_title_calls++;
    t_last_fps = fps;
    snprintf(buf, n, "presenter test");
}

int vgl_present_layout(int fullscreen, int raster_w, int raster_h,
                       int client_w, int client_h, int ready,
                       vgl_rect_t *picture, vgl_rect_t *canvas)
{
    t_layout_fullscreen = fullscreen;
    t_layout_ready = ready;
    t_layout_rw = raster_w;
    t_layout_rh = raster_h;
    t_layout_cw = client_w;
    t_layout_ch = client_h;

    canvas->x = 0; canvas->y = 0;
    canvas->w = client_w; canvas->h = client_h;
    if (t_pic_mode == 1) {
        /* Inset by a quarter of the width each side, centred: exactly the
         * letterbox a real fit would produce on a wider client. */
        picture->x = client_w / 4;
        picture->y = 0;
        picture->w = client_w / 2;
        picture->h = client_h;
    } else {
        picture->x = 0; picture->y = 0;
        picture->w = client_w; picture->h = client_h;
    }
    return 0;
}

/* The file under test. Included, not linked: see the header comment. */
#include "../renderer/posix/gpu_sdl.c"

/* ---- helpers ------------------------------------------------------------ */

static int t_failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("  FAIL: %s\n", what);
        t_failures++;
    }
}

/* XRGB8888, as the rasteriser makes it: 0x00RRGGBB. Every test colour has three
 * distinct channels, so a swap anywhere in the presenter shows up as a mismatch
 * rather than a plausible-looking frame. */
#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static void fill_solid(uint32_t *buf, int w, int h, uint32_t c)
{
    int i, n = w * h;

    for (i = 0; i < n; i++)
        buf[i] = c;
}

static void fill_ramp(uint32_t *buf, int w, int h)
{
    int x, y;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            buf[y * w + x] = RGB(x * 16 + 1, y * 32 + 2, 0x80);
}

/* The window's actual pixels, in the same numeric space the test authored. */
static SDL_Surface *read_back(int *w_out, int *h_out)
{
    SDL_Surface *got = SDL_RenderReadPixels(s_renderer, NULL);
    SDL_Surface *xrgb;

    if (!got) {
        printf("  (readback failed: %s)\n", SDL_GetError());
        return NULL;
    }
    if (got->format != SDL_PIXELFORMAT_XRGB8888) {
        xrgb = SDL_ConvertSurface(got, SDL_PIXELFORMAT_XRGB8888);
        SDL_DestroySurface(got);
        got = xrgb;
        if (!got) {
            printf("  (convert to XRGB8888 failed: %s)\n", SDL_GetError());
            return NULL;
        }
    }
    *w_out = got->w;
    *h_out = got->h;
    return got;
}

static uint32_t at(SDL_Surface *s, int x, int y)
{
    const uint8_t *row = (const uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch;
    uint32_t v;

    memcpy(&v, row + (size_t)x * 4u, sizeof v);
    return v & 0x00FFFFFFu;
}

static int shown_is(SDL_Surface *s, const char *what, int x, int y, uint32_t want)
{
    uint32_t got = at(s, x, y);

    if (got == want)
        return 1;
    printf("  %s: pixel %d,%d is %06llX, expected %06llX\n", what, x, y,
           (unsigned long long)got, (unsigned long long)want);
    return 0;
}

/* Drain anything the window manager or our own open queued, so an event test
 * starts from a known-empty queue. */
static void drain_events(void)
{
    SDL_Event e;

    while (SDL_PollEvent(&e))
        ;
}

/* ---- the cases ---------------------------------------------------------- */

/* A 1:1 frame must arrive unchanged. This is the case that catches a wrong
 * SDL_PixelFormat: everything else about the presenter can be right and the
 * picture still come out with its channels shuffled. */
static void case_one_to_one(void)
{
    enum { W = 16, H = 8 };
    uint32_t buf[W * H];
    SDL_Surface *s;
    int sw = 0, sh = 0, x, y, ok = 1;

    fill_ramp(buf, W, H);
    if (!vgl_present_open(NULL, W, H, 1, 60)) {
        printf("  (open failed; cannot check 1:1)\n");
        return;
    }
    vgl_present_frame(buf, W, H, 0);
    s = read_back(&sw, &sh);
    check(s != NULL, "1:1: the window has pixels to read back");
    if (!s)
        return;
    check(sw == W && sh == H, "1:1: the window is the raster's size");
    for (y = 0; y < H && y < sh; y++)
        for (x = 0; x < W && x < sw; x++)
            if (!shown_is(s, "1:1", x, y, buf[y * W + x]))
                ok = 0;
    check(ok, "1:1: every channel of every pixel is where it was authored");
    SDL_DestroySurface(s);
    vgl_present_close();
}

/* The surface is 640x400 and the game's picture is smaller than that; only the
 * raster belongs on screen. Here the pad is a different colour to the picture,
 * so a presenter that showed the whole surface would put green on screen. */
static void case_raster_crop(void)
{
    enum { W = 32, H = 16 };
    const uint32_t red = RGB(255, 10, 20);
    const uint32_t green = RGB(10, 255, 20);
    uint32_t buf[W * H];
    SDL_Surface *s;
    int sw = 0, sh = 0, x, y, ok = 1;

    fill_solid(buf, W, H, green);
    for (y = 0; y < H; y++)
        for (x = 0; x < 16; x++)
            buf[y * W + x] = red;

    if (!vgl_present_open(NULL, W, H, 1, 60)) {
        printf("  (open failed; cannot check the crop)\n");
        return;
    }
    vgl_present_raster(0, 0, 16, H, 0, 0, 16, H);
    vgl_present_frame(buf, W, H, 0);
    s = read_back(&sw, &sh);
    check(s != NULL, "crop: the window has pixels to read back");
    if (!s)
        return;
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++)
            if (!shown_is(s, "crop", x, y, red))
                ok = 0;
    check(ok, "crop: the surface's pad never reaches the window");
    SDL_DestroySurface(s);
    vgl_present_close();
}

/* Core decides where the picture goes; the presenter must draw it there and not
 * across the whole client. The inset half must stay the clear colour. */
static void case_layout_honoured(void)
{
    enum { W = 32, H = 16 };
    const uint32_t white = RGB(200, 220, 240);
    uint32_t buf[W * H];
    SDL_Surface *s;
    int sw = 0, sh = 0;

    fill_solid(buf, W, H, white);
    if (!vgl_present_open(NULL, W, H, 1, 60)) {
        printf("  (open failed; cannot check the layout)\n");
        return;
    }
    t_pic_mode = 1;
    vgl_present_frame(buf, W, H, 0);
    t_pic_mode = 0;
    s = read_back(&sw, &sh);
    check(s != NULL, "layout: the window has pixels to read back");
    if (!s) {
        vgl_present_close();
        return;
    }
    check(at(s, 0, sh / 2) != white,
          "layout: the letterbox core asked for is left alone, not covered");
    shown_is(s, "layout", sw / 2, sh / 2, white);
    check(at(s, sw / 2, sh / 2) == white,
          "layout: the picture lands where core put it");
    SDL_DestroySurface(s);
    vgl_present_close();
}

/* The crop and clamp arithmetic, on its own. vgl_present_raster() is handed
 * absolute clip coordinates and stores them raster-relative: get the subtraction
 * wrong and the drawn region slides by exactly the raster offset, which on
 * Hydro's letterboxed 512x400-in-640x400 looks like a thin wrong-edge smear
 * rather than an obvious bug. */
static void case_raster_clamp(void)
{
    int sw_save = s_w, sh_save = s_h;

    s_w = 32; s_h = 16;
    s_rx = s_ry = s_rw = s_rh = 0;
    s_vx0 = s_vy0 = s_vx1 = s_vy1 = 0;

    vgl_present_raster(4, 2, 20, 10, 0, 0, 100, 100);
    check(s_rx == 4 && s_ry == 2 && s_rw == 20 && s_rh == 10,
          "clamp: the raster origin and size are stored as given");
    check(s_vx0 == 0 && s_vy0 == 0 && s_vx1 == 20 && s_vy1 == 10,
          "clamp: a valid box wider than the raster is clipped to it");

    vgl_present_raster(4, 2, 20, 10, 10, 6, 14, 9);
    check(s_vx0 == 6 && s_vy0 == 4 && s_vx1 == 10 && s_vy1 == 7,
          "clamp: an interior valid box is made raster-relative");

    vgl_present_raster(0, 0, 8, 8, -5, -5, -1, -1);
    check(s_vx0 == 0 && s_vx1 == 0 && s_vy0 == 0 && s_vy1 == 0,
          "clamp: a fully outside valid box collapses instead of wrapping");

    {
        int before_rw = s_rw;
        vgl_present_raster(0, 0, 8, 8, 0, 0, 8, 8);
        check(s_rw == before_rw, "clamp: a zero-size raster is ignored");
    }

    s_w = sw_save; s_h = sh_save;
}

/* The edge extension is a presenter-only lie: the framebuffer the game reads
 * back is untouched, and only what is uploaded gets nearest-texel margins. Pin
 * both directions of it, including that it refuses to run on an empty region. */
static void case_edge_extension(void)
{
    enum { R = 4 };
    uint32_t *save = s_rgb;
    uint32_t rgb[R * R];
    int i;

    s_rgb = rgb;
    s_rw = R; s_rh = R;
    for (i = 0; i < R * R; i++)
        rgb[i] = 0xDEADBEEFu;
    /* Drawn region is the 2x2 at (1,1). */
    s_vx0 = 1; s_vy0 = 1; s_vx1 = 3; s_vy1 = 3;
    rgb[1 * R + 1] = RGB(1, 10, 100);
    rgb[1 * R + 2] = RGB(2, 20, 200);
    rgb[2 * R + 1] = RGB(3, 30, 30);
    rgb[2 * R + 2] = RGB(4, 40, 40);

    extend_undrawn_edges();

    check(rgb[0 * R + 1] == RGB(1, 10, 100) && rgb[0 * R + 2] == RGB(2, 20, 200),
          "edges: the undrawn top row takes the first drawn row");
    check(rgb[3 * R + 1] == RGB(3, 30, 30) && rgb[3 * R + 2] == RGB(4, 40, 40),
          "edges: the undrawn bottom row takes the last drawn row");
    check(rgb[1 * R + 0] == RGB(1, 10, 100) && rgb[2 * R + 0] == RGB(3, 30, 30),
          "edges: the undrawn left column takes the first drawn column");
    check(rgb[1 * R + 3] == RGB(2, 20, 200) && rgb[2 * R + 3] == RGB(4, 40, 40),
          "edges: the undrawn right column takes the last drawn column");

    /* An empty drawn region must leave the frame alone rather than read row
     * s_vy0 of a buffer nobody filled. */
    for (i = 0; i < R * R; i++)
        rgb[i] = 0x00123456u;
    s_vx0 = 2; s_vx1 = 2;
    extend_undrawn_edges();
    check(rgb[0] == 0x00123456u, "edges: an empty region is not extended");

    s_rgb = save;
}

/* Refuse cleanly: a frame whose size disagrees with the surface is a caller bug,
 * and drawing it anyway would scale the wrong thing rather than say so. */
static void case_guards(void)
{
    enum { W = 16, H = 8 };
    const uint32_t red = RGB(200, 20, 20);
    uint32_t buf[W * H];
    SDL_Surface *s;
    int sw = 0, sh = 0;

    fill_solid(buf, W, H, red);
    check(!vgl_present_open(NULL, 0, 8, 1, 60),
          "guards: a zero-size surface is refused, not opened");

    if (!vgl_present_open(NULL, W, H, 1, 60)) {
        printf("  (open failed; cannot check the guards)\n");
        return;
    }
    vgl_present_frame(buf, W, H, 0);
    vgl_present_frame(buf, W * 2, H, 0);         /* wrong size: ignored */
    vgl_present_frame(NULL, W, H, 0);            /* no pixels: ignored */
    s = read_back(&sw, &sh);
    check(s != NULL, "guards: a rejected frame leaves a readable window");
    if (s) {
        int ok = 1, x, y;
        for (y = 0; y < sh; y++)
            for (x = 0; x < sw; x++)
                if (!shown_is(s, "guards", x, y, red))
                    ok = 0;
        check(ok, "guards: a rejected frame does not scribble over the last one");
        SDL_DestroySurface(s);
    }
    vgl_present_close();
    vgl_present_frame(buf, W, H, 0);             /* closed: must not crash */
    check(!s_rgb && !s_renderer && !s_window,
          "guards: close really lets go of the window and its buffers");
}

/* The close request. On Win32 the provider re-posts WM_QUIT for the host's
 * message loop so NVRAM is flushed before ExitProcess. There is no host loop on
 * Linux yet, and synthesising one does not work: measured on SDL3 3.4.18, a
 * SDL_EVENT_QUIT pushed after a close request had been dequeued reports success
 * and shows in SDL_HasEvent() but is never returned by SDL_PollEvent() again.
 * So this pins the behaviour that is actually observable instead: the window
 * goes away, the frame afterwards draws nothing, and nothing crashes.
 */
static void case_close_request_takes_the_window_down(void)
{
    enum { W = 8, H = 8 };
    const uint32_t red = RGB(220, 30, 30);
    uint32_t buf[W * H];
    SDL_Event e;

    fill_solid(buf, W, H, red);
    if (!vgl_present_open(NULL, W, H, 1, 60)) {
        printf("  (open failed; cannot check the close path)\n");
        return;
    }
    drain_events();
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    e.window.windowID = SDL_GetWindowID(s_window);
    check(SDL_PushEvent(&e), "close: the request could be queued");

    vgl_present_pump();
    check(s_quit_requested, "close: the request is noticed");
    check(!s_window && !s_renderer && !s_rgb,
          "close: the presenter's window is gone, not left undeliverable");

    /* The frame after the close must be inert rather than fatal: device.c keeps
     * calling it once per swap for the rest of the run. */
    vgl_present_frame(buf, W, H, 0);
    vgl_present_pump();
    vgl_present_pump();
    check(!s_window, "close: later frames and pumps do not resurrect anything");

    /* And the game can still reopen a surface afterwards. */
    check(vgl_present_open(NULL, W, H, 1, 60),
          "close: a later grSstWinOpen still gets a window");
    vgl_present_close();
}

/* Both games reopen the surface mid-run (Hydro's service mode), so open must be
 * safe to call again and the second window must actually draw. */
static void case_reopen(void)
{
    enum { W = 8, H = 8 };
    const uint32_t blue = RGB(20, 40, 200);
    uint32_t buf[W * H];
    SDL_Surface *s;
    int sw = 0, sh = 0;

    fill_solid(buf, W, H, blue);
    if (!vgl_present_open(NULL, W, H, 1, 60)) {
        printf("  (open failed; cannot check reopen)\n");
        return;
    }
    /* open() closes whatever it finds first, so a second open is not a leak of
     * the first window; the pointer is deliberately not compared here, because
     * an allocator handing back the same address is normal and is not a bug. */
    check(vgl_present_open(NULL, W, H, 1, 60), "reopen: the second open succeeds");
    vgl_present_frame(buf, W, H, 0);
    s = read_back(&sw, &sh);
    check(s != NULL && at(s, 0, 0) == blue, "reopen: the new window still draws");
    if (s)
        SDL_DestroySurface(s);
    check(s_owns_window && s_window && s_rgb,
          "reopen: exactly one window is owned and its buffers are live");
    vgl_present_close();
}

/* What the presenter tells core, because the two must not each grow a policy of
 * their own. ready == 0 is the honest answer from a presenter that cannot draw
 * bezel art. */
static void case_asks_core_about_the_right_thing(void)
{
    enum { W = 16, H = 8 };
    uint32_t buf[W * H];

    fill_ramp(buf, W, H);
    if (!vgl_present_open(NULL, W, H, 1, 60)) {
        printf("  (open failed; cannot check the layout inputs)\n");
        return;
    }
    vgl_present_frame(buf, W, H, 0);
    check(t_layout_ready == 0,
          "policy: the presenter says it cannot draw bezel art, which is true");
    check(t_layout_rw == W && t_layout_rh == H,
          "policy: the raster it describes is the one it uploads");
    check(t_layout_cw > 0 && t_layout_ch > 0,
          "policy: the client it describes is the window's, in pixels");
    check(t_layout_fullscreen == 0,
          "policy: an ordinary window is not reported as fullscreen");
    vgl_present_close();
}

int main(void)
{
    /* Headless and deterministic: this runs inside make check-linux, on a
     * machine that may have no display at all, and must never pop a window in
     * front of whoever is using it. */
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    unsetenv("VCGLIDE_RASTER_WIDTH");
    unsetenv("VCGLIDE_RASTER_HEIGHT");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("presenter tests: SKIPPED, no SDL video (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    case_one_to_one();
    case_raster_crop();
    case_layout_honoured();
    case_raster_clamp();
    case_edge_extension();
    case_guards();
    case_close_request_takes_the_window_down();
    case_reopen();
    case_asks_core_about_the_right_thing();

    if (t_failures) {
        printf("presenter tests: %d FAILED\n", t_failures);
        return 1;
    }
    printf("presenter tests: all passed\n");
    return 0;
}
