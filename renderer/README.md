# Native Linux Glide renderer

`build/libvcglide.so` implements the games' Glide 2.x calls. `auto` selects
the native SDL3 GPU backend through Vulkan, with a CPU fallback. No Windows
SDK, Direct3D or runtime shader compiler is used.

```ini
[graphics]
vcglide_renderer = auto  ; auto | gpu | cpu
render_scale = 1        ; GPU internal resolution scale, 1..8
```

`tools/build-gpu-spirv.py` compiles `posix/gpu_shader.hlsl` to embedded SPIR-V
with `glslc`. Vulkan batches hold 16 triangles (3072 bytes), within SDL 3.4's
4096-byte uniform binding range. The Nuklear menu is a transparent GPU layer.

`make check-linux` checks CPU frames against the original reference hashes.
`make check-linux-gpu` compares Vulkan and CPU scenes, including batches
longer than the uniform limit and overlapping depth writes.

## How this code is sourced

Everything here is derived from the published Glide documentation, from our own measurements
of the two games, and from first-party experimentation, and from nothing else. **Other Glide
implementations may be run and observed; they may not be read.** That rule is stricter than
the rest of the repository's and binds from the first commit here, because it cannot be
retrofitted.

Every non-obvious constant names its origin at the point of use: `GR_VERTEX_BYTES = 60` is
derived from Hydro's own statically linked Glide advancing a vertex array by `count * 3 * 5 * 4`.
Structure layouts are written as offsets poked by byte arithmetic, never as transcribed struct
declarations, and `api/glide2.h` is authored here rather than being an SDK header.

## Layout

- `api/entries.def`: the measured Glide entry points and argument sizes.
- `api/glide2.h`: project-authored interface constants and offsets.
- `core/vcglide.c`: configuration, exported entry points and frame dumping.
- `core/native.c`: decoded entry-point arguments.
- `core/device.c`, `raster.c`, `tex.c`: reference raster and texture pipeline.
- `core/capture.c`, `capture.h`, `png.c`: capture format and PNG output.
- `core/platform.h`: Linux clocks, environment and readable-memory queries.
- `posix/gpu.c`, `gpu_shader.hlsl`: Vulkan drawing, textures and depth buffers.
- `posix/gpu_overlay.inc`: in-game menu composition.
- `posix/gpu_sdl.c`: SDL presentation, window adoption and event forwarding.
- `posix/par_linux.c`: pthread raster-worker pool.

The renderer shares no source or headers with the native host in `src/`.
