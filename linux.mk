# Native i386 host. The original game code runs in this process; no Wine.
# SPDX-License-Identifier: MIT
LINUX_CC ?= gcc
LINUX_FLAGS := -m32 -msse2 -mfpmath=sse -mpreferred-stack-boundary=4 \
               -mincoming-stack-boundary=2 -std=gnu11 -O2 -g \
               -Wall -Wextra -Wshadow -Wno-format-truncation -MMD -MP \
               -I. -Isrc -Igenerated -Ibuild/native/generated -Ibuild/deps/nuklear $(shell pkg-config --cflags sdl3 2>/dev/null)
LINUX_LIBS := $(shell pkg-config --libs sdl3 2>/dev/null) -pthread -lm -ldl -lz
LINUX_SRC := src/host_main.c src/host_stubs.c src/main.c src/loader.c \
             src/patch.c src/win32_bind.c src/win32_posix.c src/stubs.c \
             src/glide_bind.c src/game.c src/log.c src/config.c src/ini.c \
             src/hash.c src/cksum.c src/fs_map.c src/nvram.c src/timebase.c \
             src/net_link.c src/dual_screen.c src/window.c src/window_posix.c src/settings.c \
             src/di_role.c src/r2_name.c src/widescreen.c src/bezel.c \
             src/netsock_posix.c src/audio.c src/audio_offroad.c src/posix_profile.c src/posix_memory.c \
             src/diego.c src/io_board.c src/io_config.c src/io_host.c \
             src/dinput.c src/dinput_posix.c src/xinput_posix.c src/xinput_hid.c \
             src/rumble.c src/ffb_events.c src/diag.c src/ini_doc.c menu/native_menu.c \
             generated/import_linux_data.c \
             generated/profiles.c generated/hydro_profile.c generated/offroad_profile.c
LINUX_OBJ := $(patsubst %.c,build/native/%.o,$(LINUX_SRC))
LINUX_RENDER_SRC := $(wildcard renderer/core/*.c) renderer/posix/par_linux.c \
                    renderer/posix/gpu.c renderer/posix/gpu_sdl.c
LINUX_RENDER_OBJ := $(patsubst %.c,build/native/%.o,$(LINUX_RENDER_SRC))

.PHONY: linux host
linux host: build/vcthunder build/libvcglide.so

.PHONY: check-linux-import
check-linux-import:
	$(PYTHON) tests/import_linux_test.py

.PHONY: check-linux-gpu
check-linux-gpu: linux
	sh tools/build-linux-conformance.sh
	$(PYTHON) tools/check-linux-gpu.py

generated/default_ini_data.c: vcthunder.ini tools/embed-text.py
	$(PYTHON) tools/embed-text.py $< $@ vcthunder_ini_data

build/native/src/config.o: generated/default_ini_data.c

generated/import_linux_data.c: tools/import-linux.py tools/embed-import.py scripts/titles.py
	$(PYTHON) tools/embed-import.py

build/deps/nuklear/nuklear.h:
	$(PYTHON) tools/fetch-deps.py nuklear

build/native/menu/native_menu.o: build/deps/nuklear/nuklear.h

build/native/generated/gpu_spirv.inc: renderer/posix/gpu_shader.hlsl tools/build-gpu-spirv.py
	$(PYTHON) tools/build-gpu-spirv.py

build/native/renderer/posix/gpu.o: build/native/generated/gpu_spirv.inc

build/vcthunder: $(LINUX_OBJ)
	$(LINUX_CC) -m32 -o $@ $^ $(LINUX_LIBS)

build/libvcglide.so: $(LINUX_RENDER_OBJ)
	$(LINUX_CC) -m32 -shared -Wl,-z,defs -o $@ $^ $(LINUX_LIBS)

build/native/%.o: %.c
	@mkdir -p $(dir $@)
	$(LINUX_CC) $(LINUX_FLAGS) -fPIC -c $< -o $@

-include $(LINUX_OBJ:.o=.d) $(LINUX_RENDER_OBJ:.o=.d)
