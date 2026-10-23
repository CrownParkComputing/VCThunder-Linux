# VCThunder — native Linux

VCThunder runs the original 32-bit x86 code of **Hydro Thunder** and
**Offroad Thunder** on Linux. The native host implements the arcade operating
system, Glide graphics, sound, controllers, storage and cabinet networking.
Windows build targets and host implementations have been removed. The original
game executables remain required data; their ETS import names and calling
conventions are implemented by Linux code.

## Build and run

On x86 Linux, with a compiler that supports `gcc -m32`, `make`, Python 3,
`pkg-config`, **glslc**, and **32-bit SDL3 and zlib development libraries**:

```sh
make linux -j4
./build/vcthunder              # play an installed game; setup if none installed
./build/vcthunder --menu       # open game import / setup explicitly
./build/vcthunder hydro
./build/vcthunder offroad
```

Run from the repository root. On first launch, choose **Choose CHD / ROM
archive** or **Choose extracted game folder**. Import your own Hydro Thunder
v01.01b or Offroad Thunder 2000-02-25 disk/files. CHD import also needs `chdman`
(MAME tools), `mtools` and `7z` installed. ZIP archives and extracted folders
work with Python alone. A BIOS-only ROM archive does not contain the game.
The importer collects files across disk partitions, checks the executable and
required assets, and copies binary sound banks without text conversion. It
installs to `data/hydro/` or `data/offroad/` (or the configured data root), keeps
existing installations and saves, and reports progress/errors in the menu.

This builds a native i386 ELF executable and `build/libvcglide.so`, using
Vulkan for rendering, SDL3 for windows, controls and audio, and pthreads/POSIX
for host services. No Wine, MinGW, Direct3D or Windows SDK is needed.
The existing `vcthunder.ini` is retained; saves go to `save/`.
Once a game is installed, launch goes directly into it. The separate frontend
only imports game data; settings live in the in-game Nuklear menu.

Insert coins with **5** (Hydro's default price is three coins). **1** starts;
**Left/Right** steer. Hydro uses **Up/Down** for throttle/brake and **Left Ctrl**
for boost/accept. Offroad uses **Left Ctrl** for gas/accept, **Left Alt** for
brake, **Space** for nitro and **Z/X/C** for gears. **Escape** quits and flushes
saves; **Alt+Enter** switches fullscreen.

An Xbox pad uses **left stick** to steer, **RT/LT** for gas/brake, **A** for
boost/accept, **View** for coin and **Menu** for start; Offroad uses **LB/RB**
to shift. USB/Bluetooth hotplug and both rumble motors use SDL's gamepad API.
Open the Nuklear menu with **F1** or **View + Menu**. Navigate with the mouse,
arrows/Enter, or D-pad/A; **Tab / LB / RB** change pages. **File > Import** and
the **Setup** tab can run setup again while the game is paused. Sound gain,
mute, bus routing/balance and fullscreen apply immediately. Other settings
take effect through **File > Restart game**.
**F3** toggles the measured FPS overlay. **Performance** also shows FPS and
lets you adjust game-clock speed (25–400%, in steps of 25). At 200% the game
can approach 60 FPS if the renderer can keep up, and gameplay speeds up
with it. Restart to apply. Interpolated 60 FPS at original game speed is not implemented;
`frame_rate=60` does not enable it. Cabinet CPU identity (`cpu_hz`) does not make rendering faster.

The native SDL3 GPU renderer uses **Vulkan** by default
(`vcglide_renderer=auto`). Install a **32-bit Vulkan loader and GPU driver** to
use it. Shaders are compiled from the existing HLSL to embedded SPIR-V at build
time; no runtime shader compiler or Wine is used. The FPS overlay identifies
the active backend as **Vulkan** or **CPU**. The in-game Display page controls
renderer selection, resolution scale (1–8x), and texture/HUD filtering; restart
the game to apply. A missing Vulkan device selects the CPU fallback. The CPU
path can also be selected explicitly with `vcglide_renderer=cpu`; it presents
through OpenGL when available, with an SDL software fallback.

Use `make check-linux` for the native regression suite and reference-frame
comparison. When extracting game data with mtools, use binary copying:
**never use `mcopy -a` or `-t`**, which truncate binary files at Ctrl-Z. In
particular, Hydro's sound banks should be megabytes, rather than kilobytes.
`make check-linux-gpu` additionally compares Vulkan output with the CPU
reference and tests long triangle batches with overlapping depths. This needs
a working Vulkan driver and a desktop video session.

## Two linked Offroad screens

From your home directory:

```sh
cd ~/VCThunder
./build/vcthunder offroad --dual-screen
```

Or select **F1 > File > Linked Offroad (two screens)** during a game.
Two connected displays get one cabinet each. A single display gets two
side-by-side windows. On X11/Xwayland the launcher places the windows;
native Wayland compositors may control placement themselves.
Cabinet IDs and linked mode are selected automatically. Each process uses
its own saves and logs under `save/offroad-link/cabinet-1/` and `cabinet-2/`.
Existing single-player saves are kept. Local UDP traffic stays on loopback.

Controller 1 drives cabinet 1; controller 2 drives cabinet 2, including when
its window is unfocused. Keyboard controls affect the focused cabinet.
Closing either cabinet closes its partner. Use `--dual-screen --dry-run` to
print the launch plan. `--save DIR` changes the parent save folder.
Both games still simulate at their original 30 Hz by default.

Resolution scale and texture/HUD filtering are available in the Display menu.
DLSS and frame generation are not integrated. The current 32-bit renderer
cannot load the 64-bit Linux NGX SDK; NVIDIA frame generation also requires
newer hardware than an RTX 3060. The HUD FSR1 filter is spatial scaling.

## Supported builds

Every address VCThunder patches is an offset into one exact executable. This host supports exactly two files:

| Title | Build | SHA-256 of the executable |
|---|---|---|
| Hydro Thunder | v01.01b, 1999-02-27 | `a421f5fd4ee0d90f57815975d4a47bb784f2b46d48085e42b422605a029f8e3f` |
| Offroad Thunder | 2000-02-25 | `fa739d0f1a80c6e429ca73fdee04c3d6d6325bb95848cb701114f74bf2a5e9bf` |

The import checks the executable inside a CHD against these before it writes anything, and says which build you have.

## Licensing

Original VCThunder source and documentation are MIT-licensed. See [`LICENSE`](LICENSE).
The native Linux menu in `menu/native_menu.c` adapts the DriveClub-Linux bbport
Nuklear menu under **GPL-2.0-or-later**; see [`menu/COPYING`](menu/COPYING).
The combined Linux executable is distributed under those GPL terms. Nuklear
itself is MIT/public domain and is fetched at its pinned revision on build.

SDL3 and zlib are system library dependencies. Nuklear is downloaded with a
pinned SHA-256 by `make deps`. CHD import uses installed `chdman`, `mtools`
and `7z` tools. Game data is not included.

Hydro Thunder, Offroad Thunder, Midway and 3dfx marks belong to their owners.
This project is unaffiliated with them.
