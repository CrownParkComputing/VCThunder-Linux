/* SPDX-License-Identifier: MIT
 * Nuklear's existing SDL renderer draws only the transparent UI. The game
 * scene stays on the GPU; this layer is uploaded and composed by Vulkan.
 */
#include "overlay.h"
static SDL_Surface *s_ui_surface;
static SDL_Renderer *s_ui_renderer;
static SDL_GPUTexture *s_ui_texture;
static SDL_GPUTransferBuffer *s_ui_upload;
static SDL_GPUGraphicsPipeline *s_ui_pipeline;
static SDL_GPUSampler *s_ui_sampler;
static Uint32 s_ui_w, s_ui_h, s_ui_pitch;

static void gpu_overlay_close(void)
{
    if (s_ui_renderer) {
        vgl_overlay_draw(s_ui_renderer, 0, 0);
        SDL_DestroyRenderer(s_ui_renderer);
    }
    if (s_ui_surface) SDL_DestroySurface(s_ui_surface);
    if (s_ui_texture) SDL_ReleaseGPUTexture(device(), s_ui_texture);
    if (s_ui_upload) SDL_ReleaseGPUTransferBuffer(device(), s_ui_upload);
    if (s_ui_pipeline) SDL_ReleaseGPUGraphicsPipeline(device(), s_ui_pipeline);
    if (s_ui_sampler) SDL_ReleaseGPUSampler(device(), s_ui_sampler);
    s_ui_surface = NULL; s_ui_renderer = NULL; s_ui_texture = NULL;
    s_ui_upload = NULL; s_ui_pipeline = NULL; s_ui_sampler = NULL;
    s_ui_w = s_ui_h = s_ui_pitch = 0;
}

static int gpu_overlay_open(Uint32 w, Uint32 h)
{
    SDL_GPUTransferBufferCreateInfo transfer = {0};
    SDL_GPUSamplerCreateInfo sampler = {0};
    if (s_ui_w == w && s_ui_h == h && s_ui_renderer) return 1;
    gpu_overlay_close();
    s_ui_surface = SDL_CreateSurface((int)w, (int)h, SDL_PIXELFORMAT_RGBA32);
    s_ui_renderer = s_ui_surface ? SDL_CreateSoftwareRenderer(s_ui_surface) : NULL;
    s_ui_texture = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                               SDL_GPU_TEXTUREUSAGE_SAMPLER, w, h, 1);
    s_ui_pitch = align_up(w * 4, GPU_UPLOAD_ROW_ALIGN);
    transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer.size = s_ui_pitch * h;
    s_ui_upload = SDL_CreateGPUTransferBuffer(device(), &transfer);
    s_ui_pipeline = swapchain_pipeline(1);
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    s_ui_sampler = SDL_CreateGPUSampler(device(), &sampler);
    if (!s_ui_renderer || !s_ui_texture || !s_ui_upload || !s_ui_pipeline || !s_ui_sampler) {
        VGL_LOG_CAPPED(3, "gpu: menu overlay allocation failed: %s", SDL_GetError());
        gpu_overlay_close(); return 0;
    }
    s_ui_w = w; s_ui_h = h;
    return 1;
}

static int gpu_overlay_present(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *swap,
                               Uint32 w, Uint32 h)
{
    SDL_GPUTextureTransferInfo source = {0};
    SDL_GPUTextureRegion destination = {0};
    SDL_GPUColorTargetInfo color = {0};
    SDL_GPUTextureSamplerBinding binding;
    SDL_GPUCopyPass *copy;
    SDL_GPURenderPass *pass;
    float vd[10][4] = {{0}};
    if (!gpu_overlay_open(w, h)) return 0;
    SDL_SetRenderDrawColor(s_ui_renderer, 0, 0, 0, 0);
    SDL_RenderClear(s_ui_renderer);
    int paused = vgl_overlay_draw(s_ui_renderer, (int)w, (int)h);
    SDL_RenderPresent(s_ui_renderer);
    void *mapped = SDL_MapGPUTransferBuffer(device(), s_ui_upload, true);
    if (!mapped) { gpu_failed(GF_UPLOAD); return paused; }
    for (Uint32 y = 0; y < h; y++)
        memcpy((uint8_t *)mapped + y * s_ui_pitch,
               (uint8_t *)s_ui_surface->pixels + y * s_ui_surface->pitch, w * 4);
    SDL_UnmapGPUTransferBuffer(device(), s_ui_upload);
    copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) { gpu_failed(GF_COPYPASS); return paused; }
    source.transfer_buffer = s_ui_upload;
    source.pixels_per_row = s_ui_pitch / 4; source.rows_per_layer = h;
    destination.texture = s_ui_texture;
    destination.w = w; destination.h = h; destination.d = 1;
    SDL_UploadToGPUTexture(copy, &source, &destination, true);
    SDL_EndGPUCopyPass(copy);
    color.texture = swap; color.load_op = SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(cmd, &color, 1, NULL);
    if (!pass) { gpu_failed(GF_RENDERPASS); return paused; }
    binding.texture = s_ui_texture; binding.sampler = s_ui_sampler;
    SDL_BindGPUGraphicsPipeline(pass, s_ui_pipeline);
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    vd[0][2] = vd[0][3] = 1;
    SDL_PushGPUVertexUniformData(cmd, 0, vd, sizeof vd);
    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return paused;
}
