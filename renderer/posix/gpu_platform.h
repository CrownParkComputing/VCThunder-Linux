/* SPDX-License-Identifier: MIT */
#ifndef VGL_GPU_PLATFORM_H
#define VGL_GPU_PLATFORM_H
#include "../core/platform.h"
#include <SDL3/SDL.h>
#include <sched.h>
typedef struct { int64_t ticks; } gpu_clock_t;
static inline void gpu_clock_now(gpu_clock_t *v)
{ v->ticks = (int64_t)SDL_GetPerformanceCounter(); }
static inline void gpu_clock_frequency(gpu_clock_t *v)
{ v->ticks = (int64_t)SDL_GetPerformanceFrequency(); }
typedef struct { const uint32_t *data; size_t size; const char *entry; } shader_blob_t;
#endif
