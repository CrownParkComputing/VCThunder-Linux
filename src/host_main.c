/* host_main.c -- the Linux host's native entry point.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The game span is claimed by mmap in game_map(). The native entry point calls
 * shim_run() directly, or supervises two linked Offroad cabinet processes.
 */
#include "vcthunder.h"
#include <signal.h>
#include "dual_screen.h"
#include "../menu/native_menu.h"

/* The real command line, handed to main.c's split_cmdline() on Linux. */
int    g_linux_argc;
char **g_linux_argv;
volatile sig_atomic_t g_linux_quit;

static void request_quit(int sig) { (void)sig; g_linux_quit = 1; }

int main(int argc, char **argv)
{
    g_linux_argc = argc;
    g_linux_argv = argv;
    signal(SIGINT, request_quit);
    signal(SIGTERM, request_quit);
    atexit(native_menu_cleanup);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--dual-screen")) return dual_screen_run(argc, argv);
    int force_setup = argc == 2 && strcmp(argv[1], "--menu") == 0;
    int preferred = argc == 2 && !strcasecmp(argv[1], "hydro") ? 0 :
                    argc == 2 && !strcasecmp(argv[1], "offroad") ? 1 : -1;
    if (argc == 1 || force_setup || preferred >= 0) {
        int game = native_menu_launcher(preferred, force_setup);
        if (!game) return 0;
        char *game_argv[] = {argv[0], game == 1 ? "hydro" : "offroad", NULL};
        g_linux_argc = 2;
        g_linux_argv = game_argv;
        return shim_run();
    }
    return shim_run();
}
