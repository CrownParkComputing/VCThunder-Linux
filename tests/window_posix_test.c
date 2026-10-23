/* A close request must survive other SDL event consumers until the host exits. */
/* SPDX-License-Identifier: MIT */
#include "window_posix.c"

int main(void)
{
    posix_window *win;
    SDL_Event event = { 0 }, consumed;
    MSG msg;
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (!posix_window_create(32, 24, "window test", &win))
        return 1;
    event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    event.window.windowID = SDL_GetWindowID(win->sdl);
    if (!SDL_PushEvent(&event))
        return 1;
    while (SDL_PollEvent(&consumed))
        ;
    if (!PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) || msg.message != WM_CLOSE) {
        puts("FAIL: a consumed SDL close request must still reach the host");
        return 1;
    }
    posix_window_destroy(win);
    SDL_Quit();
    puts("window posix tests: close request survives SDL event consumption");
    return 0;
}
