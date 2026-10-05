# SteamARM - Linux (and, through FEX, x86) programs on macOS with no VM:
# the lxrun runtime, the ELF Vulkan shim over MoltenVK, and the native
# launcher app. The VM-era front-end app was removed (docs/history/MIGRATION_PLAN.md,
# FINAL ZERO-VM EXIT CRITERIA item 9).

CC        := clang

.PHONY: all clean

all: lxrt shim launcher inputd wlmac ffx-metalfx

# steamarm-inputd: the Mac's controllers as /dev/input for guests (tools/inputd).
.PHONY: inputd
# steamarm-wlmac: the native macOS Wayland compositor for Android sessions
# (tools/wlmac/README.md; ANDROID_SESSION_COMPOSITOR=wlmac).
wlmac: build/steamarm-wlmac
build/steamarm-wlmac: tools/wlmac/wlmac.m tools/wlmac/build.sh
	tools/wlmac/build.sh
.PHONY: wlmac
inputd: build/steamarm-inputd
build/steamarm-inputd: tools/inputd/inputd.c
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -O2 -I/opt/homebrew/opt/sdl2/include/SDL2 $< \
	    -L/opt/homebrew/opt/sdl2/lib -lSDL2 -o $@

# SteamARM's FidelityFX API DLLs (tools/ffx-metalfx): x86_64 Wine builtins
# over a Windows game's amd_fidelityfx_dx12/_vk/_upscaler_dx12.dll, for its
# FSR 3.1 / FSR 4 to go to MetalFX. One source, one DLL per name; mingw-w64
# builds them (scripts/setup.sh installs it), scripts/install-ffx-metalfx.sh
# puts them in the x86 Steam root's /opt/steamarm/wine, and the game tool
# (tools/steamarm-fex-proton) points Wine at them for STEAMARM_WIN_UPSCALER=metalfx.
FFX_DIR  := build/ffx-metalfx/x86_64-windows
FFX_DLLS := $(addprefix $(FFX_DIR)/,amd_fidelityfx_dx12.dll amd_fidelityfx_vk.dll amd_fidelityfx_upscaler_dx12.dll)
FFX_SRCS := $(wildcard tools/ffx-metalfx/*.c tools/ffx-metalfx/*.h tools/ffx-metalfx/*.def) \
            tools/ffx-metalfx/build.sh tools/ffx-metalfx/wine-builtin-mark.py
.PHONY: ffx-metalfx
ffx-metalfx: $(FFX_DLLS)
$(FFX_DIR)/%.dll: $(FFX_SRCS)
	FFX_OUT=$@ tools/ffx-metalfx/build.sh

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
LXRT_SRCS := runtime/ids.c runtime/procpid.c runtime/main.c runtime/android_ids.c runtime/binder.c runtime/binder_hub.c runtime/props.c runtime/propsvc.c runtime/elf.c runtime/elfsect.c runtime/errno_map.c runtime/fsflags.c runtime/pathfd.c runtime/evdev.c runtime/rewrite.c \
             runtime/dirents.c runtime/jit.c runtime/wxsplit.c runtime/dispatch.c runtime/x18.c runtime/gbase.c runtime/arena.c runtime/guestprof.c runtime/arena_seg.s \
             runtime/epoll_eventfd.c runtime/fex_support.c runtime/fileops2.c runtime/offmap.c \
             runtime/futex_ops.c runtime/futex_waitv.c runtime/inotify.c runtime/ioctl_tty.c runtime/mounts.c runtime/memlog.c runtime/mremap.c runtime/timerfd_signalfd.c runtime/posixtimer.c runtime/proc_ext.c runtime/privmap.c runtime/shmirror.c runtime/sysv_ipc.c runtime/process.c runtime/procfs.c runtime/signal.c runtime/socket.c runtime/stack.c runtime/storemu.c runtime/subpage.c runtime/sysfs.c runtime/sysreg.c runtime/window.m runtime/remote_layer.m runtime/metalfx.m runtime/ntsync.c runtime/thread.c runtime/tls.c runtime/trampoline.S runtime/vdso_map.c runtime/vdso_blob.S
LXRT_CFLAGS := -arch arm64 -fmodules -Wall -Wextra -Wno-unused-parameter -O2 -Iruntime
LXRT_LDFLAGS := -framework Cocoa -framework Metal -framework QuartzCore -weak_framework MetalFX \
                -Wl,-segaddr,__LXRT_ARENA,0x13b000000 -Wl,-segprot,__LXRT_ARENA,rw,---
# LXRT_KEEP_X18 (default 1 since stage 28; LXRT_KEEP_X18=0 opts out): link
# lxrun as built against the macOS 12.3 SDK. xnu keeps x18 across exceptions
# for such a binary -- in the process it exec'd, not in a forked child
# (MEASURED on the M4 under macOS 27 and on the macos-15 CI runner:
# tests/x18_preserve/run.sh). The x18 rewriter (runtime/x18.c) stays on
# either way; what the kernel's x18 adds is code the runtime never rewrote,
# a JIT's: Linux HotSpot's C1/C2 and llvmpipe's LLVM JIT allocate x18 and
# computed wrong results or crashed without it
# (benchmarks/stage24-minecraft-prism.txt, benchmarks/stage28-keep-x18.txt:
# the whole test matrix passes with both builds). The flag only changes the
# SDK version the binary records (-platform_version needs no 12.3 SDK
# installed); if the link fails anyway, the rule links against the current
# SDK and says so. build/.lxrun-flavor remembers the choice (rewritten only
# when it changes): switching rebuilds lxrun.
LXRT_KEEP_X18 ?= 1
ifeq ($(LXRT_KEEP_X18),1)
# SteamARM needs macOS 14 anyway: calls newer than 12.0 (mkfifoat) are there.
LXRT_KEEP_X18_CFLAGS := -mmacosx-version-min=12.0 -Wno-unguarded-availability-new
LXRT_KEEP_X18_LDFLAGS := -Wl,-platform_version,macos,12.0,12.3
endif
LXRT_FLAVOR := build/.lxrun-flavor
$(LXRT_FLAVOR): FORCE
	@mkdir -p build
	@echo "LXRT_KEEP_X18=$(LXRT_KEEP_X18)" | cmp -s - $@ || echo "LXRT_KEEP_X18=$(LXRT_KEEP_X18)" > $@
.PHONY: FORCE
FORCE:

# The vDSO (M6): a Linux aarch64 shared object, embedded by vdso_blob.S.
VDSO_CC := /opt/homebrew/opt/llvm/bin/clang
runtime/vdso/vdso.so: runtime/vdso/vdso.c runtime/vdso/vdso.lds
	$(VDSO_CC) --target=aarch64-linux-gnu -O2 -fPIC -fno-jump-tables -ffreestanding -nostdlib \
	    -ffixed-x18 -fno-stack-protector -fno-asynchronous-unwind-tables -shared -fuse-ld=lld \
	    -Wl,-T,runtime/vdso/vdso.lds -Wl,--hash-style=both -Wl,-soname,linux-vdso.so.1 \
	    -Wl,--build-id=none -o $@ runtime/vdso/vdso.c

build/lxrun: $(LXRT_SRCS) runtime/lxrt.h runtime/ids.h runtime/android_ids.h runtime/binder.h runtime/props.h runtime/x18.h runtime/storemu.h runtime/offmap.h resources/lxrt.entitlements runtime/vdso/vdso.so $(LXRT_FLAVOR)
	@mkdir -p build
ifeq ($(LXRT_KEEP_X18),1)
	$(CC) $(LXRT_CFLAGS) $(LXRT_KEEP_X18_CFLAGS) $(LXRT_LDFLAGS) $(LXRT_KEEP_X18_LDFLAGS) $(LXRT_SRCS) -o $@.new || { \
	    echo "lxrun: linking as SDK 12.3 (LXRT_KEEP_X18=1) failed; linking against the current SDK" \
	         "instead. The kernel will zero x18 for JIT code (docs/X18_VIRTUALIZATION.md)." >&2; \
	    $(CC) $(LXRT_CFLAGS) $(LXRT_LDFLAGS) $(LXRT_SRCS) -o $@.new; }
else
	$(CC) $(LXRT_CFLAGS) $(LXRT_LDFLAGS) $(LXRT_SRCS) -o $@.new
endif
	@sdk=$$(otool -l $@.new | awk '/LC_BUILD_VERSION/{f=1} f && $$1 == "sdk" {print $$2; exit}'); \
	    if [ -n "$$sdk" ] && [ "$${sdk%%.*}" -lt 13 ]; then \
	        echo "lxrun: LC_BUILD_VERSION sdk $$sdk: the kernel keeps x18 for JIT code"; \
	    else echo "lxrun: LC_BUILD_VERSION sdk $$sdk: the kernel zeroes x18 for JIT code"; fi
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

# Inspect native arm64 Steam images without starting the client.
.PHONY: test-arm64
test-arm64: lxrt
	./tests/arm64/run.sh

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
             shim/memcap.c shim/present.c shim/scaler.c shim/mailbox.c shim/spirv_names.c shim/spirv_dref.c shim/spirv_invariant.c \
             shim/mvkfix.c shim/a2c.c shim/mfx_temporal.c build/vk_rebase.c
build/libvulkan.so.1: $(SHIM_SRCS) shim/scaler_spv.h shim/mfx_temporal.h shim/adaptive_sync.h runtime/include/lxrt_host.h
	@mkdir -p build
	$(CC) -target $(LXRT_TARGET) -shared -fPIC -nostdlib -O2 \
	      -fuse-ld=$(CROSS_LD) -Iruntime/include -I$(VK_HEADERS)/include \
	      -Wl,-soname,libvulkan.so.1 -Wl,-Bsymbolic \
	      -o $@.new $(SHIM_SRCS)
	@mv -f $@.new $@

.PHONY: shim
shim: build/libvulkan.so.1

# Host-only x18 decoder/planner audit (no guest execution).
build/x18_check: tests/x18_check.c runtime/x18.c runtime/x18.h
	@mkdir -p build
	$(CC) $(LXRT_CFLAGS) -std=c17 tests/x18_check.c runtime/x18.c -o $@

# Host-only: function tables and fault-report file names across many
# dlopen/dlclose cycles of real libraries (tests/elfsect_unmap_check.c).
build/elfsect_unmap_check: tests/elfsect_unmap_check.c runtime/elfsect.c runtime/memlog.c runtime/lxrt.h
	@mkdir -p build
	$(CC) $(LXRT_CFLAGS) tests/elfsect_unmap_check.c runtime/elfsect.c runtime/memlog.c -o $@

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
# the settings -> environment translation and the compatibility inventory,
# run-app.sh's runner choice, the session wrapper (scripts/session.py), a
# root's guest environment (scripts/guest-env.sh), the root builders' path
# checks and swap (scripts/roots.sh: mkarmroot.sh, mkframeroot.sh) and what
# run-steam-arm64.sh links, refuses and stops, and the arm64 client's
# compatibility tool for x86 Proton through FEX (tools/steamarm-fex-proton,
# its installer and its dry run), apps.json compatibility of AppEntry, and
# the APK inspector and Android package manager on synthetic APKs
# (scripts/apk-inspect.py, scripts/android-pm.py). All of them also run on Linux
# (guest_env.sh skips its scripts/mkframeroot.sh part there; mkarmroot.sh and
# run_steam_arm64.sh skip: APFS clones, launchd reparenting).
.PHONY: test-launcher-core test-adaptive-sync
test-adaptive-sync:
	@mkdir -p build
	$(CC) -O2 -I$(VK_HEADERS)/include tests/elf/adaptive_sync.c -o build/adaptive-sync-tests
	build/adaptive-sync-tests
	$(CC) -O2 -I$(VK_HEADERS)/include tests/elf/adaptive_present.c -o build/adaptive-present-tests
	build/adaptive-present-tests

test-launcher-core: test-adaptive-sync
	@mkdir -p build
	swiftc -parse-as-library -swift-version 5 launcher/ApplicationCore.swift \
	    launcher/tests/ApplicationCoreTests.swift -o build/application-core-tests
	build/application-core-tests
	swiftc -parse-as-library -swift-version 5 launcher/ApplicationCore.swift launcher/Models.swift \
	    launcher/tests/AppEntryTests.swift -o build/app-entry-tests
	build/app-entry-tests
	python3 -m unittest tests/test_settings_env.py tests/test_compat_status.py tests/test_docs_records.py \
	    tests/test_apk_inspect.py tests/test_android_pm.py tests/test_android_gapps.py
	tests/launcher/run_app_dispatch.sh
	tests/launcher/session.sh
	tests/launcher/safeguard.sh
	tests/launcher/guest_env.sh
	tests/launcher/mkarmroot.sh
	tests/launcher/run_steam_arm64.sh
	tests/launcher/fex_proton_tool.sh

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
