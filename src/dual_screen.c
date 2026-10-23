/* Two to four independent Offroad cabinets using the original UDP link.
 * SPDX-License-Identifier: MIT */
#include "vcthunder.h"
#include "dual_screen.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern volatile sig_atomic_t g_linux_quit;
static int (__cdecl *s_operator_init)(void);

int dual_screen_cabinet(void)
{
    const char *v = getenv("VCT_DUAL_CABINET");
    return v && v[0] >= '1' && v[0] <= '4' && !v[1] ? v[0] - '0' : 0;
}

static int __cdecl linked_operator_init(void)
{
    int result = s_operator_init();
    /* Offroad's operator menu stores units 1..4 as indices 0..3. Apply after
     * reading NVRAM and before GameInit copies these into the network module. */
    *(uint8_t *)(uintptr_t)(G->operator_settings + 0x9c) = 0;
    *(uint32_t *)(uintptr_t)(G->operator_settings + 0xa0) = dual_screen_cabinet() - 1;
    LOGI("link: Offroad cabinet %d, linked mode", dual_screen_cabinet());
    return result;
}

int dual_screen_install(void)
{
    if (!dual_screen_cabinet()) return 1;
    if (strcmp(G->id, "offroad") || !G->operator_init || !G->operator_settings ||
        !G->hook_operator_init) return 0;
    s_operator_init = hook_prepare_call_through(G->operator_init, G->hook_operator_init);
    if (!s_operator_init) return 0;
    patch_jmp(G->operator_init, linked_operator_init);
    return 1;
}

static int mkdirs(const char *path)
{
    char copy[1024];
    if (snprintf(copy, sizeof copy, "%s", path) >= (int)sizeof copy) return 0;
    for (char *p = copy; *p; p++) if (*p == '\\') *p = '/';
    for (char *p = copy + 1; ; p++) {
        if (*p != '/' && *p) continue;
        char end = *p; *p = 0;
        int result = mkdir(copy, 0755);
        *p = end;
        if (result && errno != EEXIST) return 0;
        if (!end) return 1;
    }
}

int dual_screen_run(int argc, char **argv)
{
    shim_config_t cfg;
    char *filtered[66];
    if (argc > 64) { fprintf(stderr, "Too many arguments.\n"); return 1; }
    int count = 0, players = 2;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--dual-screen")) continue;
        if (!strcmp(argv[i], "--players")) {
            if (++i >= argc || argv[i][0] < '2' || argv[i][0] > '4' || argv[i][1]) {
                fprintf(stderr, "--players requires 2, 3 or 4.\n"); return 1;
            }
            players = argv[i][0] - '0';
            continue;
        }
        filtered[count++] = argv[i];
    }
    filtered[count] = NULL;
    config_defaults(&cfg);
    config_load_shared(&cfg, "vcthunder.ini");
    config_load_title(&cfg, "vcthunder.ini", "offroad");
    snprintf(cfg.title, sizeof cfg.title, "offroad");
    if (!config_parse_argv(&cfg, count, filtered)) return 1;
    if (strcasecmp(cfg.title, "offroad")) {
        fprintf(stderr, "--dual-screen currently supports Offroad Thunder.\n"); return 1;
    }
    char saves[4][1024], logs[4][1100], glogs[4][1100], exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) { perror("/proc/self/exe"); return 1; }
    exe[n] = 0;
    /* Positioning side-by-side windows is supported by X11/Xwayland. */
    if (!getenv("SDL_VIDEODRIVER") && getenv("DISPLAY")) setenv("SDL_VIDEODRIVER", "x11", 1);
    int displays = 0;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        SDL_DisplayID *ids = SDL_GetDisplays(&displays);
        SDL_free(ids); SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    printf("Offroad: %d linked cabinets; %s\n", players,
        displays >= players ? "one per display" :
        displays <= 1 && players > 2 ? "2 x 2 grid on one display" :
        displays <= 1 ? "side by side on one display" : "tiled across connected displays");
    for (int i = 0; i < players; i++) {
        if (snprintf(saves[i], sizeof saves[i], "%s/offroad-link/cabinet-%d", cfg.save_dir, i + 1) >= (int)sizeof saves[i]) return 1;
        snprintf(logs[i], sizeof logs[i], "%s/game.log", saves[i]);
        snprintf(glogs[i], sizeof glogs[i], "%s/glide.log", saves[i]);
        printf("Cabinet %d: controller %d; saves %s\n", i + 1, i + 1, saves[i]);
        if (!cfg.dry_run && !mkdirs(saves[i])) { perror(saves[i]); return 1; }
    }
    if (cfg.dry_run) return 0;
    fflush(NULL);
    pid_t children[4] = {0};
    int result = 0;
    for (int i = 0; i < players; i++) {
        children[i] = fork();
        if (!children[i]) {
            char cabinet[] = {(char)('1' + i), 0}, pad[] = {(char)('0' + i), 0};
            char total[] = {(char)('0' + players), 0};
            setenv("VCT_DUAL_CABINET", cabinet, 1);
            setenv("VCT_GAMEPAD_INDEX", pad, 1);
            setenv("VCT_LINK_PLAYERS", total, 1);
            char *args[80]; int a = 0;
            args[a++] = exe;
            for (int j = 1; j < count; j++) if (strcmp(filtered[j], "--dry-run")) args[a++] = filtered[j];
            args[a++] = "offroad"; args[a++] = "--link"; args[a++] = "local";
            args[a++] = "--save"; args[a++] = saves[i];
            args[a++] = "--log"; args[a++] = logs[i];
            args[a++] = "--glide-log"; args[a++] = glogs[i];
            args[a] = NULL;
            execv(exe, args); perror(exe); _exit(127);
        }
        if (children[i] < 0) { children[i] = 0; perror("fork"); result = 1; goto done; }
    }
    while (!g_linux_quit) {
        for (int i = 0; i < players; i++) {
            int status;
            pid_t p = waitpid(children[i], &status, WNOHANG);
            if (p == children[i]) {
                children[i] = 0;
                result = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
                goto done;
            }
        }
        SDL_Delay(100);
    }
done:
    for (int i = 0; i < players; i++) if (children[i]) kill(children[i], SIGTERM);
    for (int i = 0; i < players; i++) if (children[i]) {
        while (waitpid(children[i], NULL, 0) < 0 && errno == EINTR) {}
    }
    return result;
}
