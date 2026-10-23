/* SPDX-License-Identifier: GPL-2.0-or-later
 * VCThunder native menu. Menubar, row navigation and setting rows adapted from
 * DriveClub-Linux/runtime-linux/gpu/shim/nk_menu.cpp (bbport), itself modelled
 * on CrownParkComputing/native-arcade-ports. See COPYING for the GPL.
 * Nuklear is MIT/public domain. SDL rendering avoids a second graphics stack.
 */
#include "native_menu.h"
#include "vcthunder.h"
#include "settings.h"
#include "ini_doc.h"
#include "io.h"
#include "xinput_posix.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#include "nuklear.h"

extern const char vct_import_linux[];
extern volatile sig_atomic_t g_linux_quit;
static struct nk_context ctx;
static struct nk_font_atlas atlas;
static struct nk_font *font;
static struct nk_draw_null_texture null_tex;
static struct nk_buffer commands, vertices, elements;
static SDL_Texture *font_texture;
static SDL_Renderer *font_renderer;
static void *font_pixels;
static int font_w, font_h, ready, launcher, tab, row;
static int open_menu, swallow_escape, selected, request;
static int nav_moved;
static char status[2048] = "Select a game, or import your CHD / game files.";
static ini_doc_t document;
static int doc_ready;
static const char *tabs[] = {"File", "Display", "Sound", "Controls", "Performance", "Setup"};
enum { FILE_TAB, DISPLAY_TAB, SOUND_TAB, CONTROLS_TAB, PERFORMANCE_TAB, SETUP_TAB, NTABS };
static int show_fps = 1;
static double game_fps;
static unsigned fps_frames;
static Uint64 fps_epoch;
enum { UP, DOWN, LEFT, RIGHT, OK, BACK, PREV, NEXT };
static const setting_desc_t *rows[NTABS][32];
static int counts[NTABS];
static SDL_Event events[256];
static unsigned nevents;
static pid_t import_pid;
static int import_fd = -1, import_progress;
static char import_line[4096];
static size_t import_used;
static pthread_mutex_t picker_lock = PTHREAD_MUTEX_INITIALIZER;
static char *picked_paths[64];
static unsigned picked_count;
static int picker_pending, picker_cancelled;
static char picker_error[512];

static int cycle(int x, int d, int n) { return n ? (x + d + n) % n : 0; }
static int is_open(void) { return __atomic_load_n(&open_menu, __ATOMIC_ACQUIRE); }
int native_menu_blocks_input(void)
{
    return is_open() || __atomic_load_n(&swallow_escape, __ATOMIC_ACQUIRE);
}

static void set_open(int on)
{
    if (is_open() == on) return;
    __atomic_store_n(&open_menu, on, __ATOMIC_RELEASE);
    if (!launcher) timebase_pause(on);
    if (on) { row = 0; nav_moved = 1; }
    else __atomic_store_n(&swallow_escape, 1, __ATOMIC_RELEASE);
}

static const char *data_root(void)
{
    /* main.c resolves this to data/<title> for a running game. The importer
     * always uses the shared root, never that title's directory. */
    static char root[INI_VALUE_CAP];
    if (!doc_ready || !ini_doc_get(&document, "paths", "data_dir", root, sizeof root))
        snprintf(root, sizeof root, "data");
    for (char *p = root; *p; p++) if (*p == '\\') *p = '/';
    return root;
}

static int installed(int game)
{
    char path[2048];
    static const char *dirs[] = {"hydro", "offroad"};
    static const char *exes[] = {"HYDRO.EXE", "offroad3.exe"};
    snprintf(path, sizeof path, "%s/%s/%s", data_root(), dirs[game], exes[game]);
    if (access(path, R_OK) == 0) return 1;
    /* Imports preserve original spelling; accept the common alternate case. */
    char *base = strrchr(path, '/');
    if (base) for (char *p = base + 1; *p; p++)
        *p = game ? (char)toupper((unsigned char)*p) : (char)tolower((unsigned char)*p);
    return access(path, R_OK) == 0;
}

static void setup_rows(void)
{
    for (unsigned i = 0; i < VCT_UI_SETTING_COUNT; i++) {
        const setting_desc_t *s = &vct_ui_setting[i];
        int page = -1;
        if (!strcmp(s->section, "graphics") &&
            (!strcmp(s->key, "window_scale") || !strcmp(s->key, "fullscreen") ||
             !strcmp(s->key, "aspect") || !strcmp(s->key, "widescreen") ||
             !strcmp(s->key, "vcglide_renderer") || !strcmp(s->key, "render_scale") ||
             !strcmp(s->key, "texmag") || !strcmp(s->key, "upscale_2d"))) page = DISPLAY_TAB;
        if (!strcmp(s->section, "timing") && !strcmp(s->key, "speed_percent")) page = PERFORMANCE_TAB;
        if (!strcmp(s->section, "graphics") && !strcmp(s->key, "vcglide_threads")) page = PERFORMANCE_TAB;
        if (!strcmp(s->section, "audio") && strcmp(s->key, "dcs_us")) page = SOUND_TAB;
        if ((!strcmp(s->section, "analog") &&
             (!strncmp(s->key, "xinput_", 7) || !strcmp(s->key, "provider") ||
              !strcmp(s->key, "deadzone_percent"))) ||
            (!strcmp(s->section, "ffb") && !strcmp(s->key, "rumble")) ||
            (!strcmp(s->section, "controls") && !strcmp(s->key, "input_background")))
            page = CONTROLS_TAB;
        if (page >= 0 && counts[page] < 32) rows[page][counts[page]++] = s;
    }
}

static void init_ui(void)
{
    if (ready) return;
    nk_font_atlas_init_default(&atlas);
    nk_font_atlas_begin(&atlas);
    static const char *font_paths[] = {"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    for (unsigned i = 0; i < sizeof font_paths / sizeof *font_paths && !font; i++)
        if (access(font_paths[i], R_OK) == 0) font = nk_font_atlas_add_from_file(&atlas, font_paths[i], 19, NULL);
    if (!font) font = nk_font_atlas_add_default(&atlas, 20, NULL);
    const void *pixels = nk_font_atlas_bake(&atlas, &font_w, &font_h, NK_FONT_ATLAS_RGBA32);
    font_pixels = malloc((size_t)font_w * font_h * 4);
    if (!pixels || !font_pixels) abort();
    memcpy(font_pixels, pixels, (size_t)font_w * font_h * 4);
    nk_font_atlas_end(&atlas, nk_handle_id(1), &null_tex);
    if (!font) abort();
    nk_init_default(&ctx, &font->handle);
    nk_buffer_init_default(&commands);
    nk_buffer_init_default(&vertices);
    nk_buffer_init_default(&elements);
    struct nk_color colors[NK_COLOR_COUNT];
    nk_style_default(&ctx);
    for (int i = 0; i < NK_COLOR_COUNT; i++) colors[i] = nk_rgb(47, 53, 65);
    colors[NK_COLOR_TEXT] = nk_rgb(233, 237, 243);
    colors[NK_COLOR_WINDOW] = nk_rgba(22, 27, 36, 250);
    colors[NK_COLOR_HEADER] = nk_rgb(30, 37, 48);
    colors[NK_COLOR_BORDER] = nk_rgb(77, 91, 110);
    colors[NK_COLOR_BUTTON] = nk_rgb(45, 58, 75);
    colors[NK_COLOR_BUTTON_HOVER] = nk_rgb(58, 95, 129);
    colors[NK_COLOR_BUTTON_ACTIVE] = nk_rgb(35, 126, 176);
    colors[NK_COLOR_SELECT_ACTIVE] = nk_rgb(31, 119, 166);
    colors[NK_COLOR_SLIDER_CURSOR] = nk_rgb(31, 149, 197);
    colors[NK_COLOR_SLIDER_CURSOR_HOVER] = nk_rgb(65, 180, 218);
    colors[NK_COLOR_SLIDER_CURSOR_ACTIVE] = nk_rgb(65, 180, 218);
    nk_style_from_table(&ctx, colors);
    nk_style_set_font(&ctx, &font->handle);
    ctx.style.window.spacing = nk_vec2(6, 6);
    ctx.style.window.padding = nk_vec2(12, 12);
    setup_rows();
    doc_ready = ini_doc_load(&document, "vcthunder.ini");
    ready = 1;
}

static void picker_result(void *unused, const char * const *list, int filter)
{
    (void)unused; (void)filter;
    pthread_mutex_lock(&picker_lock);
    if (!list) snprintf(picker_error, sizeof picker_error, "%s", SDL_GetError());
    else if (!list[0]) picker_cancelled = 1;
    else for (unsigned i = 0; list[i] && picked_count < 64; i++)
        picked_paths[picked_count++] = strdup(list[i]);
    picker_pending = 0;
    pthread_mutex_unlock(&picker_lock);
}

static int busy(void)
{
    pthread_mutex_lock(&picker_lock);
    int result = picker_pending || picked_count || import_pid;
    pthread_mutex_unlock(&picker_lock);
    return result;
}

static void pick(int folder)
{
    if (busy()) return;
    static const SDL_DialogFileFilter filters[] = {
        {"Game disks and archives", "chd;zip;7z;rar"}, {"All files", "*"}
    };
    pthread_mutex_lock(&picker_lock);
    picker_pending = 1;
    pthread_mutex_unlock(&picker_lock);
    snprintf(status, sizeof status, "Choose %s in the file picker...", folder ? "a complete game folder" : "your CHD or game archive");
    SDL_Window *window = font_renderer ? SDL_GetRenderWindow(font_renderer) : NULL;
    if (folder) SDL_ShowOpenFolderDialog(picker_result, NULL, window, NULL, false);
    else SDL_ShowOpenFileDialog(picker_result, NULL, window, filters, 2, NULL, true);
}

static void start_import(const char *path)
{
    int pipefd[2];
    if (pipe(pipefd)) { snprintf(status, sizeof status, "Cannot start importer: %s", strerror(errno)); return; }
    const char *root = data_root();
    import_pid = fork();
    if (import_pid == 0) {
        setpgid(0, 0);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execlp("python3", "python3", "-u", "-c", vct_import_linux, "--root", root, path, (char *)NULL);
        static const char error[] = "ERROR Import needs Python 3. Install python3 and try again.\n";
        write(STDOUT_FILENO, error, sizeof error - 1);
        _exit(127);
    }
    close(pipefd[1]);
    if (import_pid < 0) { close(pipefd[0]); import_pid = 0; snprintf(status, sizeof status, "Cannot fork importer: %s", strerror(errno)); return; }
    setpgid(import_pid, import_pid);
    import_fd = pipefd[0];
    fcntl(import_fd, F_SETFL, O_NONBLOCK);
    import_progress = 0; import_used = 0;
    snprintf(status, sizeof status, "Opening %s...", path);
}

static void consume_line(void)
{
    import_line[import_used] = 0;
    if (!strncmp(import_line, "PCT ", 4)) {
        char *end;
        import_progress = (int)strtol(import_line + 4, &end, 10);
        snprintf(status, sizeof status, "%s", *end == ' ' ? end + 1 : end);
    } else if (!strncmp(import_line, "ERROR ", 6))
        snprintf(status, sizeof status, "%s", import_line + 6);
    else if (strncmp(import_line, "DONE ", 5) && *import_line)
        snprintf(status, sizeof status, "%s", import_line);
    import_used = 0;
}

static void poll_import(void)
{
    if (import_fd >= 0) {
        char data[4096];
        ssize_t n;
        while ((n = read(import_fd, data, sizeof data)) > 0)
            for (ssize_t i = 0; i < n; i++) {
                if (data[i] == '\n' || data[i] == '\r') consume_line();
                else if (import_used < sizeof import_line - 1) import_line[import_used++] = data[i];
            }
    }
    if (import_pid) {
        int code;
        if (waitpid(import_pid, &code, WNOHANG) == import_pid) {
            if (import_used) consume_line();
            close(import_fd); import_fd = -1; import_pid = 0;
            if (!WIFEXITED(code) || WEXITSTATUS(code)) {
                if (!*status) snprintf(status, sizeof status, "Import failed. Select a supported CHD or complete game archive.");
            }
        }
    }
    pthread_mutex_lock(&picker_lock);
    if (picker_error[0]) { snprintf(status, sizeof status, "File picker: %s", picker_error); picker_error[0] = 0; }
    if (picker_cancelled) { snprintf(status, sizeof status, "Import cancelled."); picker_cancelled = 0; }
    char *next = NULL;
    if (!import_pid && picked_count) {
        next = picked_paths[0];
        memmove(picked_paths, picked_paths + 1, --picked_count * sizeof *picked_paths);
    }
    pthread_mutex_unlock(&picker_lock);
    if (next) { start_import(next); free(next); }
}

static void setting_value(const setting_desc_t *s, char *out, size_t cap)
{
    char section[96];
    snprintf(section, sizeof section, "%s.%s", g_cfg.title, s->section);
    if (!launcher && ini_doc_get(&document, section, s->key, out, cap)) return;
    if (!ini_doc_get(&document, s->section, s->key, out, cap)) snprintf(out, cap, "%s", s->fallback);
}

static void change_setting(const setting_desc_t *s, int direction)
{
    if (!strcmp(s->key, "fullscreen") && getenv("VCT_DUAL_CABINET")) {
        snprintf(status, sizeof status, "Linked mode places each cabinet on its own screen or display half.");
        return;
    }
    char value[INI_VALUE_CAP], next[INI_VALUE_CAP], why[512] = "Cannot update INI document", section[96];
    if (!doc_ready) { snprintf(status, sizeof status, "Cannot read vcthunder.ini"); return; }
    setting_value(s, value, sizeof value);
    if (s->type == ST_BOOL) snprintf(next, sizeof next, "%s", !strcasecmp(value, "true") ? "false" : "true");
    else if (s->choices) {
        char choices[INI_VALUE_CAP];
        snprintf(choices, sizeof choices, "%s", s->choices);
        char *options[64], *save, *p; int n = 0, current = 0;
        for (p = strtok_r(choices, "|", &save); p && n < 64; p = strtok_r(NULL, "|", &save)) {
            if (!strcasecmp(value, p)) current = n;
            options[n++] = p;
        }
        if (!n) return;
        snprintf(next, sizeof next, "%s", options[cycle(current, direction < 0 ? -1 : 1, n)]);
    } else {
        long v = strtol(value, NULL, 10), step = (s->maximum >= 100) ? 5 : 1;
        if (!strcmp(s->section, "timing") && !strcmp(s->key, "speed_percent")) step = 25;
        long minimum = s->type == ST_AUTO_UINT ? 0 : (long)s->minimum;
        v += direction < 0 ? -step : step;
        if (v < minimum) v = minimum;
        if (v > (long)s->maximum) v = (long)s->maximum;
        if (s->type == ST_AUTO_UINT && v == 0) snprintf(next, sizeof next, "auto");
        else snprintf(next, sizeof next, "%ld", v);
    }
    snprintf(section, sizeof section, launcher ? "%s" : "%s.%s", launcher ? s->section : g_cfg.title, s->section);
    if (!ini_doc_set(&document, section, s->key, next) || !ini_doc_save(&document, why, sizeof why)) {
        snprintf(status, sizeof status, "Cannot save setting: %s", why); return;
    }
    if (!launcher && !strcmp(s->section, "audio")) {
        if (!strcmp(s->key, "audio")) g_cfg.audio = !strcasecmp(next, "true");
        if (!strcmp(s->key, "volume")) g_cfg.audio_volume = strtoul(next, NULL, 10);
        if (!strcmp(s->key, "output")) snprintf(g_cfg.audio_routing, sizeof g_cfg.audio_routing, "%s", next);
        if (!strcmp(s->key, "balance")) g_cfg.audio_balance = strtoul(next, NULL, 10);
        audio_update_output(g_cfg.audio ? g_cfg.audio_volume : 0, g_cfg.audio_routing, g_cfg.audio_balance);
        snprintf(status, sizeof status, "Sound setting applied and saved.");
    } else if (!launcher && !strcmp(s->key, "fullscreen")) {
        g_cfg.fullscreen = !strcasecmp(next, "true");
        window_set_fullscreen(g_cfg.fullscreen);
        snprintf(status, sizeof status, "Fullscreen setting applied and saved.");
    } else snprintf(status, sizeof status, "Saved. %s", launcher ? "Applies when you launch a game." : "Use File > Restart game to apply.");
}

static int nrows(void) { return tab == FILE_TAB ? 8 : tab == SETUP_TAB ? (launcher ? 4 : 2) : counts[tab] + (tab == PERFORMANCE_TAB); }
static int enabled(int r)
{
    if (launcher && tab == SETUP_TAB && r >= 2) return installed(r - 2) && !busy();
    if (tab == SETUP_TAB || (tab == FILE_TAB && (r == 4 || r == 5))) return !busy();
    if (tab != FILE_TAB) return 1;
    if (r == 6) return installed(1) && !busy() && !getenv("VCT_DUAL_CABINET");
    if (r == 1 && getenv("VCT_DUAL_CABINET")) return 0;
    if (r == 0 || r == 3) return !launcher;
    if (r == 1 || r == 2) return installed(r - 1) && !busy();
    return !busy();
}

static void action(int r, int d)
{
    if (!enabled(r)) return;
    if (tab == SETUP_TAB) {
        if (launcher && r >= 2) { selected = request = r - 1; }
        else pick(r == 1);
        return;
    }
    if (tab == PERFORMANCE_TAB && r == 0) { show_fps = !show_fps; return; }
    if (tab != FILE_TAB) { change_setting(rows[tab][r - (tab == PERFORMANCE_TAB)], d); return; }
    if (r == 0) set_open(0);
    if (r == 1 || r == 2) { selected = r; request = r; }
    if (r == 3) request = !strcmp(g_cfg.title, "offroad") ? 2 : 1;
    if (r == 4 || r == 5) { tab = SETUP_TAB; row = r - 4; pick(r == 5); }
    if (r == 6) request = 3;
    if (r == 7) request = -1;
}

static void nav(int key)
{
    switch (key) {
    case UP: row = row <= -1 ? nrows() - 1 : row - 1; break;
    case DOWN: row = row >= nrows() - 1 ? -1 : row + 1; break;
    case LEFT: case RIGHT:
        if (row < 0) { if (!launcher) tab = cycle(tab, key == LEFT ? -1 : 1, NTABS); row = 0; }
        else if (tab != FILE_TAB && tab != SETUP_TAB) action(row, key == LEFT ? -1 : 1);
        break;
    case OK: if (row < 0) row = 0; else action(row, 0); break;
    case BACK: if (!busy()) { if (launcher) request = -1; else set_open(0); } break;
    case PREV: case NEXT: if (!launcher) { tab = cycle(tab, key == PREV ? -1 : 1, NTABS); row = 0; } break;
    }
    nav_moved = 1;
}

void native_menu_event(const void *ptr)
{
    const SDL_Event *e = ptr;
    /* Only scalar events are retained: SDL text/drop event pointers expire. */
    if ((e->type >= SDL_EVENT_KEY_DOWN && e->type <= SDL_EVENT_KEY_UP) ||
        (e->type >= SDL_EVENT_MOUSE_MOTION && e->type <= SDL_EVENT_MOUSE_WHEEL) ||
        e->type == SDL_EVENT_QUIT || e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        if (nevents < 256) events[nevents++] = *e;
}

static void inputs(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) native_menu_event(&event);
    nk_input_begin(&ctx);
    for (unsigned i = 0; i < nevents; i++) {
        SDL_Event *e = &events[i];
        if (e->type == SDL_EVENT_QUIT || e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) { request = -1; continue; }
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
            if (e->key.key == SDLK_F1) { if (!launcher) set_open(!is_open()); continue; }
            if (e->key.key == SDLK_F3) { if (!launcher) show_fps = !show_fps; continue; }
            if (is_open()) switch (e->key.key) {
            case SDLK_UP: nav(UP); break; case SDLK_DOWN: nav(DOWN); break;
            case SDLK_LEFT: nav(LEFT); break; case SDLK_RIGHT: nav(RIGHT); break;
            case SDLK_RETURN: case SDLK_KP_ENTER: nav(OK); break;
            case SDLK_ESCAPE: case SDLK_BACKSPACE: nav(BACK); break;
            case SDLK_TAB: nav(e->key.mod & SDL_KMOD_SHIFT ? PREV : NEXT); break;
            case SDLK_PAGEUP: nav(PREV); break; case SDLK_PAGEDOWN: nav(NEXT); break;
            default: break;
            }
        }
        if (!is_open()) continue;
        if (e->type == SDL_EVENT_MOUSE_MOTION) nk_input_motion(&ctx, (int)e->motion.x, (int)e->motion.y);
        if ((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) && e->button.button == SDL_BUTTON_LEFT)
            nk_input_button(&ctx, NK_BUTTON_LEFT, (int)e->button.x, (int)e->button.y, e->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        if (e->type == SDL_EVENT_MOUSE_WHEEL) nk_input_scroll(&ctx, nk_vec2(e->wheel.x, e->wheel.y));
    }
    nevents = 0;
    nk_input_end(&ctx);
    const bool *keys = SDL_GetKeyboardState(NULL);
    if (!keys[SDL_SCANCODE_ESCAPE] && !keys[SDL_SCANCODE_F1])
        __atomic_store_n(&swallow_escape, 0, __ATOMIC_RELEASE);
    static unsigned last;
    XINPUT_STATE pad;
    unsigned pad_index = launcher ? 0 : io_config()->xinput_device;
    unsigned buttons = XInputGetState(pad_index, &pad) == ERROR_SUCCESS ? pad.Gamepad.wButtons : 0;
    unsigned pressed = buttons & ~last;
    const unsigned chord = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
    if (!launcher && SDL_GetKeyboardFocus() && (buttons & chord) == chord && (last & chord) != chord) set_open(!is_open());
    else if (is_open() && SDL_GetKeyboardFocus()) {
        if (pressed & XINPUT_GAMEPAD_DPAD_UP) nav(UP);
        if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) nav(DOWN);
        if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) nav(LEFT);
        if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) nav(RIGHT);
        if (pressed & XINPUT_GAMEPAD_A) nav(OK);
        if (pressed & XINPUT_GAMEPAD_B) nav(BACK);
        if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) nav(PREV);
        if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) nav(NEXT);
    }
    last = buttons;
}

static void build_ui(int width, int height)
{
    float tw = fminf(120, (width - 24) / (float)NTABS);
    if (nk_begin(&ctx, "menubar", nk_rect(0, 0, width, 44), NK_WINDOW_NO_SCROLLBAR)) {
        if (launcher) {
            nk_layout_row_dynamic(&ctx, 28, 1);
            nk_label(&ctx, "VCThunder - Game setup", NK_TEXT_LEFT);
        } else {
            nk_layout_row_static(&ctx, 28, (int)tw, NTABS);
            for (int i = 0; i < NTABS; i++)
                if (nk_select_label(&ctx, tabs[i], NK_TEXT_CENTERED, i == tab) && tab != i) { tab = i; row = 0; }
        }
    }
    nk_end(&ctx);
    float dw = fminf(720, width - 16), dx = launcher ? (width - dw) / 2 : fminf(tab * (tw + 6), width - dw - 8);
    if (dx < 8) dx = 8;
    struct nk_rect bounds = nk_rect(dx, 48, dw, height - 58);
    if (nk_begin(&ctx, "page", bounds, NK_WINDOW_BORDER)) {
        nk_window_set_bounds(&ctx, "page", bounds);
        nk_layout_row_dynamic(&ctx, 28, 1);
        nk_label(&ctx, launcher ? "Import your game files, then choose Play" : "VCThunder | Paused", NK_TEXT_LEFT);
        if (tab == FILE_TAB || tab == SETUP_TAB) {
            static const char *names[] = {"Hydro Thunder", "Offroad Thunder"};
            for (int game = 0; game < 2; game++) {
                int present = installed(game);
                nk_layout_row_dynamic(&ctx, 26, 2);
                nk_label(&ctx, names[game], NK_TEXT_LEFT);
                nk_label_colored(&ctx, present ? "Ready to play" : "Needs setup - import game files",
                                 NK_TEXT_LEFT, present ? nk_rgb(115, 218, 151) : nk_rgb(241, 189, 102));
            }
            nk_layout_row_dynamic(&ctx, 8, 1); nk_spacing(&ctx, 1);
        }
        static const char *actions[] = {"Resume", "Play Hydro Thunder", "Play Offroad Thunder", "Restart game", "Import CHD / ROM archive...", "Import extracted folder...", "Linked Offroad (two screens)", "Quit"};
        static const char *setup[] = {"Choose CHD / ROM archive...", "Choose extracted game folder...", "Play Hydro Thunder", "Play Offroad Thunder"};
        for (int i = 0; i < nrows(); i++) {
            if (nav_moved && row == i) {
                struct nk_rect b = nk_widget_bounds(&ctx), c = nk_window_get_content_region(&ctx);
                nk_uint x, y; nk_window_get_scroll(&ctx, &x, &y);
                if (b.y < c.y) nk_window_set_scroll(&ctx, x, (nk_uint)fmaxf(0, y - (c.y - b.y)));
                else if (b.y + 38 > c.y + c.h) nk_window_set_scroll(&ctx, x, y + (nk_uint)(b.y + 38 - c.y - c.h));
            }
            int en = enabled(i);
            if (!en) nk_widget_disable_begin(&ctx);
            if (tab == FILE_TAB || tab == SETUP_TAB) {
                char label[256];
                const char *name = tab == FILE_TAB ? actions[i] : setup[i];
                const char *suffix = tab == FILE_TAB && (i == 1 || i == 2)
                    ? (installed(i - 1) ? "  [Ready]" : "  [Needs setup]") :
                    launcher && i >= 2 ? (installed(i - 2) ? "  [Ready]" : "  [Needs setup]") : "";
                snprintf(label, sizeof label, "%s%s%s", row == i ? ">  " : "    ", name, suffix);
                nk_layout_row_dynamic(&ctx, 36, 1);
                if (nk_button_label(&ctx, label)) { row = i; action(i, 0); }
            } else if (tab == PERFORMANCE_TAB && i == 0) {
                nk_layout_row_dynamic(&ctx, 32, 2);
                if (nk_select_label(&ctx, "Show FPS (F3)", NK_TEXT_LEFT, i == row)) row = i;
                if (nk_button_label(&ctx, show_fps ? "On" : "Off")) { row = i; action(i, 0); }
            } else {
                const setting_desc_t *s = rows[tab][i - (tab == PERFORMANCE_TAB)];
                char value[INI_VALUE_CAP]; setting_value(s, value, sizeof value);
                nk_layout_row_template_begin(&ctx, 32);
                nk_layout_row_template_push_variable(&ctx, 190);
                nk_layout_row_template_push_static(&ctx, 28);
                nk_layout_row_template_push_dynamic(&ctx);
                nk_layout_row_template_push_static(&ctx, 28);
                nk_layout_row_template_end(&ctx);
                if (nk_select_label(&ctx, s->label, NK_TEXT_LEFT, i == row)) row = i;
                if (nk_button_symbol(&ctx, NK_SYMBOL_TRIANGLE_LEFT)) { row = i; action(i, -1); }
                if (nk_button_label(&ctx, value)) { row = i; action(i, 0); }
                if (nk_button_symbol(&ctx, NK_SYMBOL_TRIANGLE_RIGHT)) { row = i; action(i, 1); }
            }
            if (!en) nk_widget_disable_end(&ctx);
        }
        nav_moved = 0;
        nk_layout_row_dynamic(&ctx, 12, 1); nk_spacing(&ctx, 1);
        if (tab == SETUP_TAB) {
            nk_layout_row_dynamic(&ctx, 62, 1);
            nk_label_wrap(&ctx, "First-time setup: select your Hydro Thunder or Offroad Thunder CHD, a complete game ZIP/7z/RAR, or extracted folder. BIOS ROMs alone do not contain the game.");
            nk_layout_row_dynamic(&ctx, 42, 1);
            nk_label_wrap(&ctx, "Supported: Hydro v01.01b; Offroad 2000-02-25. Imports keep saves and existing installations.");
            char root[1024]; snprintf(root, sizeof root, "Install folder: %s", data_root());
            nk_layout_row_dynamic(&ctx, 46, 1); nk_label_wrap(&ctx, root);
        }
        if (tab == CONTROLS_TAB) {
            XINPUT_STATE state;
            char text[256];
            if (XInputGetState(io_config()->xinput_device, &state) == ERROR_SUCCESS)
                snprintf(text, sizeof text, "Xbox pad connected | steer %d | RT %u | LT %u", state.Gamepad.sThumbLX, state.Gamepad.bRightTrigger, state.Gamepad.bLeftTrigger);
            else snprintf(text, sizeof text, "No Xbox pad connected. Connect by USB or Bluetooth.");
            nk_layout_row_dynamic(&ctx, 46, 1); nk_label_wrap(&ctx, text);
            nk_layout_row_dynamic(&ctx, 66, 1);
            nk_label_wrap(&ctx, "Left stick: steer. RT: gas. LT: brake. A: boost / accept. View: coin. Menu: start. Offroad: LB/RB shift. View + Menu: open this menu.");
        }
        if (tab == PERFORMANCE_TAB) {
            char measured[128];
            snprintf(measured, sizeof measured, "%s | %.1f FPS | current game clock: %u%%", glide_renderer_name(), game_fps, g_cfg.speed_percent);
            nk_layout_row_dynamic(&ctx, 28, 1); nk_label(&ctx, measured, NK_TEXT_LEFT);
            nk_layout_row_dynamic(&ctx, 86, 1);
            nk_label_wrap(&ctx, "100% is original game speed. Try 200% for roughly 60 FPS if the renderer can keep up; gameplay also runs faster. Smooth 60 FPS at original speed is not available in this Linux build yet. Restart the game to apply clock changes.");
            nk_layout_row_dynamic(&ctx, 48, 1);
            nk_label_wrap(&ctx, "DLSS: unavailable in this 32-bit host. Frame generation: not integrated. Resolution and texture/HUD filtering are on the Display page.");
        } else if (tab != FILE_TAB && tab != SETUP_TAB) {
            nk_layout_row_dynamic(&ctx, 42, 1);
            nk_label_wrap(&ctx, tab == SOUND_TAB ? "Volume, routing and balance apply immediately. Enabling sound after launching with audio disabled requires a restart." : "Settings are saved to vcthunder.ini. Restart the game to apply; fullscreen applies immediately.");
        }
        if (busy()) {
            nk_size progress = (nk_size)import_progress;
            nk_layout_row_dynamic(&ctx, 18, 1); nk_progress(&ctx, &progress, 100, NK_FIXED);
        }
        nk_layout_row_dynamic(&ctx, 82, 1); nk_label_wrap(&ctx, status);
        nk_layout_row_dynamic(&ctx, 42, 1);
        nk_label_wrap(&ctx, launcher ? "Choose files to import, then Play. Arrows / Enter, D-pad / A, or mouse." : "F1 / View + Menu: menu. F3: FPS. Arrows / Enter or D-pad / A. Tab / LB / RB: page. B: resume.");
    }
    nk_end(&ctx);
}

static void fps_overlay(int width)
{
    char label[96];
    if (game_fps > 0) snprintf(label, sizeof label, "%s | %.1f FPS | %u%%%s", glide_renderer_name(), game_fps, g_cfg.speed_percent, is_open() ? " | paused" : "");
    else snprintf(label, sizeof label, "%s | FPS: measuring...", glide_renderer_name());
    struct nk_rect box = nk_rect(fmaxf(4, width - 366), is_open() ? 48 : 8, 358, 38);
    if (nk_begin(&ctx, "fps", box, NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT)) {
        nk_window_set_bounds(&ctx, "fps", box);
        nk_layout_row_dynamic(&ctx, 20, 1);
        nk_label_colored(&ctx, label, NK_TEXT_LEFT, nk_rgb(142, 236, 167));
    }
    nk_end(&ctx);
}

struct vertex { float pos[2], uv[2]; nk_byte color[4]; };
static void render_ui(SDL_Renderer *renderer)
{
    if (font_renderer != renderer) {
        if (font_texture) SDL_DestroyTexture(font_texture);
        font_renderer = renderer;
        font_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, font_w, font_h);
        if (!font_texture) return;
        SDL_UpdateTexture(font_texture, NULL, font_pixels, font_w * 4);
        SDL_SetTextureBlendMode(font_texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(font_texture, SDL_SCALEMODE_LINEAR);
    }
    static const struct nk_draw_vertex_layout_element layout[] = {
        {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, offsetof(struct vertex, pos)},
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, offsetof(struct vertex, uv)},
        {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, offsetof(struct vertex, color)},
        {NK_VERTEX_LAYOUT_END}
    };
    struct nk_convert_config cfg = {0};
    cfg.vertex_layout = layout; cfg.vertex_size = sizeof(struct vertex); cfg.vertex_alignment = NK_ALIGNOF(struct vertex);
    cfg.tex_null = null_tex; cfg.circle_segment_count = cfg.curve_segment_count = cfg.arc_segment_count = 22;
    cfg.global_alpha = 1; cfg.shape_AA = cfg.line_AA = NK_ANTI_ALIASING_ON;
    nk_buffer_clear(&commands); nk_buffer_clear(&vertices); nk_buffer_clear(&elements);
    if (nk_convert(&ctx, &commands, &vertices, &elements, &cfg) != NK_CONVERT_SUCCESS) { nk_clear(&ctx); return; }
    const struct vertex *v = nk_buffer_memory_const(&vertices);
    const nk_draw_index *indices = nk_buffer_memory_const(&elements);
    int count = (int)(vertices.allocated / sizeof *v);
    if (!count) { nk_clear(&ctx); return; }
    SDL_Vertex *out = malloc((size_t)count * sizeof *out);
    if (!out) { nk_clear(&ctx); return; }
    for (int i = 0; i < count; i++) {
        out[i].position = (SDL_FPoint){v[i].pos[0], v[i].pos[1]};
        out[i].tex_coord = (SDL_FPoint){v[i].uv[0], v[i].uv[1]};
        out[i].color = (SDL_FColor){v[i].color[0]/255.f, v[i].color[1]/255.f, v[i].color[2]/255.f, v[i].color[3]/255.f};
    }
    const struct nk_draw_command *cmd;
    nk_draw_foreach(cmd, &ctx, &commands) {
        if (!cmd->elem_count) continue;
        SDL_Rect clip = {(int)cmd->clip_rect.x, (int)cmd->clip_rect.y, (int)cmd->clip_rect.w, (int)cmd->clip_rect.h};
        SDL_SetRenderClipRect(renderer, &clip);
        SDL_RenderGeometryRaw(renderer, font_texture, &out[0].position.x, sizeof *out,
                              &out[0].color, sizeof *out, &out[0].tex_coord.x, sizeof *out,
                              count, indices, (int)cmd->elem_count, sizeof *indices);
        indices += cmd->elem_count;
    }
    free(out);
    SDL_SetRenderClipRect(renderer, NULL);
    nk_clear(&ctx);
}

static void perform_request(void)
{
    if (!request || launcher) return;
    if (request < 0 || g_linux_quit) { nvram_flush(); audio_shutdown(); diego_shutdown(); exit(0); }
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) { snprintf(status, sizeof status, "Cannot locate executable to restart."); request = 0; return; }
    exe[n] = 0;
    /* Rebuilding while a game is open unlinks its old executable inode. */
    static const char deleted[] = " (deleted)";
    if ((size_t)n >= sizeof deleted - 1 && !strcmp(exe + n - (sizeof deleted - 1), deleted))
        exe[n - (sizeof deleted - 1)] = 0;
    extern int g_linux_argc;
    extern char **g_linux_argv;
    char *args[68];
    const char *title = request == 1 ? "hydro" : "offroad";
    int count = 0;
    args[count++] = exe; args[count++] = (char *)title;
    for (int i = 1; i < g_linux_argc && count < 64; i++) {
        const char *arg = g_linux_argv[i];
        if (!strcmp(arg, "hydro") || !strcmp(arg, "offroad") || !strcmp(arg, "--menu")) continue;
        if (!strcmp(arg, "--title") || (strcmp(title, g_cfg.title) &&
            (!strcmp(arg, "--exe") || !strcmp(arg, "--data")))) { i++; continue; }
        args[count++] = g_linux_argv[i];
        /* Keep option values verbatim, including a directory called hydro. */
        if (!strncmp(arg, "--", 2) && strcmp(arg, "--dry-run") &&
            strcmp(arg, "--probe-image") && strcmp(arg, "--probe-image-refusal") &&
            strcmp(arg, "--trace") && i + 1 < g_linux_argc && count < 64)
            args[count++] = g_linux_argv[++i];
    }
    if (request == 3) args[count++] = "--dual-screen";
    args[count] = NULL;
    nvram_flush(); audio_shutdown(); diego_shutdown();
    execv(exe, args);
    fprintf(stderr, "Cannot restart: %s\n", strerror(errno)); exit(1);
}

int native_menu_draw(void *renderer, int width, int height)
{
    if (!width || !height) {
        if (font_texture) SDL_DestroyTexture(font_texture);
        font_texture = NULL; font_renderer = NULL; return 0;
    }
    init_ui();
    if (!launcher) {
        Uint64 now = SDL_GetTicks();
        if (is_open() || !fps_epoch) { fps_epoch = now; fps_frames = 0; }
        else {
            fps_frames++;
            if (now - fps_epoch >= 1000) {
                game_fps = fps_frames * 1000.0 / (double)(now - fps_epoch);
                fps_epoch = now; fps_frames = 0;
            }
        }
    }
    inputs(); poll_import();
    if (g_linux_quit) request = -1;
    perform_request();
    if (is_open()) build_ui(width, height);
    if (!launcher && show_fps) fps_overlay(width);
    if (!is_open() && (launcher || !show_fps)) return 0;
    render_ui(renderer);
    return is_open();
}

int native_menu_launcher(int preferred, int force_setup)
{
    launcher = 1; request = selected = 0;
    config_seed_ini("vcthunder.ini");
    init_ui();
    if (!force_setup) {
        int game = preferred >= 0 ? preferred : installed(0) ? 0 : 1;
        if (installed(game)) { launcher = 0; return game + 1; }
    }
    tab = SETUP_TAB;
    snprintf(status, sizeof status, "Welcome. Import your game files to get started. Settings are available in the game with F1.");
    set_open(1);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { fprintf(stderr, "Menu: %s\n", SDL_GetError()); return 0; }
    SDL_Window *window = SDL_CreateWindow("VCThunder - Game setup", 1000, 760, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, "software") : NULL;
    if (!renderer) { fprintf(stderr, "Menu: %s\n", SDL_GetError()); if (window) SDL_DestroyWindow(window); SDL_QuitSubSystem(SDL_INIT_VIDEO); return 0; }
    SDL_SetWindowMinimumSize(window, 640, 480);
    while (!request && !g_linux_quit) {
        int width, height; SDL_GetWindowSize(window, &width, &height);
        SDL_SetRenderDrawColor(renderer, 10, 16, 24, 255); SDL_RenderClear(renderer);
        native_menu_draw(renderer, width, height);
        SDL_RenderPresent(renderer); SDL_Delay(16);
    }
    native_menu_draw(renderer, 0, 0);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_QuitSubSystem(SDL_INIT_VIDEO);
    int result = request > 0 ? selected : 0;
    native_menu_cleanup();
    launcher = 0; request = 0; tab = row = 0;
    __atomic_store_n(&open_menu, 0, __ATOMIC_RELEASE);
    return result;
}

void native_menu_cleanup(void)
{
    if (import_pid) { kill(-import_pid, SIGTERM); waitpid(import_pid, NULL, 0); import_pid = 0; }
    if (import_fd >= 0) { close(import_fd); import_fd = -1; }
    /* The asynchronous picker may still be closing. Its static state remains
     * valid until process exit. The UI itself can be re-used by the game. */
}
