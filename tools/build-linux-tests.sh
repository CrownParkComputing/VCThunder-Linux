#!/bin/sh
# build-linux-tests.sh -- the host's portable test suite, on 32-bit Linux ELF.
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
#
# Each test in tests/ #includes the source it covers, so building one IS
# porting that source. This is the Linux counterpart of `make check`'s
# program-level half: the instruments that prove the host's logic behaves the
# same here, without needing a title, a window or a cabinet.
#
# The conformance hash covers the renderer. This covers the host. Neither covers
# the game-facing seams (the ETS kernel boundary, the fixed-base loader, the
# privileged-instruction layer), which need the host to actually run a title.
#
# rumble_model_test is absent on purpose: it includes src/rumble.c, which
# includes the DirectInput SDK's direct.h. That is input-tier work, not
# portable logic.
#
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=build/lt
mkdir -p "$OUT"

# Same core flags as the renderer build, for the same two reasons: the SSE
# x87-avoidance and -mpreferred-stack-boundary=2. See build-linux-conformance.sh
# for why the stack boundary is load-bearing rather than tuning.
#
# -Werror is back on. The Makefile builds with -Wall -Wextra -Wshadow -Werror
# and states the tree is clean at those flags; -w was only ever a staging
# measure while the port was mid-flight.
CFLAGS="-m32 -msse2 -mfpmath=sse -mpreferred-stack-boundary=4 -mincoming-stack-boundary=2 -std=gnu11 -O1 -Wall -Wextra -Wshadow -Werror -I. -Isrc -Igenerated"

# The port's own sources are held to the flags above with nothing waived.
#
# The UPSTREAM sources these tests #include are not, and the reason is the
# compiler version rather than anything about Linux. GCC 16 tightened
# -Wformat-truncation enough that the 19 snprintf sites in launcher_model.c and
# the tests now fail at -Werror. This is not a Linux-only diagnostic and not a
# Windows/Linux split: `make launcher-model-test` under the bundled
# i686-w64-mingw32-gcc fails with the same 13 errors on the same lines, and
# `make di-role-test` and `make widescreen-test` likewise. The Makefile's claim
# that the tree is clean at -Werror is true of the GCC it was written against
# and simply out of date here, where both compilers are 16.x.
#
# So the diagnostic is waived for the upstream TUs only, and posix_profile.c --
# the one file the port added here -- keeps it. That asymmetry is the point: it
# means a new truncation bug written in port code fails this build, without the
# port having to rewrite upstream code to satisfy an analysis upstream's own
# toolchain also fails.
PORT_CFLAGS="$CFLAGS"
TEST_CFLAGS="$CFLAGS -Wno-format-truncation"

# The shipped settings file, which Windows carries as RCDATA and Linux carries
# as a byte array. Generated here for the same reason the build generates it for
# config.c: src/config.c includes this file directly.
if [ ! -f generated/default_ini_data.c ] || \
   [ vcthunder.ini -nt generated/default_ini_data.c ]; then
    echo "==> embedding vcthunder.ini"
    python3 tools/embed-text.py vcthunder.ini \
        generated/default_ini_data.c vcthunder_ini_data
fi

TESTS="cabinet_layout_test di_role_test button_mask_test widescreen_shift_test ini_doc_test \
       launcher_model_test patch_emit_test config_test dinput_posix_test \
       xinput_posix_test rumble_model_test net_link_loopback_test \
       presenter_test window_posix_test hash_test loader_fixed_test win32_bind_test"

# rumble_model_test is the odd one out: it links src/ini.c, the real INI reader,
# because it writes an ini and has the reader parse it back -- that round trip is
# part of what it is checking. Every other test here is self-contained or needs
# only posix_profile.c. Selected per test below so the difference is visible
# rather than hidden in a special case.

# SDL3, and only for the one test that needs it. It is asked for through
# pkg-config rather than hard-coded, because a machine with the headers but no
# .pc file is a different problem from one with no SDL at all, and the two
# should not report the same failure. Absent SDL means dinput_posix_test is
# honestly SKIPPED -- a check that did not run must never read as one that
# passed.
SDL_CFLAGS=""
SDL_LIBS=""
if pkg-config --exists sdl3 2>/dev/null; then
    SDL_CFLAGS=$(pkg-config --cflags sdl3)
    SDL_LIBS=$(pkg-config --libs sdl3)
fi

# --- the input tier must BUILD, not merely be exercised by its own test ----
#
# tests/dinput_posix_test.c #includes dinput_posix.c, so it proves the
# implementation compiles -- but it never touches src/dinput.c, which is the
# 1680-line upstream file the whole tier exists to make compile. Those #ifdef
# guards are load-bearing: break one and dinput.c stops building on Linux with
# nothing in this suite noticing.
#
# So build both and LINK them. Linking is the part that matters and the part a
# -c compile would miss: the __wrapper macros in dinput_posix.h are what connect
# dinput.c's calls to dinput_posix.c's definitions, and a macro that resolves to
# an undefined symbol is exactly the failure mode this step exists to catch.
# Building a shared object rather than a binary is what keeps unresolved
# externals visible without needing a main().
# SDL3 IS BUILT AS AN ORDINARY 16-BYTE-ALIGNED ELF LIBRARY, so every
# translation unit that CALLS INTO IT must also be compiled for that ABI.
#
# That is the opposite requirement to the renderer and the replay harness, and
# getting it backwards segfaults on the first SDL call. -mpreferred-stack-boundary=2
# is what those two need: the 69 Glide entry points are entered through
# call_stdcall's hand-written asm, which leaves the stack 4 mod 16, and GCC's
# auto-vectorised code then faults on movaps. Nothing here goes through those
# thunks, so these files need the opposite -- and verified they need it: the
# same SDL probe runs clean under the ELF default and segfaults on the first
# SDL_WasInit under the flag.
#
# So the flag is FILTERED OUT for the SDL-facing files rather than added to
# them. That is deliberate and is not a relaxation: -Werror and the rest stay.
SDL_SAFE_CFLAGS=$(printf '%s\n' $PORT_CFLAGS | grep -v -- '-mpreferred-stack-boundary=2')

if [ -n "$SDL_LIBS" ]; then
    if ! gcc $SDL_SAFE_CFLAGS -Wno-format-truncation -Wno-unused-function \
            $SDL_CFLAGS -fPIC -shared -o "$OUT/libvcthinput.so" \
            src/dinput.c src/dinput_posix.c \
            src/xinput_posix.c src/posix_profile.c src/xinput_hid.c \
            $SDL_LIBS 2>"$OUT/input.build"; then
        echo "input tier: BUILD FAILED"
        grep -m3 -E 'error:|undefined reference' "$OUT/input.build" \
            | sed 's/^/    /'
        exit 1
    fi
    # Undefined symbols are NOT all failures here, and treating them as such
    # would be wrong: src/dinput.c legitimately calls into other host
    # translation units (ini.c, di_role.c) that this object deliberately does
    # not link, because the host is not buildable yet -- see the handover's
    # items 7-12. Those are expected and are not reported.
    #
    # What IS a failure is an unresolved symbol belonging to the input tier
    # itself: that is a __wrapper macro naming a function nobody wrote, which
    # links as a shared object and would only surface at load time.
    undef=$(nm -u --dynamic "$OUT/libvcthinput.so" 2>/dev/null \
            | awk '{print $2}' \
            | grep -E '^(IDirectInput|posix_di|XInput|joyGet)' | grep -v '@' || true)
    if [ -n "$undef" ]; then
        echo "input tier: a backend does not resolve:"
        printf '%s\n' "$undef" | sed 's/^/    /'
        exit 1
    fi
    hostdeps=$(nm -u --dynamic "$OUT/libvcthinput.so" 2>/dev/null \
               | awk '{print $2}' \
               | grep -vE '^(__|_ITM|_edata|_end|SDL)' \
               | grep -vE '@GLIBC|@(GCC|GLIBCPP)' \
               | grep -vE '^(IDirectInput|posix_di|XInput|joyGet)' \
               | grep -v '^$' \
               | wc -l)
    echo "input tier: dinput.c + input_probe.c and their three backends build,"
    echo "            link, and resolve ($hostdeps further host symbol(s) await"
    echo "            the host build -- ini.c, di_role.c and the rest)"
    echo
fi

pass=0
fail=0
for t in $TESTS; do
    printf '%-26s ' "$t"
    # Per-test flags. A test that reaches SDL3 must be built for the ELF ABI,
    # or it segfaults on the first SDL call (see SDL_SAFE_CFLAGS above); the
    # others keep the full set. Picking this per test rather than globally is
    # what stops the flag leaking back in through TEST_CFLAGS -- which it did,
    # once, and the symptom was a crash that looked like a bug in the
    # implementation.
    if [ "$t" = dinput_posix_test ] || [ "$t" = xinput_posix_test ] || [ "$t" = window_posix_test ]; then
        TFLAGS="$SDL_SAFE_CFLAGS -Wno-format-truncation"
        PFLAGS="$SDL_SAFE_CFLAGS"
        if [ -z "$SDL_LIBS" ]; then
            echo "SKIPPED: no SDL3. This check did NOT run."
            continue
        fi
    elif [ "$t" = win32_bind_test ]; then
        # win32_bind_test #includes src/win32_posix.c, whose __stdcall entry
        # points are reached from the game's 4-mod-16 stack and which call
        # pthread (a 16-byte-aligned ELF library) -- the same realignment
        # requirement as the presenter calling into SDL3 below.
        TFLAGS="$SDL_SAFE_CFLAGS -mincoming-stack-boundary=2"
        PFLAGS="$SDL_SAFE_CFLAGS -mincoming-stack-boundary=2"
    elif [ "$t" = presenter_test ]; then
        # presenter_test #includes renderer/posix/gpu_sdl.c -- port code, in the
        # test's own TU -- so it gets NO waiver: -Wformat-truncation stays on
        # precisely because the file being checked is one the port wrote.
        #
        # It is also built the way that file is built for the conformance
        # library: ELF default alignment PLUS -mincoming-stack-boundary=2, which
        # is what makes GCC realign before its calls into SDL3. The test drives
        # the presenter from ordinary aligned C, so it would pass either way --
        # building it the same way the library is built is what keeps this test
        # describing the shipping object rather than a luckier copy of it.
        if [ -z "$SDL_LIBS" ]; then
            echo "SKIPPED: no SDL3. This check did NOT run."
            continue
        fi
        TFLAGS="$SDL_SAFE_CFLAGS -mincoming-stack-boundary=2"
        PFLAGS="$SDL_SAFE_CFLAGS -mincoming-stack-boundary=2"
    elif [ "$t" = hash_test ]; then
        # NO waiver, not even the upstream-facing one. src/hash.c's portable
        # half is the implementation this suite depends on for the loader's
        # "refuse an image whose digest does not match" rule, and neither it
        # nor tests/hash_test.c formats anything into a buffer, so there is
        # nothing for -Wformat-truncation to excuse. Holding it to the strict
        # flags is what caught the bug this test was written for: the output
        # loop wrote four hex digits per 32-bit state word at a stride of eight
        # and skipped every odd nibble, so it emitted a truncated digest -- and
        # had never been RUN, only syntax-checked, because no test built it.
        TFLAGS="$PORT_CFLAGS"
        PFLAGS="$PORT_CFLAGS"
    else
        TFLAGS="$TEST_CFLAGS"
        PFLAGS="$PORT_CFLAGS"
    fi
    EXTRA=""
    if [ "$t" = rumble_model_test ]; then
        EXTRA="src/ini.c"
    fi
    # net_link_loopback_test #includes src/net_link.c and drives the Winsock
    # boundary over REAL BSD sockets -- so building and running it IS porting
    # the netcode tier. Its backing, src/netsock_posix.c, is the port's own
    # source and is compiled separately at the strict flags, exactly like
    # posix_profile.c below, so the -Wformat-truncation waiver for upstream TUs
    # cannot reach it.
    if [ "$t" = net_link_loopback_test ]; then
        if ! gcc $PFLAGS -c src/netsock_posix.c \
                -o "$OUT/netsock_posix.o" 2>>"$OUT/$t.build"; then
            fail=$((fail + 1))
            echo "BUILD FAILED (netsock_posix.c)"
            grep -m2 -E 'error:|fatal:' "$OUT/$t.build" | sed 's/^/    /'
            continue
        fi
        EXTRA="$OUT/netsock_posix.o"
    fi
    # The port's source is compiled separately and at the strict flags, so the
    # waiver above cannot reach it. A test that did not need it still pays for
    # this one compile.
    if ! gcc $PFLAGS -c src/posix_profile.c \
            -o "$OUT/posix_profile.o" 2>"$OUT/$t.build"; then
        fail=$((fail + 1))
        echo "BUILD FAILED (posix_profile.c)"
        grep -m2 -E 'error:|fatal:' "$OUT/$t.build" | sed 's/^/    /'
        continue
    fi
    THREADS=""
    if [ "$t" = win32_bind_test ]; then THREADS="-pthread"; fi
    if ! gcc $TFLAGS $SDL_CFLAGS -o "$OUT/$t" \
            "tests/$t.c" "$OUT/posix_profile.o" src/posix_memory.c $EXTRA $SDL_LIBS -lm -pthread $THREADS \
            2>>"$OUT/$t.build"; then
        fail=$((fail + 1))
        echo "BUILD FAILED"
        grep -m2 -E 'error:|fatal:' "$OUT/$t.build" | sed 's/^/    /'
        continue
    fi
    # The exit status is captured HERE, on the same line the test is run on.
    # `if out=$(cmd)` reports the status correctly inside the `if`, but by the
    # time the else-branch printed it, $? belonged to the grep that had just
    # tested for SKIPPED -- so a failing test announced itself as
    # "FAILED (exit 0)", which reads like the harness itself is broken and was
    # first noticed on hash_test.
    out=$("$OUT/$t" 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then
        pass=$((pass + 1))
        echo "pass  | $(printf '%s' "$out" | tail -1)"
    elif printf '%s\n' "$out" | grep -q ': SKIPPED'; then
        # A test prints "SKIPPED, <why>" and exits nonzero when it could not run
        # at all -- here, no SDL video backend even with the dummy driver. That is
        # counted as neither a pass nor a failure, and never as a green: the last
        # line says what did not run.
        echo "SKIPPED | $(printf '%s' "$out" | tail -1)"
    else
        fail=$((fail + 1))
        echo "FAILED (exit $rc)"
        printf '%s\n' "$out" | tail -5 | sed 's/^/    /'
    fi
done

echo


echo
echo "linux-tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
