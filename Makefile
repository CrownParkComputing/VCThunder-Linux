# Native Linux i386 host and Vulkan renderer.
# SPDX-License-Identifier: MIT
PYTHON ?= python3
.DEFAULT_GOAL := linux
include linux.mk

.PHONY: all run menu deps check check-linux pack clean
all: linux
run: linux
	./build/vcthunder $(GAME)
menu: linux
	./build/vcthunder --menu
deps:
	$(PYTHON) tools/fetch-deps.py nuklear
check: check-linux
check-linux: linux check-linux-import
	sh tools/build-linux-conformance.sh
	sh tools/build-linux-tests.sh
pack: linux
	mkdir -p run/vcthunder
	install -m755 build/vcthunder build/libvcglide.so run/vcthunder/
	@test -f run/vcthunder/vcthunder.ini || cp vcthunder.ini run/vcthunder/
	cp LICENSE README.md run/vcthunder/
	cp menu/COPYING run/vcthunder/MENU-COPYING
clean:
	rm -rf build/native build/linux build/lt build/vcthunder build/libvcglide.so
