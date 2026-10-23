# Native Linux tools

`tools/import-linux.py --root data PATH` imports a supported CHD, complete
ZIP/7z/RAR archive or extracted game folder. The same importer is embedded
in the first-time setup UI and in-game Nuklear menu. CHDs require installed
`chdman`, `mtools` and `7z`; ZIP/folder imports use Python 3 alone.

`make check-linux` builds the i386 replay harness, checks eight reference
frames, runs the native host/input/network tests and checks the importer.
`make check-linux-gpu` compares Vulkan output with the CPU reference,
including long batches and overlapping depths. It needs a Vulkan desktop.

`glide-replay.c` replays `.vglc` streams into a native shared library. Use
`--hash` for frame hashes, `--out DIR` for PNG images, `--aux-out DIR` for
raw depth buffers, `--present` for a window or `--list` for a summary.

`build-gpu-spirv.py` compiles project-authored HLSL with `glslc` and embeds
SPIR-V. `fetch-deps.py` downloads the pinned, hash-checked Nuklear header.
`embed-import.py` embeds the Python importer; `embed-text.py` embeds text.
`make-conformance-capture.py` creates synthetic scenes without game data.
