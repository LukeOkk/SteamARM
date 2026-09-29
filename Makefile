# SteamARM - Linux (and, through FEX, x86) programs on macOS with no VM:
# the lxrun runtime, the ELF Vulkan shim over MoltenVK, and the native
# launcher app. The VM-era front-end app was removed (docs/history/MIGRATION_PLAN.md,
# FINAL ZERO-VM EXIT CRITERIA item 9).

CC        := clang

.PHONY: all clean

all: lxrt shim launcher inputd

# steamarm-inputd: the Mac's controllers as /dev/input for guests (tools/inputd).
.PHONY: inputd
inputd: build/steamarm-inputd
build/steamarm-inputd: tools/inputd/inputd.c
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -O2 -I/opt/homebrew/opt/sdl2/include/SDL2 $< \
	    -L/opt/homebrew/opt/sdl2/lib -lSDL2 -o $@

resources/AppIcon.icns: scripts/make-icon.py scripts/make-icns.sh
	@python3 scripts/make-icon.py resources/icon.png
	@./scripts/make-icns.sh resources/icon.png $@

clean:
	rm -rf build

# ---------------------------------------------------------------- lxrt
# MIGRATION_PLAN Stage 2: run a Linux aarch64 ELF with no VM.
#
# Note: there is deliberately no -pagezero_size here. Darwin reserves the low
# 4 GiB of an arm64 process for __PAGEZERO, and shrinking it is not permitted:
# every size below the 4 GiB default makes the kernel SIGKILL the binary at
# exec (measured, see benchmarks/stage2-pagezero.txt). The consequence is that
# non-PIE Linux images, which link at 0x200000, cannot be loaded in-process.
LXRT_SRCS := runtime/procpid.c runtime/main.c runtime/elf.c runtime/elfsect.c runtime/errno_map.c runtime/fsflags.c runtime/pathfd.c runtime/evdev.c runtime/rewrite.c \
             runtime/dirents.c runtime/jit.c runtime/dispatch.c runtime/x18.c runtime/gbase.c \
             runtime/epoll_eventfd.c runtime/fex_support.c runtime/fileops2.c \
             runtime/futex_ops.c runtime/inotify.c runtime/ioctl_tty.c runtime/mounts.c runtime/memlog.c runtime/mremap.c runtime/timerfd_signalfd.c runtime/proc_ext.c runtime/privmap.c runtime/shmirror.c runtime/sysv_ipc.c runtime/process.c runtime/procfs.c runtime/signal.c runtime/socket.c runtime/stack.c runtime/subpage.c runtime/sysfs.c runtime/sysreg.c runtime/window.m runtime/remote_layer.m runtime/thread.c runtime/tls.c runtime/trampoline.S runtime/vdso_map.c runtime/vdso_blob.S
LXRT_CFLAGS := -arch arm64 -fmodules -Wall -Wextra -Wno-unused-parameter -O2 -Iruntime
LXRT_LDFLAGS := -framework Cocoa -framework Metal -framework QuartzCore

# The vDSO (M6): a Linux aarch64 shared object, embedded by vdso_blob.S.
VDSO_CC := /opt/homebrew/opt/llvm/bin/clang
runtime/vdso/vdso.so: runtime/vdso/vdso.c runtime/vdso/vdso.lds
	$(VDSO_CC) --target=aarch64-linux-gnu -O2 -fPIC -fno-jump-tables -ffreestanding -nostdlib \
	    -ffixed-x18 -fno-stack-protector -fno-asynchronous-unwind-tables -shared -fuse-ld=lld \
	    -Wl,-T,runtime/vdso/vdso.lds -Wl,--hash-style=both -Wl,-soname,linux-vdso.so.1 \
	    -Wl,--build-id=none -o $@ runtime/vdso/vdso.c

build/lxrun: $(LXRT_SRCS) runtime/lxrt.h runtime/x18.h resources/lxrt.entitlements runtime/vdso/vdso.so
	@mkdir -p build
	$(CC) $(LXRT_CFLAGS) $(LXRT_LDFLAGS) $(LXRT_SRCS) -o $@.new
	@# MAP_JIT needs the allow-jit entitlement, and a guest JIT needs MAP_JIT:
	@# Apple Silicon refuses plain read-write-execute to every process.
	@codesign -f -s - --entitlements resources/lxrt.entitlements $@.new
	@# Replace by rename, never in place: every running guest maps this file,
	@# and rewriting a signed executable under a live process gets that
	@# process killed at its next page-in (Xvnc and the Steam client died
	@# during a rebuild, MEASURED 2026-09-27). A rename leaves them the old inode.
	@mv -f $@.new $@

# The smallest Linux binary that exercises the whole path: map, rewrite, run.
CROSS_LD := /opt/homebrew/opt/lld/bin/ld.lld
build/hello-linux: tests/elf/hello.S
	@mkdir -p build
	$(CC) -target aarch64-unknown-linux-gnu -nostdlib -static-pie -fPIE \
	      -fuse-ld=$(CROSS_LD) -Wl,-e,_start -o $@ $<

.PHONY: lxrt test-lxrt
lxrt: build/lxrun build/hello-linux

test-lxrt: lxrt
	./tests/elf/run.sh

# ---------------------------------------------------------------- vulkan shim
# An ELF libvulkan.so.1 whose entry points tail-call into Mach-O MoltenVK
# (or, with STEAMARM_VK_ICD=kosmickrisp, Mesa's KosmicKrisp: shim/gen.py)
# through the runtime's host bridge. Built on the host: clang+lld can target
# Linux aarch64 directly, no cross toolchain and no guest needed.
LXRT_TARGET := aarch64-unknown-linux-gnu

# Guest pointers below 4 GiB (a guest address base) are rebased at the shim's
# entry by wrappers generated from the Vulkan registry (shim/gen_rebase.py).
VK_HEADERS := $(shell brew --prefix vulkan-headers 2>/dev/null || echo /opt/homebrew/opt/vulkan-headers)
VK_XML := $(VK_HEADERS)/share/vulkan/registry/vk.xml

build/vk_rebase.c build/vk_rebase.names: shim/gen_rebase.py shim/entrypoints.txt shim/manual.txt
	@mkdir -p build
	python3 shim/gen_rebase.py $(VK_XML) shim/entrypoints.txt shim/manual.txt build/vk_rebase.c

build/shim-overrides.txt: shim/overrides.txt build/vk_rebase.names
	cat shim/overrides.txt build/vk_rebase.names > $@

shim/vulkan_shim.S shim/vulkan_shim.c: shim/gen.py shim/entrypoints.txt build/shim-overrides.txt
	python3 shim/gen.py shim/entrypoints.txt shim/vulkan_shim.S shim/vulkan_shim.c build/shim-overrides.txt

SHIM_SRCS := shim/vulkan_shim.S shim/vulkan_shim.c shim/wsi.c shim/features.c shim/fallback.c shim/map32.c \
             shim/memcap.c shim/present.c build/vk_rebase.c
build/libvulkan.so.1: $(SHIM_SRCS) runtime/include/lxrt_host.h
	@mkdir -p build
	$(CC) -target $(LXRT_TARGET) -shared -fPIC -nostdlib -O2 \
	      -fuse-ld=$(CROSS_LD) -Iruntime/include -I$(VK_HEADERS)/include \
	      -Wl,-soname,libvulkan.so.1 \
	      -o $@.new $(SHIM_SRCS)
	@mv -f $@.new $@

.PHONY: shim
shim: build/libvulkan.so.1

# Host-only x18 decoder/planner audit (no guest execution).
build/x18_check: tests/x18_check.c runtime/x18.c runtime/x18.h
	@mkdir -p build
	$(CC) $(LXRT_CFLAGS) -std=c17 tests/x18_check.c runtime/x18.c -o $@

# ---------------------------------------------------------------- launcher
# The native SwiftUI launcher (launcher/SPEC.md): build/SteamARM.app with
# SDL2 (sdl2-compat) and the SDL3 it loads at runtime bundled in Frameworks.
LAUNCHER_APP   := build/SteamARM.app
LAUNCHER_SRCS  := $(wildcard launcher/*.swift)
LAUNCHER_BIN   := build/launcher/SteamARM
SDL2_PREFIX    := /opt/homebrew/opt/sdl2
SDL3_PREFIX    := /opt/homebrew/opt/sdl3
SDL2_DYLIB     := $(SDL2_PREFIX)/lib/libSDL2-2.0.0.dylib
SDL3_DYLIB     := $(SDL3_PREFIX)/lib/libSDL3.0.dylib

$(LAUNCHER_BIN): $(LAUNCHER_SRCS) launcher/SDLShim.h
	@mkdir -p build/launcher
	swiftc -O -target arm64-apple-macos14 -parse-as-library -swift-version 5 \
	    -module-name SteamARM \
	    -import-objc-header launcher/SDLShim.h \
	    -Xcc -I$(SDL2_PREFIX)/include/SDL2 \
	    -L$(SDL2_PREFIX)/lib -lSDL2-2.0.0 \
	    -Xlinker -rpath -Xlinker @executable_path/../Frameworks \
	    $(LAUNCHER_SRCS) -o $@
	@install_name_tool -change "$$(otool -D $(SDL2_DYLIB) | tail -1)" \
	    @rpath/libSDL2-2.0.0.dylib $@

# The launcher's application core without UI (launcher/ApplicationCore.swift),
# run-app.sh's runner choice and the session wrapper (scripts/session.py). All
# three also run on Linux.
.PHONY: test-launcher-core
test-launcher-core:
	@mkdir -p build
	swiftc -parse-as-library -swift-version 5 launcher/ApplicationCore.swift \
	    launcher/tests/ApplicationCoreTests.swift -o build/application-core-tests
	build/application-core-tests
	tests/launcher/run_app_dispatch.sh
	tests/launcher/session.sh
	tests/launcher/safeguard.sh

.PHONY: launcher
launcher: $(LAUNCHER_BIN) launcher/Info.plist.in
	@rm -rf $(LAUNCHER_APP)
	@mkdir -p $(LAUNCHER_APP)/Contents/MacOS $(LAUNCHER_APP)/Contents/Resources \
	    $(LAUNCHER_APP)/Contents/Frameworks
	@cp $(LAUNCHER_BIN) $(LAUNCHER_APP)/Contents/MacOS/SteamARM
	@sed 's|@PROJECT_DIR@|$(CURDIR)|' launcher/Info.plist.in > $(LAUNCHER_APP)/Contents/Info.plist
	@cp resources/AppIcon.icns $(LAUNCHER_APP)/Contents/Resources/AppIcon.icns
	@cp $(SDL2_DYLIB) $(LAUNCHER_APP)/Contents/Frameworks/libSDL2-2.0.0.dylib
	@# sdl2-compat dlopens SDL3; @loader_path/libSDL3.dylib is its first choice.
	@cp $(SDL3_DYLIB) $(LAUNCHER_APP)/Contents/Frameworks/libSDL3.dylib
	@chmod u+w $(LAUNCHER_APP)/Contents/Frameworks/*.dylib
	@install_name_tool -id @rpath/libSDL2-2.0.0.dylib $(LAUNCHER_APP)/Contents/Frameworks/libSDL2-2.0.0.dylib 2>/dev/null
	@install_name_tool -id @rpath/libSDL3.dylib $(LAUNCHER_APP)/Contents/Frameworks/libSDL3.dylib 2>/dev/null
	@codesign -f -s - $(LAUNCHER_APP)/Contents/Frameworks/libSDL2-2.0.0.dylib
	@codesign -f -s - $(LAUNCHER_APP)/Contents/Frameworks/libSDL3.dylib
	@codesign -f -s - $(LAUNCHER_APP)
	@echo "built $(LAUNCHER_APP)"
