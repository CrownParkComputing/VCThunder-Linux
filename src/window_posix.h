/* window_posix.h -- native SDL window ownership and ETS window ABI.
 * SPDX-License-Identifier: MIT
 * HWND identifies a port-owned SDL window. The retained message and window
 * types allow the original game and host policy to use their measured ABI.
 */
#ifndef VCTH_WINDOW_POSIX_H
#define VCTH_WINDOW_POSIX_H


#include "posix_compat.h"

/* --- the scalar types window.c passes messages around in -------------------
 *
 * WPARAM, LPARAM and LRESULT are already in posix_compat.h as the Win32 message
 * types are; only LONG_PTR is added here, because SetWindowLongPtr needs it and
 * nothing else in the compat layer does. */

/* Pointer-sized, not a fixed width. On this 32-bit target that makes it four
 * bytes, which is what SetWindowLongPtr needs: a WNDPROC and a LONG_PTR are the
 * same size, and the round trip through this type is a no-op. A `long long`
 * here would be eight bytes and every cast between a function pointer and this
 * type would be a truncation the compiler rightly objects to. */
typedef long                LONG_PTR;

typedef struct tagRECT {
    LONG left, top, right, bottom;
} RECT, *PRECT, *LPRECT;

typedef struct tagPOINT {
    LONG x, y;
} POINT, *LPOINT;

typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);

/* LoadCursorA's return type. A cursor handle is opaque here for the same
 * reason a window handle is: nothing outside this file inspects one. */
typedef void *HCURSOR;

/* MonitorFromWindow returns one of these and GetMonitorInfoA takes one back. It
 * is an opaque token rather than a display index so that a caller cannot start
 * depending on which display a window landed on. */
typedef void *HMONITOR;

/* GetMonitorInfoA. Only the work-area rectangle is ever read; the rest is
 * present so the struct is the size the caller passes. */
typedef struct tagMONITORINFO {
    DWORD cbSize;
    RECT  rcMonitor;
    RECT  rcWork;
    DWORD dwFlags;
} MONITORINFO, *PMONITORINFO, *LPMONITORINFO;

#define MONITORINFOF_PRIMARY  0x00000001

/* MonitorFromWindow's second argument. */
#define MONITOR_DEFAULTTONEAREST  0x00000002

/* SetWindowLongPtr's index and return. 32-bit Linux is a 32-bit pointer target
 * like Win32, so LONG_PTR is pointer-sized and the values line up. */
#define GWLP_WNDPROC  (-4)

/* Styles and ex-styles window.c applies. Only the ones it names are defined;
 * an undefined one is a compile error rather than a silently-zero style, which
 * is the failure mode that would strip a border and leave nobody knowing why. */
#define WS_POPUP          0x80000000L
#define WS_CAPTION        0x00C00000L
#define WS_THICKFRAME     0x00040000L
#define WS_MINIMIZE       0x00020000L
#define WS_MAXIMIZE       0x00010000L
#define WS_MAXIMIZEBOX    0x00020000L
#define WS_DLGFRAME       0x00400000L

#define WS_EX_CLIENTEDGE  0x00000200L
#define WS_EX_STATICEDGE  0x00020000L
#define WS_EX_WINDOWEDGE  0x00000100L
#define WS_EX_DLGMODALFRAME 0x00000001L

/* GetSystemMetrics indices. */
#define SM_CXSCREEN      0
#define SM_CYSCREEN      1
#define SM_CXICON        11
#define SM_CYICON        12
#define SM_CXSMICON      49
#define SM_CYSMICON      50
#define SM_SWAPBUTTON    34

/* SystemParametersInfo. */
#define SPI_GETWORKAREA  0x0030

/* SetWindowPos. */
#define SWP_NOSIZE       0x0001
#define SWP_NOMOVE       0x0002
#define SWP_NOZORDER     0x0004
#define SWP_NOACTIVATE   0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_NOOWNERZORDER 0x0200
#define SWP_NOSENDCHANGING 0x0400

typedef const WCHAR *LPCWSTR;

/* GetWindowLongA / SetWindowLongA indices window.c reads and writes. */
#define GWL_STYLE      (-16)
#define GWL_EXSTYLE    (-20)

/* Icons. LoadImageA has no PNG/ICO source on Linux and returns NULL, which
 * window.c already treats as "no icon"; these constants keep the call sites
 * compiling. */
#define IMAGE_ICON      1
#define LR_SHARED       0x00008000
#define LR_DEFAULTCOLOR 0
#define ICON_BIG        1
#define ICON_SMALL      0

/* The virtual keys window.c polls for edge/capture logic. GetAsyncKeyState
 * answers 0, so these only ever read "not held". */
#define VK_LBUTTON      0x01
#define VK_RBUTTON      0x02
#define VK_MENU         0x12

/* WM_NCHITTEST answers. */
#define HTCAPTION       2
#define HTCLIENT        1

#define HWND_TOP        ((HWND)0)

#define LOWORD(l)  ((WORD)((DWORD)(l) & 0xFFFFu))
#define HIWORD(l)  ((WORD)((DWORD)(l) >> 16))
#define MAKEINTRESOURCEA(i) ((LPCSTR)(uintptr_t)(i))

/* GetWindowPlacement / SetWindowPlacement. Only `.length` is touched (the
 * caller sizes the struct), so the geometry fields are carried for shape. */
typedef struct tagWINDOWPLACEMENT {
    UINT  length;
    UINT  flags;
    UINT  showCmd;
    POINT ptMinPosition;
    POINT ptMaxPosition;
    RECT  rcNormalPosition;
} WINDOWPLACEMENT, *LPWINDOWPLACEMENT;

#define IDC_ARROW        ((LPCSTR)32512)

/* The messages src/window.c actually handles. WM_NCHITTEST is NOT here because
 * it is not used: the hit testing the port needs is in the window procedure
 * window.c itself installs, not in the system's. */
#define WM_MOUSEMOVE         0x0200
#define WM_LBUTTONUP         0x0202
#define WM_NCMOUSEMOVE       0x00A0
#define WM_NCLBUTTONDOWN     0x00A1
#define WM_NCLBUTTONUP       0x00A2
#define WM_SIZE              0x0005
#define WM_GETMINMAXINFO     0x0024
#define WM_SETICON           0x0080
#define WM_SETCURSOR         0x0020
#define WM_CAPTURECHANGED    0x0215
#define WM_WINDOWPOSCHANGED  0x0047
#define WM_ENTERSIZEMOVE     0x0231
#define WM_EXITSIZEMOVE      0x0232
#define WM_ENTERMENULOOP     0x0211
#define WM_EXITMENULOOP      0x0212
#define WM_QUIT              0x0012
#define WM_CLOSE             0x0010
#define WM_SYSCOMMAND        0x0112
#define SC_CLOSE             0xF060
#define PM_REMOVE            0x0001

/* PeekMessageA's message struct. The Linux port's event loop is SDL's, so
 * PeekMessageA answers "no message"; the struct exists so the call sites
 * compile and read the same shape they do on Windows. */
typedef struct tagMSG {
    HWND   hwnd;
    UINT   message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD  time;
    POINT  pt;
} MSG, *LPMSG;

/* --- the implementation ---------------------------------------------------
 *
 * Split by whether the answer is real on Linux or is deferred:
 *
 *   ANSWERABLE NOW -- GetSystemMetrics, SystemParametersInfoA (work area),
 *   GetClientRect, SetWindowTextA, SetWindowPos. SDL knows all of these, and
 *   window.c's scale and bezel arithmetic depends on them being RIGHT rather
 *   than merely present.
 *
 *   DEFERRED UNTIL THE LOADER EXISTS -- EnumWindows, the subclassing trio.
 *   There is no window to enumerate and none to subclass until a game hands one
 *   over. They are implemented against posix_window so the logic is real, and
 *   they are inert until then rather than absent.
 */

/* The window this port owns. Defined in window_posix.c; opaque here because
 * window.c must not reach inside one. */
typedef struct posix_window posix_window;

BOOL     posix_window_create(int w, int h, const char *title,
                             posix_window **out);
void     posix_window_destroy(posix_window *win);
/* What src/glide_bind.c's window_set_hwnd() is given on Linux: a handle the
 * port owns, standing in for the game's. NULL until the loader supplies one. */
HWND     posix_window_handle(posix_window *win);
void    *posix_window_sdl(posix_window *win);
BOOL     posix_window_fullscreen(HWND win, int enabled);
BOOL     posix_window_place_cabinet(HWND win);
void     posix_window_set_event_handler(void (*handler)(const void *));

BOOL EnumWindows(BOOL (CALLBACK *cb)(HWND, LPARAM), LPARAM opaque);
BOOL GetClassNameA(HWND h, LPSTR buf, int max);
BOOL IsWindowVisible(HWND h);
DWORD GetWindowThreadProcessId(HWND h, DWORD *pid);
int   GetClientRect(HWND h, RECT *r);
BOOL  SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags);
BOOL  SetWindowTextA(HWND h, LPCSTR text);
int   GetSystemMetrics(int index);
BOOL  SystemParametersInfoA(UINT action, UINT param, void *data, UINT winini);
HWND  MonitorFromWindow(HWND h, DWORD flags);
BOOL  GetMonitorInfoA(HMONITOR monitor, LPMONITORINFO mi);
LONG_PTR SetWindowLongPtrA(HWND h, int index, LONG_PTR value);
LONG_PTR SetWindowLongPtrW(HWND h, int index, LONG_PTR value);
LRESULT CallWindowProcA(WNDPROC prev, HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CallWindowProcW(WNDPROC prev, HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK DefWindowProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp);
HCURSOR LoadCursorA(HINSTANCE inst, LPCSTR name);
HCURSOR SetCursor(HCURSOR c);
BOOL  GetCursorPos(POINT *p);

/* The windowing surface window.c still needs. These are the deferred half:
 * styles, placement, icons, capture/focus and keyboard, all of which either
 * answer a plausible value or are no-ops until the loader supplies a window. */
LONG  GetWindowLongA(HWND h, int index);
LONG  SetWindowLongA(HWND h, int index, LONG value);
void *LoadImageA(HINSTANCE inst, LPCSTR name, UINT type, int cx, int cy,
                 UINT flags);
HWND  GetConsoleWindow(void);
BOOL  IsWindow(HWND h);
HWND  GetForegroundWindow(void);
HWND  GetCapture(void);
BOOL  ReleaseCapture(void);
HWND  SetCapture(HWND h);
BOOL  GetWindowRect(HWND h, RECT *r);
SHORT GetAsyncKeyState(int vk);
BOOL  IsWindowUnicode(HWND h);
BOOL  GetWindowPlacement(HWND h, WINDOWPLACEMENT *wp);
BOOL  SetWindowPlacement(HWND h, const WINDOWPLACEMENT *wp);
BOOL  IsIconic(HWND h);

/* The message loop. SDL owns the real one on Linux; these keep the call sites
 * compiling and report "no message" so the port's own pump() drives itself. */
BOOL PeekMessageA(MSG *msg, HWND h, UINT min, UINT max, UINT remove);
BOOL TranslateMessage(const MSG *msg);
LONG DispatchMessageA(const MSG *msg);

#endif /* VCTH_WINDOW_POSIX_H */
