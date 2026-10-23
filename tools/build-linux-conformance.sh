#!/bin/sh
# build-linux-conformance.sh -- the Linux conformance loop, on a 32-bit host.
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
#
# Builds the renderer's CPU reference path and the replay harness as 32-bit ELF,
# then proves it against tests/conformance-frames.hash. That hash is the whole
# point: it was taken on Windows/MinGW, so a match says the port reproduces the
# rasteriser pixel for pixel, and a mismatch says exactly which frame moved.
#
# This is the CPU backend only, and deliberately so. The GPU backend has no
# equivalent oracle -- renderer/README.md records that the two are different
# code paths and a result from one says nothing about the other.
#
# -Werror is on. See the CFLAGS note below.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

OUT=build/linux
CAP=build/conformance.vglc
GOLDEN=tests/conformance-frames.hash

# Absolute, because the replay runs from inside $OUT and a relative path there
# would silently resolve against build/linux rather than the repository root.
CAP_ABS=$ROOT/$CAP
GOLDEN_ABS=$ROOT/$GOLDEN

# The project's own core flags, with the cross-compiler's -m32 standing in for
# it. -msse2 -mfpmath=sse keeps the x87 stack free for the game's code, and is
# load-bearing here for a second reason: x87 80-bit intermediates would make the
# frame hash differ from the Windows build for reasons that have nothing to do
# with the port.
#
# -Werror is on, with nothing waived -- unlike the host test build, which has
# to waive -Wformat-truncation for the upstream sources it compiles in. The
# renderer and the harness build clean here at -Wall -Wextra -Wshadow, upstream
# core included, so there is nothing to waive and keeping it honest means the
# next change that introduces a warning fails the conformance run rather than
# slipping past it.
#
# -mpreferred-stack-boundary=2 is not optional and is not tuning. MinGW-w64
# defaults to 4-byte stack alignment because that is all the Win32 ABI
# guarantees; Linux ELF defaults to 16 bytes. The 69 entry points are reached
# through call_stdcall's hand-written asm, which pushes a variable number of
# 4-byte words and calls -- so a 3-argument call enters the callee with the
# stack 4 mod 16. Under the ELF default GCC auto-vectorises the plane setup and
# emits `movaps` against a stack slot, which faults on the misalignment the
# harness causes. This flag restores the assumption the harness was written
# under, and is why the same binary runs on both. The alternative -- realigning
# inside every stub -- is left alone because the asm is shared with the Windows
# build and is correct there.
CFLAGS="-m32 -msse2 -mfpmath=sse -mpreferred-stack-boundary=2 -std=gnu11 -O2 -g -Wall -Wextra -Wshadow -Werror -fPIC -I. -Irenderer/core"

# ---- the presenter seam ---------------------------------------------------------------
#
# renderer/posix/gpu_sdl.c puts the frame the CPU rasteriser produced into an SDL
# window, which is what makes this build something a person can look at instead of
# a hash file. It does not replace the GPU refusal: gpu_null.c keeps returning 0
# from vgl_gpu_open(), so the frames presented are the reference-path frames the
# golden hash came from. Presentation reads the finished frame, it does not
# produce it, so asking for a window cannot move a pixel.
#
# Asked for through pkg-config, for the reason recorded in build-linux-tests.sh:
# a missing .pc file and a missing library are different problems and should not
# report the same failure. Without SDL3 the conformance run still runs -- the
# presenter falls back to refusing, exactly the behaviour it had before.
HAVE_SDL=0
if pkg-config --exists sdl3 2>/dev/null; then
    HAVE_SDL=1
fi

# NOT $CFLAGS, and this is the one place the boundary flag has to be inverted
# rather than removed. The Glide entry points are reached through call_stdcall's
# asm with the stack 4 mod 16, and core/ is compiled to match; SDL3 is an
# ordinary 16-byte-aligned ELF library. -mincoming-stack-boundary=2 tells GCC
# that the ENTRY stack is not what the ELF default assumes, so it realigns in the
# prologue and the calls out to SDL are aligned. Dropping -mpreferred-stack-
# boundary=2 alone -- correct for src/'s SDL-facing files, which are called from
# aligned C -- leaves the misalignment in place and segfaults inside SDL.
PRESENTER_CFLAGS="-m32 -msse2 -mfpmath=sse -mpreferred-stack-boundary=4 -mincoming-stack-boundary=2 -std=gnu11 -O2 -g -Wall -Wextra -Wshadow -Werror -fPIC -I. -Irenderer/core"
PRESENTER_O=""
PRESENTER_LIBS=""
NULL_DEFS=""
if [ "$HAVE_SDL" = 1 ]; then
    PRESENTER_CFLAGS="$PRESENTER_CFLAGS $(pkg-config --cflags sdl3)"
    PRESENTER_LIBS=$(pkg-config --libs sdl3)
    PRESENTER_O="$OUT/gpu_sdl.o"
else
    echo "NOTE: no SDL3 (pkg-config sdl3 failed); the CPU renderer will not present"
    NULL_DEFS="-DVGL_NO_PRESENTER"
fi

mkdir -p "$OUT"

if [ ! -f "$CAP" ]; then
    echo "==> generating $CAP"
    python3 tools/make-conformance-capture.py "$CAP"
fi

if [ -n "$PRESENTER_O" ]; then
    echo "==> renderer/posix/gpu_sdl.c (SDL3 presenter)"
    gcc $PRESENTER_CFLAGS -c renderer/posix/gpu_sdl.c -o "$PRESENTER_O"
    # Assert the load-bearing flag rather than trusting it. If a future flag
    # change quietly drops the realignment, the failure is a segfault inside SDL
    # at the first present, on a machine with a display -- not a build error, and
    # the conformance run itself never presents anything so it would stay green.
    # vgl_present_frame is the one that must have it: it makes the SDL calls.
    if ! objdump -d "$PRESENTER_O" --no-show-raw-insn |
           sed -n '/<vgl_present_frame>:/,/^$/p' |
           grep -q 'and.*0xfffffff0'; then
        echo "ERROR: $PRESENTER_O has no stack realignment in vgl_present_frame."
        echo "       It is called with the stack 4 mod 16 and SDL3 needs 16;"
        echo "       -mincoming-stack-boundary=2 is not reaching this file."
        exit 1
    fi
fi

echo "==> libvcglide.so (renderer/core + posix seam)"
# On ELF every non-static symbol is exported, so the generated PE .def file
# this project uses on Windows is not needed here: the 69 Glide entry points and
# grLfbReadRegion are all ordinary globals.
gcc $CFLAGS -shared -o "$OUT/libvcglide.so" \
    renderer/core/*.c \
    renderer/posix/par_linux.c \
    $NULL_DEFS renderer/posix/gpu_null.c \
    $PRESENTER_O \
    -lpthread -lm $PRESENTER_LIBS

echo "==> glide-replay"
gcc $CFLAGS -o "$OUT/glide-replay" \
    tools/glide-replay.c \
    renderer/core/png.c \
    -ldl -lm

echo "==> replay"
cd "$OUT"
# Exactly the environment `make conformance` sets. VCGLIDE_PRESENT=0 is what
# keeps this headless; VCGLIDE_RENDERER=cpu is what keeps it on the reference
# path the golden hash came from.
VCGLIDE_RENDERER=cpu VCGLIDE_PRESENT=0 VCGLIDE_LOG=conformance.log \
    ./glide-replay "$CAP_ABS" --provider ./libvcglide.so \
    --hash conformance.hash

echo
# The golden file was written on Windows and carries CRLF; this run writes LF.
# Compare the content, not the line endings -- a hash line that differs only in
# its terminator is the same frame. Written with mktemp rather than <() so this
# stays POSIX sh.
GOLDEN_N=$(mktemp)
HASH_N=$(mktemp)
tr -d '\r' < "$GOLDEN_ABS" > "$GOLDEN_N"
tr -d '\r' < conformance.hash > "$HASH_N"
if diff -u "$GOLDEN_N" "$HASH_N" > conformance.diff 2>&1; then
    echo "conformance: $(wc -l < conformance.hash) frames match the golden hashes"
    echo "conformance: the Linux CPU renderer is pixel-identical to the Windows build"
else
    echo "conformance: THE RENDERER'S OUTPUT CHANGED"
    cat conformance.diff
    rm -f "$GOLDEN_N" "$HASH_N"
    exit 1
fi
rm -f "$GOLDEN_N" "$HASH_N"

# ---- the present pass ------------------------------------------------------
#
# The check above proves GCC realigned vgl_present_frame. This one proves the
# realignment was the RIGHT thing, by presenting through the path the game
# actually uses: glide-replay enters grBufSwap through call_stdcall's asm, which
# leaves the stack 4 mod 16, and that is the stack vgl_present_frame is entered
# with. The presenter's own unit test cannot cover this -- it is called from
# ordinary aligned C -- so without this pass a flag change would put a segfault
# inside SDL3 while every other check still reported green.
#
# The dummy video driver keeps it headless. This cannot move the golden hash: the
# frames here are not hashed, and VCGLIDE_PRESENT only decides whether a
# finished frame is shown.
if [ -n "$PRESENTER_O" ]; then
    echo "==> present pass (through call_stdcall, dummy video)"
    # Absolute: the replay above already moved into $OUT, and a second relative
    # cd would fail there. Everything this pass writes belongs next to the hash.
    cd "$ROOT/$OUT"
    if ! SDL_VIDEODRIVER=dummy VCGLIDE_RENDERER=cpu VCGLIDE_LOG=present-pass.log \
           ./glide-replay "$CAP_ABS" --provider ./libvcglide.so --present \
           > present-pass.out 2>&1; then
        echo "present pass: the replay failed with presentation on"
        tail -8 present-pass.out | sed 's/^/    /'
        exit 1
    fi
    # 'present:' with SDL3 named is the library saying it opened a window, not
    # the harness, and not a quiet decline.
    if ! grep -q 'present: .*through SDL3' present-pass.log; then
        echo "present pass: the presenter never opened a window. Its log:"
        grep -i present present-pass.log | tail -5 | sed 's/^/    /'
        exit 1
    fi
    echo "present pass: OK -- a CPU frame was presented from the misaligned path"
fi