/* menu/main.c -- native Linux settings menu: Nuklear on SDL3's GPU (Vulkan).
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A native, immediate-mode settings menu rendered through SDL3's GPU API on the
 * Vulkan device (SDL_CreateGPUDevice("vulkan")), with Nuklear driving the UI.
 * This replaces the Windows GDI launcher for the Linux port rather than
 * porting ~100 GDI symbols. The settings themselves come from src/settings.def;
 * this first cut draws a few representative widgets to prove the pipeline, and
 * the full catalogue is wired on top of the same render loop.
 */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#include "nuklear.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "shader_vert_spv.h"
#include "shader_frag_spv.h"

/* ---- the settings, from the single table -------------------------------- */
#include "vcthunder.h"
#include "settings.h"

/* The shipped settings, from src/settings.def (the one table). Same expansion
 * as src/settings.c's vct_ui_setting[]: SETTING rows only, no dev switches. */
static const setting_desc_t menu_settings[VCT_UI_SETTING_COUNT] = {
#define SETTING(section, key, store, show) { section, key, store, show },
#define SETTING_DEV(section, key, store)
#include "settings.def"
#undef SETTING
#undef SETTING_DEV
};

static const char *const PAGE_NAMES[PAGE_COUNT] = {
    "Home", "Graphics", "Audio", "Controls", "Force feedback",
    "System", "Link", "Files", "Troubleshooting"
};

static char menu_values[VCT_UI_SETTING_COUNT][256];
static int menu_page = PAGE_HOME;
static int menu_advanced;
static const char *menu_ini = "vcthunder.ini";

static void menu_settings_load(void)
{
    int i;

    for (i = 0; i < VCT_UI_SETTING_COUNT; i++) {
        const setting_desc_t *d = &menu_settings[i];
        posix_profile_get_string(d->section, d->key, d->fallback,
                                 menu_values[i], sizeof menu_values[i], menu_ini);
    }
}

static void menu_settings_save(int i)
{
    const setting_desc_t *d = &menu_settings[i];
    posix_profile_put_string(d->section, d->key, menu_values[i], menu_ini);
}

/* Split a '|'-separated choice list into null-terminated strings. */
static int menu_split_choices(const char *choices, char *buf, size_t bufsz,
                              const char *items[], int max)
{
    int n = 0;
    char *dst = buf;
    const char *p = choices ? choices : "";

    while (*p && n < max) {
        const char *end = strchr(p, '|');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if ((size_t)(dst - buf) + len + 1 > bufsz)
            break;
        items[n] = dst;
        memcpy(dst, p, len);
        dst[len] = '\0';
        dst += len + 1;
        n++;
        if (!end)
            break;
        p = end + 1;
    }
    return n;
}

static int menu_choice_index(const setting_desc_t *d, const char *value)
{
    char buf[256];
    const char *items[32];
    int n = menu_split_choices(d->choices, buf, sizeof buf, items, 32);
    int i;

    for (i = 0; i < n; i++)
        if (strcasecmp(items[i], value) == 0)
            return i;
    return 0;
}

static int menu_bool_get(int i)
{
    return strcasecmp(menu_values[i], "true") == 0 ||
           strcmp(menu_values[i], "1") == 0;
}

static void menu_bool_set(int i, int v)
{
    snprintf(menu_values[i], sizeof menu_values[i], "%s", v ? "true" : "false");
    menu_settings_save(i);
}

static int menu_uint_get(int i)
{
    return (int)strtol(menu_values[i], NULL, 0);
}

static void menu_uint_set(int i, int v)
{
    snprintf(menu_values[i], sizeof menu_values[i], "%d", v);
    menu_settings_save(i);
}

/* ---- the game data on disk --------------------------------------------- */

static void format_size(char *out, size_t n, long long bytes)
{
    if (bytes >= 1024LL * 1024 * 1024)
        snprintf(out, n, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024LL * 1024)
        snprintf(out, n, "%.1f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024)
        snprintf(out, n, "%.1f KB", bytes / 1024.0);
    else
        snprintf(out, n, "%lld B", bytes);
}

static int list_dir(const char *path, char (*names)[256], int max)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;

    if (!d)
        return 0;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(names[n], 256, "%s", e->d_name);
        n++;
    }
    closedir(d);
    return n;
}

/* One row: name on the left, a size/kind summary on the right. */
static void render_entry(struct nk_context *ctx, const char *path,
                         const char *name, int is_dir)
{
    char full[512], text[40];
    struct stat st;

    snprintf(full, sizeof full, "%s/%s", path, name);
    if (stat(full, &st) == 0 && S_ISREG(st.st_mode))
        format_size(text, sizeof text, (long long)st.st_size);
    else if (is_dir)
        snprintf(text, sizeof text, "folder");
    else
        snprintf(text, sizeof text, "?");

    nk_layout_row_begin(ctx, NK_DYNAMIC, 22, 2);
    nk_layout_row_push(ctx, 300.0f);
    nk_label(ctx, name, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 120.0f);
    nk_label(ctx, text, NK_TEXT_RIGHT);
    nk_layout_row_end(ctx);
}

/* Count files and total bytes in a directory, for a "hard drive folder" row. */
static void folder_summary(const char *path, char *out, size_t n)
{
    DIR *d = opendir(path);
    struct dirent *e;
    struct stat st;
    char full[512], sz[32];
    int count = 0;
    long long total = 0;

    if (!d) {
        snprintf(out, n, "(not present)");
        return;
    }
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            count++;
            total += st.st_size;
        }
    }
    closedir(d);
    format_size(sz, sizeof sz, total);
    snprintf(out, n, "%d files, %s", count, sz);
}

/* The menu's vertex: position(2f) + uv(2f) + colour(4 x u8). 20 bytes. Nuklear
 * v4.13.3 packs vertices into a byte buffer according to the layout below, so
 * there is no built-in nk_draw_vertex struct to match. */
struct menu_vertex {
    float position[2];
    float uv[2];
    unsigned char col[4];
};
static const struct nk_draw_vertex_layout_element vtx_layout[] = {
    { NK_VERTEX_POSITION, NK_FORMAT_FLOAT, NK_OFFSETOF(struct menu_vertex, position) },
    { NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(struct menu_vertex, uv) },
    { NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, NK_OFFSETOF(struct menu_vertex, col) },
    { NK_VERTEX_LAYOUT_END }
};

#define MAX_VERTEX_MEMORY (512 * 1024)
#define MAX_ELEMENT_MEMORY (128 * 1024)

typedef struct {
    SDL_Window *window;
    SDL_GPUDevice *device;
    SDL_GPUGraphicsPipeline *pipeline;
    SDL_GPUBuffer *vtx, *idx;
    SDL_GPUTransferBuffer *vtx_up, *idx_up;
    SDL_GPUTexture *font_tex;
    SDL_GPUSampler *font_sampler;
    struct nk_context ctx;
    struct nk_font_atlas atlas;
    struct nk_buffer cmds, vbuf, ibuf;
    int width, height;
    int running;
} menu_t;

static void die(const char *what)
{
    fprintf(stderr, "menu: %s: %s\n", what, SDL_GetError());
    exit(1);
}

static SDL_GPUShader *make_shader(SDL_GPUDevice *dev, const unsigned char *code,
                                  size_t size, SDL_GPUShaderStage stage,
                                  Uint32 num_samplers)
{
    SDL_GPUShaderCreateInfo ci;
    SDL_zero(ci);
    ci.code_size = size;
    ci.code = code;
    ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    ci.stage = stage;
    ci.num_samplers = num_samplers;
    return SDL_CreateGPUShader(dev, &ci);
}

static void upload_buffer(SDL_GPUDevice *dev, SDL_GPUTransferBuffer *up,
                          SDL_GPUBuffer *dst, const void *data, size_t size)
{
    void *p = SDL_MapGPUTransferBuffer(dev, up, false);
    if (!p)
        die("SDL_MapGPUTransferBuffer");
    memcpy(p, data, size);
    SDL_UnmapGPUTransferBuffer(dev, up);

    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTransferBufferLocation src = { up, 0 };
    SDL_GPUBufferRegion region = { dst, 0, (Uint32)size };
    SDL_UploadToGPUBuffer(cp, &src, &region, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cb);
}

static void menu_init(menu_t *m)
{
    SDL_GPUGraphicsPipelineCreateInfo pci;
    SDL_GPUVertexBufferDescription vbdesc;
    SDL_GPUVertexAttribute attrs[3];
    SDL_GPUColorTargetBlendState blend;
    SDL_GPUColorTargetDescription ctd;
    SDL_GPUGraphicsPipelineTargetInfo target_info;
    SDL_GPURasterizerState raster;
    SDL_GPUMultisampleState ms;
    SDL_GPUDepthStencilState ds;
    SDL_GPUVertexInputState vis;
    SDL_GPUShader *vs, *fs;

    if (!SDL_Init(SDL_INIT_VIDEO))
        die("SDL_Init(VIDEO)");

    m->window = SDL_CreateWindow("VCThunder", 1280, 720,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!m->window)
        die("SDL_CreateWindow");

    m->device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, 0, "vulkan");
    if (!m->device)
        die("SDL_CreateGPUDevice(vulkan)");

    if (!SDL_ClaimWindowForGPUDevice(m->device, m->window))
        die("SDL_ClaimWindowForGPUDevice");
    if (!SDL_SetGPUSwapchainParameters(m->device, m->window,
                                       SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                       SDL_GPU_PRESENTMODE_VSYNC))
        die("SDL_SetGPUSwapchainParameters");

    /* ---- shaders + pipeline --------------------------------------------- */
    vs = make_shader(m->device, shader_vert_spv, sizeof shader_vert_spv,
                     SDL_GPU_SHADERSTAGE_VERTEX, 0);
    fs = make_shader(m->device, shader_frag_spv, sizeof shader_frag_spv,
                     SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    if (!vs || !fs)
        die("SDL_CreateGPUShader");

    SDL_zero(vbdesc);
    vbdesc.slot = 0;
    vbdesc.pitch = sizeof(struct menu_vertex);
    vbdesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    attrs[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
                                         NK_OFFSETOF(struct menu_vertex, position) };
    attrs[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
                                         NK_OFFSETOF(struct menu_vertex, uv) };
    attrs[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,
                                         NK_OFFSETOF(struct menu_vertex, col) };
    vis = (SDL_GPUVertexInputState){ &vbdesc, 1, attrs, 3 };

    SDL_zero(blend);
    blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.color_write_mask = 0xF;
    blend.enable_blend = true;
    blend.enable_color_write_mask = false;

    ctd = (SDL_GPUColorTargetDescription){
        SDL_GetGPUSwapchainTextureFormat(m->device, m->window), blend };
    target_info = (SDL_GPUGraphicsPipelineTargetInfo){
        &ctd, 1, SDL_GPU_TEXTUREFORMAT_INVALID, false, 0, 0, 0 };

    SDL_zero(raster);
    raster.fill_mode = SDL_GPU_FILLMODE_FILL;
    raster.cull_mode = SDL_GPU_CULLMODE_NONE;
    raster.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    raster.enable_depth_bias = false;
    raster.enable_depth_clip = true;

    SDL_zero(ms);
    ms.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ms.enable_mask = false;
    ms.enable_alpha_to_coverage = false;

    SDL_zero(ds);
    ds.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    ds.enable_depth_test = false;
    ds.enable_depth_write = false;
    ds.enable_stencil_test = false;

    SDL_zero(pci);
    pci.vertex_shader = vs;
    pci.fragment_shader = fs;
    pci.vertex_input_state = vis;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.rasterizer_state = raster;
    pci.multisample_state = ms;
    pci.depth_stencil_state = ds;
    pci.target_info = target_info;
    m->pipeline = SDL_CreateGPUGraphicsPipeline(m->device, &pci);
    if (!m->pipeline)
        die("SDL_CreateGPUGraphicsPipeline");

    SDL_ReleaseGPUShader(m->device, vs);
    SDL_ReleaseGPUShader(m->device, fs);

    SDL_GPUBufferCreateInfo bci;
    SDL_GPUTransferBufferCreateInfo tbci;
    SDL_GPUTextureCreateInfo tci;
    SDL_GPUSamplerCreateInfo sci;
    SDL_GPUTransferBuffer *tb;
    SDL_GPUCommandBuffer *cb;
    SDL_GPUCopyPass *cp;
    const void *pixels;
    struct nk_font *font;
    int fw = 0, fh = 0;

    /* ---- vertex/index buffers + upload buffers --------------------------- */
    SDL_zero(bci);
    bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bci.size = MAX_VERTEX_MEMORY;
    m->vtx = SDL_CreateGPUBuffer(m->device, &bci);
    bci.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    bci.size = MAX_ELEMENT_MEMORY;
    m->idx = SDL_CreateGPUBuffer(m->device, &bci);

    SDL_zero(tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = MAX_VERTEX_MEMORY;
    m->vtx_up = SDL_CreateGPUTransferBuffer(m->device, &tbci);
    tbci.size = MAX_ELEMENT_MEMORY;
    m->idx_up = SDL_CreateGPUTransferBuffer(m->device, &tbci);

    /* ---- font atlas, baked as R8 alpha ----------------------------------- */
    nk_font_atlas_init_default(&m->atlas);
    nk_font_atlas_begin(&m->atlas);
    font = nk_font_atlas_add_default(&m->atlas, 13, NULL);
    pixels = nk_font_atlas_bake(&m->atlas, &fw, &fh, NK_FONT_ATLAS_ALPHA8);
    nk_font_atlas_end(&m->atlas, nk_handle_id(0), NULL);

    SDL_zero(tci);
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width = (Uint32)fw;
    tci.height = (Uint32)fh;
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    m->font_tex = SDL_CreateGPUTexture(m->device, &tci);

    tbci.size = (Uint32)(fw * fh);
    tb = SDL_CreateGPUTransferBuffer(m->device, &tbci);
    {
        void *dst = SDL_MapGPUTransferBuffer(m->device, tb, false);
        memcpy(dst, pixels, (size_t)fw * fh);
        SDL_UnmapGPUTransferBuffer(m->device, tb);
    }
    cb = SDL_AcquireGPUCommandBuffer(m->device);
    cp = SDL_BeginGPUCopyPass(cb);
    {
        SDL_GPUTextureTransferInfo tti = { tb, 0, (Uint32)fw, (Uint32)fh };
        SDL_GPUTextureRegion treg = { m->font_tex, 0, 0, 0, 0, 0,
                                      (Uint32)fw, (Uint32)fh, 1 };
        SDL_UploadToGPUTexture(cp, &tti, &treg, false);
    }
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cb);
    SDL_ReleaseGPUTransferBuffer(m->device, tb);

    SDL_zero(sci);
    sci.min_filter = SDL_GPU_FILTER_LINEAR;
    sci.mag_filter = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.max_anisotropy = 0.0f;
    sci.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    sci.min_lod = -1000.0f;
    sci.max_lod = 1000.0f;
    sci.enable_anisotropy = false;
    sci.enable_compare = false;
    m->font_sampler = SDL_CreateGPUSampler(m->device, &sci);

    /* ---- nuklear context ------------------------------------------------- */
    nk_init_default(&m->ctx, &font->handle);
    nk_buffer_init_default(&m->cmds);
    nk_buffer_init_default(&m->vbuf);
    nk_buffer_init_default(&m->ibuf);

    m->running = 1;
}

static void handle_events(menu_t *m)
{
    SDL_Event e;

    nk_input_begin(&m->ctx);
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            m->running = 0;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE) {
                m->running = 0;
                break;
            }
            {
                int down = 1;
                switch (e.key.key) {
                case SDLK_BACKSPACE: nk_input_key(&m->ctx, NK_KEY_BACKSPACE, down); break;
                case SDLK_DELETE:    nk_input_key(&m->ctx, NK_KEY_DEL, down); break;
                case SDLK_TAB:       nk_input_key(&m->ctx, NK_KEY_TAB, down); break;
                case SDLK_LEFT:      nk_input_key(&m->ctx, NK_KEY_LEFT, down); break;
                case SDLK_RIGHT:     nk_input_key(&m->ctx, NK_KEY_RIGHT, down); break;
                case SDLK_UP:        nk_input_key(&m->ctx, NK_KEY_UP, down); break;
                case SDLK_DOWN:      nk_input_key(&m->ctx, NK_KEY_DOWN, down); break;
                case SDLK_RETURN:    nk_input_key(&m->ctx, NK_KEY_ENTER, down); break;
                default: break;
                }
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            nk_input_motion(&m->ctx, (int)e.motion.x, (int)e.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_LEFT)
                nk_input_button(&m->ctx, NK_BUTTON_LEFT, (int)e.button.x,
                                (int)e.button.y, nk_true);
            else if (e.button.button == SDL_BUTTON_RIGHT)
                nk_input_button(&m->ctx, NK_BUTTON_RIGHT, (int)e.button.x,
                                (int)e.button.y, nk_true);
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT)
                nk_input_button(&m->ctx, NK_BUTTON_LEFT, (int)e.button.x,
                                (int)e.button.y, nk_false);
            else if (e.button.button == SDL_BUTTON_RIGHT)
                nk_input_button(&m->ctx, NK_BUTTON_RIGHT, (int)e.button.x,
                                (int)e.button.y, nk_false);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            nk_input_scroll(&m->ctx, nk_vec2((float)e.wheel.x, (float)e.wheel.y));
            break;
        case SDL_EVENT_TEXT_INPUT:
            nk_input_unicode(&m->ctx, nk_utf_decode(e.text.text, NULL, 0));
            break;
        case SDL_EVENT_KEY_UP:
            break;
        default:
            break;
        }
    }
    nk_input_end(&m->ctx);
}

static void render_setting(struct nk_context *ctx, int i)
{
    const setting_desc_t *d = &menu_settings[i];
    int v;

    nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
    nk_layout_row_push(ctx, 240.0f);
    nk_label_wrap(ctx, d->label ? d->label : d->key);
    nk_layout_row_push(ctx, 260.0f);

    switch (d->type) {
    case ST_BOOL:
        v = menu_bool_get(i);
        if (nk_checkbox_label(ctx, "", &v))
            menu_bool_set(i, v);
        break;

    case ST_ENUM: {
        char buf[256];
        const char *items[32];
        int n = menu_split_choices(d->choices, buf, sizeof buf, items, 32);
        int sel = menu_choice_index(d, menu_values[i]);
        int prev = sel;

        if (n > 0) {
            nk_combobox(ctx, items, n, &sel, 24, nk_vec2(240, 300));
            if (sel != prev) {
                snprintf(menu_values[i], sizeof menu_values[i], "%s", items[sel]);
                menu_settings_save(i);
            }
        }
        break;
    }

    case ST_UINT:
    case ST_MHZ_UINT: {
        int lo = (int)d->minimum, hi = (int)d->maximum;

        if (hi <= lo)
            hi = lo + 1;
        v = menu_uint_get(i);
        if (v < lo)
            v = lo;
        if (v > hi)
            v = hi;
        if (nk_slider_int(ctx, lo, &v, hi, 1))
            menu_uint_set(i, v);
        break;
    }

    case ST_AUTO_UINT:
    case ST_TEXT:
    default: {
        nk_flags f = nk_edit_string_zero_terminated(
            ctx, NK_EDIT_FIELD, menu_values[i], (int)sizeof menu_values[i],
            nk_filter_default);
        if (f & (NK_EDIT_COMMITTED | NK_EDIT_DEACTIVATED))
            menu_settings_save(i);
        break;
    }
    }
    nk_layout_row_end(ctx);
}

static void build_ui(menu_t *m)
{
    struct nk_context *ctx = &m->ctx;
    int i;

    if (nk_begin(ctx, "VCThunder", nk_rect(16, 16, 300, 150),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, "Page", NK_TEXT_LEFT);
        nk_combobox(ctx, PAGE_NAMES, PAGE_COUNT, &menu_page, 22,
                    nk_vec2(260, 300));
        nk_layout_row_dynamic(ctx, 28, 1);
        nk_checkbox_label(ctx, "Show advanced", &menu_advanced);
    }
    nk_end(ctx);

    if (nk_begin(ctx, PAGE_NAMES[menu_page], nk_rect(332, 16, 560, 660),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        for (i = 0; i < VCT_UI_SETTING_COUNT; i++) {
            const setting_desc_t *d = &menu_settings[i];

            if (d->page != menu_page)
                continue;
            if ((d->flags & SF_ADVANCED) && !menu_advanced)
                continue;
            render_setting(ctx, i);
        }

        /* The Files page also shows the game data on disk: the CHD disk
         * images and the extracted hard-drive folders, not just the path
         * settings above. */
        if (menu_page == PAGE_FILES) {
            char names[256][256];
            char summary[64];
            int n, j;

            nk_layout_row_dynamic(ctx, 10, 1);
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label(ctx, "Disk images (chd/)", NK_TEXT_LEFT);
            n = list_dir("chd", names, 256);
            for (j = 0; j < n; j++) {
                size_t l = strlen(names[j]);

                if (l < 4 || strcmp(names[j] + l - 4, ".chd") != 0)
                    continue;
                render_entry(ctx, "chd", names[j], 0);
            }

            nk_layout_row_dynamic(ctx, 10, 1);
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label(ctx, "Hard drive folders (data/)", NK_TEXT_LEFT);
            n = list_dir("data", names, 256);
            for (j = 0; j < n; j++) {
                char sub[512];

                snprintf(sub, sizeof sub, "data/%s", names[j]);
                folder_summary(sub, summary, sizeof summary);
                nk_layout_row_begin(ctx, NK_DYNAMIC, 22, 2);
                nk_layout_row_push(ctx, 300.0f);
                nk_label(ctx, names[j], NK_TEXT_LEFT);
                nk_layout_row_push(ctx, 160.0f);
                nk_label(ctx, summary, NK_TEXT_RIGHT);
                nk_layout_row_end(ctx);
            }
        }
    }
    nk_end(ctx);
}

static void menu_frame(menu_t *m)
{
    struct nk_convert_config cfg;
    const struct nk_draw_command *cmd;
    SDL_GPUCommandBuffer *cb;
    SDL_GPUTexture *swapchain;
    Uint32 sw = 0, sh = 0;
    size_t vtx_size, idx_size, elem_offset = 0;
    struct menu_vertex *v;
    int i, n;

    handle_events(m);

    nk_clear(&m->ctx);
    build_ui(m);

    memset(&cfg, 0, sizeof cfg);
    cfg.global_alpha = 1.0f;
    cfg.line_AA = NK_ANTI_ALIASING_ON;
    cfg.shape_AA = NK_ANTI_ALIASING_ON;
    cfg.circle_segment_count = 22;
    cfg.arc_segment_count = 22;
    cfg.curve_segment_count = 22;
    cfg.vertex_layout = vtx_layout;
    cfg.vertex_size = sizeof(struct menu_vertex);
    cfg.vertex_alignment = NK_ALIGNOF(struct menu_vertex);

    nk_buffer_clear(&m->cmds);
    nk_buffer_clear(&m->vbuf);
    nk_buffer_clear(&m->ibuf);
    if (nk_convert(&m->ctx, &m->cmds, &m->vbuf, &m->ibuf, &cfg) !=
        NK_CONVERT_SUCCESS) {
        fprintf(stderr, "menu: nk_convert failed\n");
        return;
    }

    vtx_size = m->vbuf.needed;
    idx_size = m->ibuf.needed;
    if (m->width <= 0 || m->height <= 0)
        return;

    /* Nuklear draws in top-left-origin screen space; convert to clip space. */
    n = (int)(vtx_size / sizeof(struct menu_vertex));
    v = (struct menu_vertex *)m->vbuf.memory.ptr;
    for (i = 0; i < n; i++) {
        v[i].position[0] = (v[i].position[0] / (float)m->width) * 2.0f - 1.0f;
        v[i].position[1] = 1.0f - (v[i].position[1] / (float)m->height) * 2.0f;
    }

    upload_buffer(m->device, m->vtx_up, m->vtx, m->vbuf.memory.ptr, vtx_size);
    upload_buffer(m->device, m->idx_up, m->idx, m->ibuf.memory.ptr, idx_size);

    cb = SDL_AcquireGPUCommandBuffer(m->device);
    if (!SDL_AcquireGPUSwapchainTexture(cb, m->window, &swapchain, &sw, &sh)) {
        SDL_SubmitGPUCommandBuffer(cb);
        return;
    }
    m->width = (int)sw;
    m->height = (int)sh;

    {
        SDL_GPUColorTargetInfo cti;
        SDL_GPURenderPass *rp;
        SDL_GPUBufferBinding vbind = { m->vtx, 0 };
        SDL_GPUBufferBinding ibind = { m->idx, 0 };
        SDL_GPUTextureSamplerBinding tsb = { m->font_tex, m->font_sampler };

        SDL_zero(cti);
        cti.texture = swapchain;
        cti.clear_color = (SDL_FColor){ 0.10f, 0.11f, 0.13f, 1.0f };
        cti.load_op = SDL_GPU_LOADOP_CLEAR;
        cti.store_op = SDL_GPU_STOREOP_STORE;

        rp = SDL_BeginGPURenderPass(cb, &cti, 1, NULL);
        SDL_BindGPUGraphicsPipeline(rp, m->pipeline);
        SDL_BindGPUVertexBuffers(rp, 0, &vbind, 1);
        SDL_BindGPUIndexBuffer(rp, &ibind, SDL_GPU_INDEXELEMENTSIZE_16BIT);
        SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);

        nk_draw_foreach(cmd, &m->ctx, &m->cmds) {
            SDL_Rect scissor;
            if (!cmd->elem_count)
                continue;
            scissor.x = (int)cmd->clip_rect.x;
            scissor.y = (int)cmd->clip_rect.y;
            scissor.w = (int)cmd->clip_rect.w;
            scissor.h = (int)cmd->clip_rect.h;
            SDL_SetGPUScissor(rp, &scissor);
            SDL_DrawGPUIndexedPrimitives(rp, cmd->elem_count, 1,
                                         (Uint32)(elem_offset / sizeof(nk_draw_index)),
                                         0, 0);
            elem_offset += cmd->elem_count * sizeof(nk_draw_index);
        }
        SDL_EndGPURenderPass(rp);
    }

    SDL_SubmitGPUCommandBuffer(cb);
}

static void menu_shutdown(menu_t *m)
{
    nk_buffer_free(&m->cmds);
    nk_buffer_free(&m->vbuf);
    nk_buffer_free(&m->ibuf);
    nk_font_atlas_clear(&m->atlas);
    nk_free(&m->ctx);

    SDL_ReleaseGPUSampler(m->device, m->font_sampler);
    SDL_ReleaseGPUTexture(m->device, m->font_tex);
    SDL_ReleaseGPUTransferBuffer(m->device, m->vtx_up);
    SDL_ReleaseGPUTransferBuffer(m->device, m->idx_up);
    SDL_ReleaseGPUBuffer(m->device, m->vtx);
    SDL_ReleaseGPUBuffer(m->device, m->idx);
    SDL_ReleaseGPUGraphicsPipeline(m->device, m->pipeline);
    SDL_DestroyGPUDevice(m->device);
    SDL_DestroyWindow(m->window);
    SDL_Quit();
}

int main(int argc, char **argv)
{
    menu_t m;

    (void)argc;
    (void)argv;
    memset(&m, 0, sizeof m);
    menu_init(&m);
    menu_settings_load();

    while (m.running)
        menu_frame(&m);

    menu_shutdown(&m);
    return 0;
}
