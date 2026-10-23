/* window_posix.c -- native SDL window ownership and ETS window ABI.
 * SPDX-License-Identifier: MIT */
#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

#include "window_posix.h"

struct posix_window {
    SDL_Window *sdl;
    WNDPROC     prev_proc;      /* what GWLP_WNDPROC held before we hooked it */
    BOOL        hooked;
    BOOL        cabinet_placed;
    LONG        style;          /* GetWindowLongA(GWL_STYLE) answer */
    LONG        exstyle;        /* GetWindowLongA(GWL_EXSTYLE) answer */
};

/* The windows this port owns. window.c has exactly one, but the surface is
 * written for a list because EnumWindows takes a callback and a one-element
 * special case would be the kind of thing that breaks the second time a window
 * is needed. */
#define MAX_WINDOWS 4
static posix_window *s_windows[MAX_WINDOWS];
static unsigned      s_window_count;
static int           s_close_requested;

static bool SDLCALL watch_close(void *unused, SDL_Event *event)
{
    (void)unused;
    if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        __atomic_store_n(&s_close_requested, 1, __ATOMIC_RELEASE);
    return true;
}

/* SDL init, refcounted. Same reasoning as the input backends: these entry
 * points can be the first thing in the process to touch SDL. */
static int ensure_video(void)
{
    static int watching;
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0 &&
        !SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        return 0;
    if (!watching) {
        if (!SDL_AddEventWatch(watch_close, NULL))
            return 0;
        watching = 1;
    }
    return 1;
}

/* --- ownership ------------------------------------------------------------ */

BOOL posix_window_create(int w, int h, const char *title, posix_window **out)
{
    posix_window *win;

    if (!out || w <= 0 || h <= 0)
        return FALSE;
    if (s_window_count >= MAX_WINDOWS)
        return FALSE;
    if (!ensure_video())
        return FALSE;

    win = calloc(1, sizeof *win);
    if (!win)
        return FALSE;
    /* Dialog windows float on tiling desktops, allowing each linked cabinet
     * to keep the display/half-display rectangle requested below. */
    if (getenv("VCT_DUAL_CABINET"))
        SDL_SetHint(SDL_HINT_X11_WINDOW_TYPE, "_NET_WM_WINDOW_TYPE_DIALOG");
    win->sdl = SDL_CreateWindow(title ? title : "VCThunder", w, h,
                                SDL_WINDOW_RESIZABLE);
    if (!win->sdl) {
        free(win);
        return FALSE;
    }
    win->prev_proc = DefWindowProcA;
    s_windows[s_window_count++] = win;
    *out = win;
    return TRUE;
}

void posix_window_destroy(posix_window *win)
{
    unsigned i;

    if (!win)
        return;
    for (i = 0; i < s_window_count; i++) {
        if (s_windows[i] == win) {
            s_windows[i] = s_windows[--s_window_count];
            break;
        }
    }
    if (win->sdl)
        SDL_DestroyWindow(win->sdl);
    free(win);
}

HWND posix_window_handle(posix_window *win)
{
    return (HWND)win;
}

void *posix_window_sdl(posix_window *win) { return win ? win->sdl : NULL; }

/* Resolve a handle back to the window it names, or NULL. The one check that
 * matters: a handle that is not one of ours must never be dereferenced, because
 * on this port a "window" can also be the input subsystem's placeholder token. */
static posix_window *resolve(HWND h)
{
    unsigned i;

    if (!h)
        return NULL;
    for (i = 0; i < s_window_count; i++)
        if ((HWND)s_windows[i] == h)
            return s_windows[i];
    return NULL;
}

BOOL posix_window_place_cabinet(HWND h)
{
    const char *value = getenv("VCT_DUAL_CABINET");
    if (!value || (strcmp(value, "1") && strcmp(value, "2"))) return FALSE;
    posix_window *win = resolve(h);
    if (!win) return FALSE;
    if (win->cabinet_placed) return TRUE;
    int cabinet = value[0] - '1', count = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    SDL_Rect area;
    if (!ids || !count || !SDL_GetDisplayUsableBounds(ids[count > 1 ? cabinet : 0], &area)) {
        SDL_free(ids); return FALSE;
    }
    SDL_SetWindowFullscreen(win->sdl, false);
    SDL_SetWindowBordered(win->sdl, false);
    if (count == 1) {
        int half = area.w / 2;
        area.x += half * cabinet;
        area.w = cabinet ? area.w - half : half;
    }
    SDL_SetWindowPosition(win->sdl, area.x, area.y);
    SDL_SetWindowSize(win->sdl, area.w, area.h);
    if (count > 1) SDL_SetWindowFullscreen(win->sdl, true);
    char title[96];
    snprintf(title, sizeof title, "Offroad Thunder - Linked cabinet %d", cabinet + 1);
    SDL_SetWindowTitle(win->sdl, title);
    win->cabinet_placed = TRUE;
    SDL_free(ids);
    return TRUE;
}

/* --- the answers that are real -------------------------------------------- */

int GetSystemMetrics(int index)
{
    SDL_Rect bounds;
    SDL_DisplayID display;

    if (!ensure_video())
        return 0;
    display = SDL_GetPrimaryDisplay();
    if (!display)
        return 0;

    switch (index) {
    case SM_CXSCREEN:
        if (!SDL_GetDisplayBounds(display, &bounds))
            return 0;
        return bounds.w;
    case SM_CYSCREEN:
        if (!SDL_GetDisplayBounds(display, &bounds))
            return 0;
        return bounds.h;
    /* Windows' metric names here mean "the system icon size", which is what a
     * window frame uses to decide its own border thickness. SDL has no system
     * icon on this path, so the conventional desktop values are what make the
     * frame arithmetic in window.c come out the same size it does on Windows. */
    case SM_CXICON:     return 32;
    case SM_CYICON:     return 32;
    case SM_CXSMICON:   return 16;
    case SM_CYSMICON:   return 16;
    case SM_SWAPBUTTON: return 0;
    default:            return 0;
    }
}

BOOL SystemParametersInfoA(UINT action, UINT param, void *data, UINT winini)
{
    SDL_Rect usable;
    RECT *out = (RECT *)data;

    (void)param;
    (void)winini;
    if (action != SPI_GETWORKAREA || !out)
        return FALSE;
    if (!ensure_video())
        return FALSE;
    if (!SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable))
        return FALSE;
    out->left = usable.x;
    out->top = usable.y;
    out->right = usable.x + usable.w;
    out->bottom = usable.y + usable.h;
    return TRUE;
}

int GetClientRect(HWND h, RECT *r)
{
    posix_window *win = resolve(h);
    int w = 0, hgt = 0;

    if (!win || !r)
        return 0;
    if (!SDL_GetWindowSizeInPixels(win->sdl, &w, &hgt))
        return 0;
    /* Win32's client rect is always origin-relative, whatever the window's
     * position. SDL reports a position-relative rect, so the origin is dropped
     * here rather than in every caller -- window.c reads this to size itself and
     * a stray non-zero origin would push the whole window down the screen. */
    r->left = 0;
    r->top = 0;
    r->right = w;
    r->bottom = hgt;
    return 1;
}

BOOL SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    posix_window *win = resolve(h);

    (void)after;
    if (!win)
        return FALSE;
    if (!(flags & SWP_NOSIZE) && cx > 0 && cy > 0)
        SDL_SetWindowSize(win->sdl, cx, cy);
    if (!(flags & SWP_NOMOVE))
        SDL_SetWindowPosition(win->sdl, x, y);
    if (flags & SWP_FRAMECHANGED)
        SDL_SyncWindow(win->sdl);
    return TRUE;
}

BOOL SetWindowTextA(HWND h, LPCSTR text)
{
    posix_window *win = resolve(h);

    if (!win || !text)
        return FALSE;
    return SDL_SetWindowTitle(win->sdl, text);
}

/* --- subclassing, real but unreachable until the game loads --------------- */

/* SetWindowLongPtr(GWLP_WNDPROC) is the one index window.c uses, and it works
 * here for the same reason it works on Windows: the PREVIOUS procedure is what
 * you call to reach the original behaviour, and that is stored and handed back.
 * The difference is only that the thing being subclassed is one we made. */
LONG_PTR SetWindowLongPtrA(HWND h, int index, LONG_PTR value)
{
    posix_window *win = resolve(h);
    LONG_PTR prev;

    if (!win || index != GWLP_WNDPROC)
        return 0;
    prev = (LONG_PTR)win->prev_proc;
    win->prev_proc = (WNDPROC)value;
    win->hooked = TRUE;
    return prev;
}

LONG_PTR SetWindowLongPtrW(HWND h, int index, LONG_PTR value)
{
    return SetWindowLongPtrA(h, index, value);
}

LRESULT CallWindowProcA(WNDPROC prev, HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!prev)
        return DefWindowProcA(h, msg, wp, lp);
    return prev(h, msg, wp, lp);
}

LRESULT CallWindowProcW(WNDPROC prev, HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    return CallWindowProcA(prev, h, msg, wp, lp);
}

/* The default procedure. On Windows this runs the window manager's behaviour,
 * which is what a caller that has nothing better to do delegates to. Here it
 * acknowledges the message and returns 0: no window manager to ask, and the
 * callers in this port either handle what they need or genuinely have nothing
 * to do. It is a real answer, not a crash -- a caller's switch that falls
 * through to here expects 0. */
LRESULT CALLBACK DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)h; (void)wp; (void)lp;

    switch (msg) {
    case WM_SETICON:
    case WM_SETCURSOR:
    case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE:
        return 0;
    default:
        return 0;
    }
}

LRESULT CALLBACK DefWindowProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(h, msg, wp, lp);
}

/* SendMessageA is synchronous, so this CALLS the hooked procedure rather than
 * queueing anything. That is the whole distinction from PostMessage, and
 * window.c depends on it: it sends WM_GETMINMAXINFO and reads the answer before
 * continuing. Queuing would return 0 and it would size the window wrong. */
LRESULT SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    posix_window *win = resolve(h);

    if (win && win->hooked && win->prev_proc)
        return win->prev_proc(h, msg, wp, lp);
    return DefWindowProcA(h, msg, wp, lp);
}

BOOL EnumWindows(BOOL (CALLBACK *cb)(HWND, LPARAM), LPARAM opaque)
{
    unsigned i;

    if (!cb)
        return FALSE;
    for (i = 0; i < s_window_count; i++)
        if (!cb((HWND)s_windows[i], opaque))
            return FALSE;
    return TRUE;
}

BOOL GetClassNameA(HWND h, LPSTR buf, int max)
{
    if (!resolve(h) || !buf || max <= 0)
        return FALSE;
    snprintf(buf, (size_t)max, "posix_window");
    return TRUE;
}

BOOL IsWindowVisible(HWND h)
{
    posix_window *win = resolve(h);

    return win && SDL_GetWindowFlags(win->sdl) & SDL_WINDOW_HIDDEN ? FALSE : TRUE;
}

DWORD GetWindowThreadProcessId(HWND h, DWORD *pid)
{
    if (!resolve(h))
        return 0;
    /* The port's own windows belong to this process by construction -- they are
     * our own SDL windows. Returning 0 is what the caller reads as "no window",
     * and returning a real thread id with no thread to go with would be a lie. */
    if (pid)
        *pid = (DWORD)getpid();
    return 1;
}

HWND MonitorFromWindow(HWND h, DWORD flags)
{
    (void)flags;
    /* One display is assumed, which is what a cabinet-in-a-cabinete case is. A
     * multi-monitor arrangement would need SDL_GetDisplayForWindow here, and
     * window.c's use of this is only to ask which monitor's work area applies. */
    return resolve(h) ? (HWND)&s_windows[0] : NULL;
}

BOOL GetMonitorInfoA(HMONITOR monitor, LPMONITORINFO mi)
{
    SDL_Rect bounds, usable;
    SDL_DisplayID display;

    (void)monitor;
    if (!mi || mi->cbSize < sizeof *mi)
        return FALSE;
    if (!ensure_video())
        return FALSE;
    display = SDL_GetPrimaryDisplay();
    if (!display)
        return FALSE;
    if (!SDL_GetDisplayBounds(display, &bounds))
        return FALSE;
    mi->rcMonitor.left = bounds.x;
    mi->rcMonitor.top = bounds.y;
    mi->rcMonitor.right = bounds.x + bounds.w;
    mi->rcMonitor.bottom = bounds.y + bounds.h;
    /* The work area is the monitor minus the taskbar and any dock. SDL reports
     * it directly; window.c uses it to clamp the window scale, so returning the
     * full monitor here would let a full-scale window hide under the panel. */
    if (SDL_GetDisplayUsableBounds(display, &usable)) {
        mi->rcWork.left = usable.x;
        mi->rcWork.top = usable.y;
        mi->rcWork.right = usable.x + usable.w;
        mi->rcWork.bottom = usable.y + usable.h;
    } else {
        mi->rcWork = mi->rcMonitor;
    }
    mi->dwFlags = MONITORINFOF_PRIMARY;
    return TRUE;
}

/* --- cursor ---------------------------------------------------------------
 *
 * LoadCursorA is a request for one of the system cursors and SetCursor installs
 * it. SDL has cursors, but this port asks for IDC_ARROW only and never draws a
 * custom one, so returning NULL from LoadCursorA and accepting it in SetCursor
 * is the honest answer: there is no cursor object here to hand back. Returning
 * a fake non-NULL handle would let a caller believe it owns one. */
HCURSOR LoadCursorA(HINSTANCE inst, LPCSTR name)
{
    (void)inst;
    (void)name;
    return NULL;
}

HCURSOR SetCursor(HCURSOR c)
{
    return c;
}

BOOL GetCursorPos(POINT *p)
{
    float x = 0.0f, y = 0.0f;

    if (!p)
        return FALSE;
    if (!SDL_GetMouseState(&x, &y))
        return FALSE;
    p->x = (LONG)x;
    p->y = (LONG)y;
    return TRUE;
}

/* --- the deferred half: styles, placement, icons, capture, keyboard ------- */

LONG GetWindowLongA(HWND h, int index)
{
    posix_window *w = resolve(h);

    if (!w)
        return 0;
    if (index == GWL_STYLE)
        return w->style;
    if (index == GWL_EXSTYLE)
        return w->exstyle;
    return 0;
}

LONG SetWindowLongA(HWND h, int index, LONG value)
{
    posix_window *w = resolve(h);
    LONG prev = 0;

    if (!w)
        return 0;
    if (index == GWL_STYLE) {
        prev = w->style;
        w->style = value;
    } else if (index == GWL_EXSTYLE) {
        prev = w->exstyle;
        w->exstyle = value;
    }
    return prev;
}

/* No icon source on Linux; window.c treats a NULL icon as "no icon". */
void *LoadImageA(HINSTANCE inst, LPCSTR name, UINT type, int cx, int cy,
                 UINT flags)
{
    (void)inst; (void)name; (void)type; (void)cx; (void)cy; (void)flags;
    return NULL;
}

HWND GetConsoleWindow(void) { return NULL; }

BOOL IsWindow(HWND h) { return resolve(h) != NULL; }

HWND GetForegroundWindow(void)
{
    SDL_Window *focused = SDL_GetKeyboardFocus();
    unsigned i;
    for (i = 0; i < s_window_count; i++)
        if (s_windows[i]->sdl == focused)
            return (HWND)s_windows[i];
    return NULL;
}

BOOL posix_window_fullscreen(HWND h, int enabled)
{
    posix_window *win = resolve(h);
    return win && SDL_SetWindowFullscreen(win->sdl, enabled != 0);
}

HWND GetCapture(void) { return NULL; }
BOOL ReleaseCapture(void) { return FALSE; }
HWND SetCapture(HWND h) { (void)h; return NULL; }

BOOL GetWindowRect(HWND h, RECT *r)
{
    posix_window *w = resolve(h);
    int x, y, ww, wh;

    if (!w || !r)
        return FALSE;
    SDL_GetWindowPosition(w->sdl, &x, &y);
    SDL_GetWindowSize(w->sdl, &ww, &wh);
    r->left = x;
    r->top = y;
    r->right = x + ww;
    r->bottom = y + wh;
    return TRUE;
}

/* SDL pumps events on the game thread; the input poller reads this snapshot. */
SHORT GetAsyncKeyState(int vk)
{
    SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
    const bool *keys;
    if (vk >= 'A' && vk <= 'Z')
        sc = SDL_SCANCODE_A + vk - 'A';
    else if (vk >= '1' && vk <= '9')
        sc = SDL_SCANCODE_1 + vk - '1';
    else if (vk >= VK_F1 && vk <= VK_F1 + 11)
        sc = SDL_SCANCODE_F1 + vk - VK_F1;
    else if (vk >= VK_F1 + 12 && vk <= VK_F24)
        sc = SDL_SCANCODE_F13 + vk - VK_F1 - 12;
    else switch (vk) {
    case '0': sc = SDL_SCANCODE_0; break;
    case VK_ESCAPE: sc = SDL_SCANCODE_ESCAPE; break;
    case VK_RETURN: sc = SDL_SCANCODE_RETURN; break;
    case VK_SPACE: sc = SDL_SCANCODE_SPACE; break;
    case VK_LEFT: sc = SDL_SCANCODE_LEFT; break;
    case VK_RIGHT: sc = SDL_SCANCODE_RIGHT; break;
    case VK_UP: sc = SDL_SCANCODE_UP; break;
    case VK_DOWN: sc = SDL_SCANCODE_DOWN; break;
    case VK_LCONTROL: sc = SDL_SCANCODE_LCTRL; break;
    case VK_RCONTROL: sc = SDL_SCANCODE_RCTRL; break;
    case VK_LMENU: sc = SDL_SCANCODE_LALT; break;
    case VK_RMENU: sc = SDL_SCANCODE_RALT; break;
    case VK_MENU:
        keys = SDL_GetKeyboardState(NULL);
        return keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT] ? (SHORT)0x8000 : 0;
    case VK_LSHIFT: sc = SDL_SCANCODE_LSHIFT; break;
    case VK_RSHIFT: sc = SDL_SCANCODE_RSHIFT; break;
    case VK_BACK: sc = SDL_SCANCODE_BACKSPACE; break;
    case VK_TAB: sc = SDL_SCANCODE_TAB; break;
    case VK_HOME: sc = SDL_SCANCODE_HOME; break;
    case VK_END: sc = SDL_SCANCODE_END; break;
    case VK_PRIOR: sc = SDL_SCANCODE_PAGEUP; break;
    case VK_NEXT: sc = SDL_SCANCODE_PAGEDOWN; break;
    case VK_INSERT: sc = SDL_SCANCODE_INSERT; break;
    case VK_DELETE: sc = SDL_SCANCODE_DELETE; break;
    case VK_OEM_MINUS: sc = SDL_SCANCODE_MINUS; break;
    case VK_OEM_PLUS: sc = SDL_SCANCODE_EQUALS; break;
    case VK_OEM_COMMA: sc = SDL_SCANCODE_COMMA; break;
    case VK_OEM_PERIOD: sc = SDL_SCANCODE_PERIOD; break;
    }
    keys = SDL_GetKeyboardState(NULL);
    return sc != SDL_SCANCODE_UNKNOWN && keys[sc] ? (SHORT)0x8000 : 0;
}

BOOL IsWindowUnicode(HWND h) { (void)h; return FALSE; }

BOOL GetWindowPlacement(HWND h, WINDOWPLACEMENT *wp)
{
    if (!wp)
        return FALSE;
    wp->length = sizeof *wp;
    (void)h;
    return TRUE;
}

BOOL SetWindowPlacement(HWND h, const WINDOWPLACEMENT *wp)
{
    (void)h; (void)wp;
    return TRUE;
}

BOOL IsIconic(HWND h) { (void)h; return FALSE; }

/* Latch close requests even when an SDL subsystem has already pumped them. */
static void (*s_event_handler)(const void *);
void posix_window_set_event_handler(void (*handler)(const void *))
{
    s_event_handler = handler;
}

BOOL PeekMessageA(MSG *msg, HWND h, UINT min, UINT max, UINT remove)
{
    SDL_Event event;
    (void)h; (void)min; (void)max; (void)remove;
    SDL_PumpEvents();
    if (__atomic_exchange_n(&s_close_requested, 0, __ATOMIC_ACQ_REL)) {
        memset(msg, 0, sizeof *msg);
        msg->message = WM_CLOSE;
        return TRUE;
    }
    while (SDL_PollEvent(&event)) {
        if (s_event_handler) s_event_handler(&event);
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            memset(msg, 0, sizeof *msg);
            msg->message = WM_CLOSE;
            return TRUE;
        }
    }
    return FALSE;
}

BOOL TranslateMessage(const MSG *msg) { (void)msg; return FALSE; }

LONG DispatchMessageA(const MSG *msg) { (void)msg; return 0; }
