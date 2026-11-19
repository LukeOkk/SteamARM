# SteamARM - Linux (and, through FEX, x86) programs on macOS with no VM:
# the lxrun runtime, the ELF Vulkan shim over MoltenVK, and the native
# launcher app. The VM-era front-end app was removed (docs/history/MIGRATION_PLAN.md,
# FINAL ZERO-VM EXIT CRITERIA item 9).

CC        := clang
STEAMARM_ROOTS_DIR ?= $(if $(STEAMARM_STATE),$(STEAMARM_STATE),$(if $(wildcard $(HOME)/Developer/SteamARM-roots/arm64root),$(HOME)/Developer/SteamARM-roots,$(HOME)/SteamARM-roots))
STEAMARM_ARM64_SYSROOT ?= $(STEAMARM_ROOTS_DIR)/arm64root
STEAMARM_LINUX_CLANG ?= $(shell brew --prefix llvm 2>/dev/null)/bin/clang

.PHONY: all clean

all: lxrt runtime-support shim launcher inputd fex-runner native-runner steam-arm64-runner fex-proton-tool fex-proton-install fex-host-build fex-thunks-build steam-install steamroot-gfx-install vnc-control steam-launcher steam-arm64-links audio x11-control image-volume wlmac ffx-metalfx ffx-metalfx-install settings-env app-plan app-runner compat-status wine-prefix-options session proton-command env-links steamroot-build frame-root-utils steamframe-image heroic-asar androidroot-tools android-pm arch-audit wine-builtin-mark safeguard build/x11spawn

# steamarm-inputd: the Mac's controllers as /dev/input for guests (tools/inputd).
.PHONY: inputd
inputd: build/steamarm-inputd build/steamarm-inputctl
# steamarm-wlmac: the native macOS Wayland compositor for Android sessions
# (tools/wlmac/README.md; ANDROID_SESSION_COMPOSITOR=wlmac).
wlmac: build/steamarm-wlmac
build/steamarm-wlmac: tools/wlmac/wlmac.m tools/wlmac/build.sh
	tools/wlmac/build.sh
.PHONY: wlmac
build/steamarm-inputd: tools/inputd/Cargo.toml tools/inputd/Cargo.lock tools/inputd/src/main.rs
	@mkdir -p build
	RUSTFLAGS="-L native=/opt/homebrew/opt/sdl2/lib" cargo build --offline --locked --release \
	    --manifest-path tools/inputd/Cargo.toml --target-dir build/rust-inputd
	install -m 755 build/rust-inputd/release/steamarm-inputd $@
	install -m 755 build/rust-inputd/release/steamarm-inputctl build/steamarm-inputctl

build/steamarm-inputctl: build/steamarm-inputd
	@test -x $@

.PHONY: fex-runner
fex-runner: build/steamarm-fex-runner
build/steamarm-fex-runner: tools/fex-runner/Cargo.toml tools/fex-runner/Cargo.lock tools/fex-runner/src/main.rs tools/common/safeguard.rs
	cargo build --offline --locked --release --manifest-path tools/fex-runner/Cargo.toml --target-dir build/rust-fex-runner
	install -m 755 build/rust-fex-runner/release/steamarm-fex-runner $@

# SteamARM's FidelityFX API DLLs (tools/ffx-metalfx): x86_64 Wine builtins
# over a Windows game's amd_fidelityfx_dx12/_vk/_upscaler_dx12.dll, for its
# FSR 3.1 / FSR 4 to go to MetalFX. One source, one DLL per name; mingw-w64
# builds them (scripts/setup.sh installs it), scripts/install-ffx-metalfx.sh
# puts them in the x86 Steam root's /opt/steamarm/wine, and the game tool
# (tools/steamarm-fex-proton) points Wine at them for STEAMARM_WIN_UPSCALER=metalfx.
FFX_DIR  := build/ffx-metalfx/x86_64-windows
FFX_DLLS := $(addprefix $(FFX_DIR)/,amd_fidelityfx_dx12.dll amd_fidelityfx_vk.dll amd_fidelityfx_upscaler_dx12.dll)
FFX_SRCS := $(wildcard tools/ffx-metalfx/*.c tools/ffx-metalfx/*.h tools/ffx-metalfx/*.def) \
            tools/ffx-metalfx/build.sh tools/wine-builtin-mark/Cargo.toml \
            tools/wine-builtin-mark/Cargo.lock tools/wine-builtin-mark/src/main.rs
.PHONY: ffx-metalfx
ffx-metalfx: $(FFX_DLLS)
$(FFX_DIR)/%.dll: $(FFX_SRCS) build/steamarm-wine-builtin-mark
	FFX_OUT=$@ tools/ffx-metalfx/build.sh

ffx-metalfx-install: ffx-metalfx fex-proton-tool build/steamarm-ffx-metalfx-install
build/steamarm-ffx-metalfx-install: tools/ffx-metalfx-install/Cargo.toml tools/ffx-metalfx-install/Cargo.lock tools/ffx-metalfx-install/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/ffx-metalfx-install/Cargo.toml --target-dir build/rust-ffx-metalfx-install
	install -m 755 build/rust-ffx-metalfx-install/release/steamarm-ffx-metalfx-install $@

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
LXRT_SRCS := runtime/procpid.c runtime/main.c runtime/lowpage.c runtime/pefile.c runtime/android_ids.c runtime/binder.c runtime/binder_hub.c runtime/propsvc.c runtime/evdev.c runtime/rewrite.c \
             runtime/jit.c runtime/wxsplit.c runtime/dispatch.c runtime/x18.c runtime/arena_seg.s \
             runtime/epoll_eventfd.c runtime/fex_support.c runtime/fileops2.c \
             runtime/futex_ops.c runtime/inotify.c runtime/mounts.c runtime/mremap.c runtime/netif.c runtime/timerfd_signalfd.c runtime/proc_ext.c runtime/sysv_ipc.c runtime/process.c runtime/procfs.c runtime/signal.c runtime/socket.c runtime/storemu.c runtime/subpage.c runtime/window.m runtime/remote_layer.m runtime/metalfx.m runtime/ntsync.c runtime/thread.c runtime/trampoline.S runtime/vdso_blob.S
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

RUNTIME_SUPPORT_LIB := build/rust-runtime-support/release/libsteamarm_runtime_support.a
.PHONY: runtime-support
runtime-support: $(RUNTIME_SUPPORT_LIB)
$(RUNTIME_SUPPORT_LIB): tools/runtime-support/Cargo.toml tools/runtime-support/Cargo.lock tools/runtime-support/build.rs runtime/runtime_support.rs runtime/arena.rs runtime/elf.rs runtime/elfsect.rs runtime/gbase.rs runtime/memlog.rs runtime/offmap.rs runtime/props.rs runtime/shmirror.rs runtime/errno_map.rs runtime/fsflags.rs runtime/guestprof.rs runtime/ids.rs runtime/ioctl_tty.rs runtime/dirents.rs runtime/pathfd.rs runtime/posixtimer.rs runtime/privmap.rs runtime/stack.rs runtime/sysfs.rs runtime/sysreg.rs runtime/vdso_map.rs runtime/futex_waitv.rs runtime/tls.rs runtime/socket_path.rs
	cargo build --offline --locked --release --manifest-path tools/runtime-support/Cargo.toml --target-dir build/rust-runtime-support

build/lxrun: $(LXRT_SRCS) runtime/lxrt.h runtime/ids.h runtime/android_ids.h runtime/binder.h runtime/props.h runtime/x18.h runtime/storemu.h runtime/offmap.h resources/lxrt.entitlements runtime/vdso/vdso.so $(LXRT_FLAVOR) $(RUNTIME_SUPPORT_LIB)
	@mkdir -p build
ifeq ($(LXRT_KEEP_X18),1)
	$(CC) $(LXRT_CFLAGS) $(LXRT_KEEP_X18_CFLAGS) $(LXRT_LDFLAGS) $(LXRT_KEEP_X18_LDFLAGS) $(LXRT_SRCS) $(RUNTIME_SUPPORT_LIB) -o $@.new || { \
	    echo "lxrun: linking as SDK 12.3 (LXRT_KEEP_X18=1) failed; linking against the current SDK" \
	         "instead. The kernel will zero x18 for JIT code (docs/X18_VIRTUALIZATION.md)." >&2; \
	    $(CC) $(LXRT_CFLAGS) $(LXRT_LDFLAGS) $(LXRT_SRCS) $(RUNTIME_SUPPORT_LIB) -o $@.new; }
else
	$(CC) $(LXRT_CFLAGS) $(LXRT_LDFLAGS) $(LXRT_SRCS) $(RUNTIME_SUPPORT_LIB) -o $@.new
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

# The client of the X server's spawn service (tools/x11spawn,
# patches/xquartz-spawn-service.patch): scripts/run-app.sh starts an app and
# the services beside it through it, in the X server's coalition (Game Mode).
build/x11spawn: tools/x11spawn/x11spawn.c
	@mkdir -p build
	$(CC) -O2 -Wall -Wextra -o $@ $<

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
RUNTIME_SUPPORT_TEST_LIB := build/rust-runtime-support-elfsect-test/release/libsteamarm_runtime_support.a
$(RUNTIME_SUPPORT_TEST_LIB): tools/runtime-support/Cargo.toml tools/runtime-support/Cargo.lock tools/runtime-support/build.rs runtime/runtime_support.rs runtime/elfsect.rs runtime/memlog.rs runtime/tls.rs
	cargo build --offline --locked --release --no-default-features --features elfsect-test --manifest-path tools/runtime-support/Cargo.toml --target-dir build/rust-runtime-support-elfsect-test
build/elfsect_unmap_check: tests/elfsect_unmap_check.c runtime/lxrt.h $(RUNTIME_SUPPORT_TEST_LIB)
	@mkdir -p build
	$(CC) $(LXRT_CFLAGS) tests/elfsect_unmap_check.c $(RUNTIME_SUPPORT_TEST_LIB) -o $@

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
# run-app.sh's runner choice, the Rust session wrapper, a
# root's guest environment (scripts/guest-env.sh), the root builders' path
# checks and swap (scripts/roots.sh: mkarmroot.sh, mkframeroot.sh), and the
# run-scoped process handling plus Rust-owned guest links for native Steam ARM64,
# and the arm64 client's
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

test-launcher-core: test-adaptive-sync session settings-env safeguard image-volume \
	frame-root-utils steamroot-build app-plan app-runner wine-prefix-options \
	steam-arm64-runner fex-proton-tool
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
	tests/launcher/image_volume.sh
	tests/launcher/safeguard.sh
	tests/launcher/guest_env.sh
	tests/launcher/mkarmroot.sh
	tests/launcher/run_steam_arm64.sh
	tests/launcher/fex_proton_tool.sh

.PHONY: release-check frame-client-install setup-runner settings-env compat-status wine-prefix-options session proton-command steam-launcher steam-arm64-links native-runner steam-arm64-runner fex-proton-tool fex-proton-install fex-host-build fex-thunks-build steam-install steamroot-gfx-install vnc-control audio x11-control image-volume steamroot-build frame-root-utils steamframe-image heroic-asar androidroot-tools android-pm arch-audit wine-builtin-mark safeguard ffx-metalfx-install launcher
release-check:
	@if command -v cargo >/dev/null 2>&1; then \
	    cargo build --offline --locked --release --manifest-path tools/release-check/Cargo.toml --target-dir build/rust-release-check; \
	else \
	    echo "Rust missing; update check will not be bundled in this developer build"; \
	fi

frame-client-install:
	@if command -v cargo >/dev/null 2>&1; then \
	    cargo build --offline --locked --release --manifest-path tools/frame-client-install/Cargo.toml --target-dir build/rust-frame-client-install; \
	    install -m 755 build/rust-frame-client-install/release/steamarm-frame-client-install build/steamarm-frame-client-install; \
	else \
	    echo "Rust missing; Steam Frame client installer will be built by setup"; \
	fi

setup-runner:
	@if command -v cargo >/dev/null 2>&1; then \
	    cargo build --offline --locked --release --manifest-path tools/setup/Cargo.toml --target-dir build/rust-setup; \
	    install -m 755 build/rust-setup/release/steamarm-setup build/steamarm-setup; \
	else \
	    echo "Rust missing; setup runner will be built by scripts/setup.sh"; \
	fi

settings-env: build/steamarm-settings-env
build/steamarm-settings-env: tools/settings-env/Cargo.toml tools/settings-env/Cargo.lock tools/settings-env/src/main.rs
	@if [ -n "$${STEAMARM_SETTINGS_ENV_BIN:-}" ] && [ -x "$$STEAMARM_SETTINGS_ENV_BIN" ]; then \
	    install -m 755 "$$STEAMARM_SETTINGS_ENV_BIN" $@; \
	else \
	    cargo build --offline --locked --release --manifest-path tools/settings-env/Cargo.toml --target-dir build/rust-settings-env; \
	    install -m 755 build/rust-settings-env/release/steamarm-settings-env $@; \
	fi

.PHONY: app-plan
app-plan: build/steamarm-app-plan
build/steamarm-app-plan: tools/app-plan/Cargo.toml tools/app-plan/Cargo.lock tools/app-plan/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/app-plan/Cargo.toml --target-dir build/rust-app-plan
	install -m 755 build/rust-app-plan/release/steamarm-app-plan $@

.PHONY: app-runner
app-runner: build/steamarm-app-runner
build/steamarm-app-runner: tools/app-runner/Cargo.toml tools/app-runner/Cargo.lock tools/app-runner/src/main.rs tools/common/safeguard.rs
	cargo build --offline --locked --release --manifest-path tools/app-runner/Cargo.toml --target-dir build/rust-app-runner
	install -m 755 build/rust-app-runner/release/steamarm-app-runner $@

compat-status: build/steamarm-compat-status
build/steamarm-compat-status: tools/compat-status/Cargo.toml tools/compat-status/Cargo.lock tools/compat-status/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/compat-status/Cargo.toml --target-dir build/rust-compat-status
	install -m 755 build/rust-compat-status/release/steamarm-compat-status $@

wine-prefix-options: build/steamarm-wine-prefix-options
build/steamarm-wine-prefix-options: tools/wine-prefix-options/Cargo.toml tools/wine-prefix-options/Cargo.lock tools/wine-prefix-options/src/main.rs
	@if [ -n "$${STEAMARM_WINE_PREFIX_OPTIONS_BIN:-}" ] && [ -x "$$STEAMARM_WINE_PREFIX_OPTIONS_BIN" ]; then \
	    install -m 755 "$$STEAMARM_WINE_PREFIX_OPTIONS_BIN" $@; \
	else \
	    cargo build --offline --locked --release --manifest-path tools/wine-prefix-options/Cargo.toml --target-dir build/rust-wine-prefix-options; \
	    install -m 755 build/rust-wine-prefix-options/release/steamarm-wine-prefix-options $@; \
	fi

session: build/steamarm-session
build/steamarm-session: tools/session/Cargo.toml tools/session/Cargo.lock tools/session/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/session/Cargo.toml --target-dir build/rust-session
	install -m 755 build/rust-session/release/steamarm-session $@

proton-command: build/steamarm-proton-command
build/steamarm-proton-command: tools/proton-command/Cargo.toml tools/proton-command/Cargo.lock tools/proton-command/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/proton-command/Cargo.toml --target-dir build/rust-proton-command
	install -m 755 build/rust-proton-command/release/steamarm-proton-command $@

steam-launcher: build/steamarm-steam-launcher
build/steamarm-steam-launcher: tools/steam-launcher/Cargo.toml tools/steam-launcher/Cargo.lock tools/steam-launcher/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/steam-launcher/Cargo.toml --target-dir build/rust-steam-launcher
	install -m 755 build/rust-steam-launcher/release/steamarm-steam-launcher $@

fex-host-build: build/steamarm-fex-host-build
build/steamarm-fex-host-build: tools/fex-host-build/Cargo.toml tools/fex-host-build/Cargo.lock tools/fex-host-build/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/fex-host-build/Cargo.toml --target-dir build/rust-fex-host-build
	install -m 755 build/rust-fex-host-build/release/steamarm-fex-host-build $@

fex-thunks-build: build/steamarm-fex-thunks-build
build/steamarm-fex-thunks-build: tools/fex-thunks-build/Cargo.toml tools/fex-thunks-build/Cargo.lock tools/fex-thunks-build/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/fex-thunks-build/Cargo.toml --target-dir build/rust-fex-thunks-build
	install -m 755 build/rust-fex-thunks-build/release/steamarm-fex-thunks-build $@

steam-install: build/steamarm-steam-install
build/steamarm-steam-install: tools/steam-install/Cargo.toml tools/steam-install/Cargo.lock tools/steam-install/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/steam-install/Cargo.toml --target-dir build/rust-steam-install
	install -m 755 build/rust-steam-install/release/steamarm-steam-install $@

steamroot-gfx-install: build/steamarm-steamroot-gfx-install
build/steamarm-steamroot-gfx-install: tools/steamroot-gfx-install/Cargo.toml tools/steamroot-gfx-install/Cargo.lock tools/steamroot-gfx-install/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/steamroot-gfx-install/Cargo.toml --target-dir build/rust-steamroot-gfx-install
	install -m 755 build/rust-steamroot-gfx-install/release/steamarm-steamroot-gfx-install $@

vnc-control: build/steamarm-vnc-control
build/steamarm-vnc-control: tools/vnc-control/Cargo.toml tools/vnc-control/Cargo.lock tools/vnc-control/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/vnc-control/Cargo.toml --target-dir build/rust-vnc-control
	install -m 755 build/rust-vnc-control/release/steamarm-vnc-control $@

steam-arm64-links: build/steamarm-steam-arm64-links
build/steamarm-steam-arm64-links: tools/steam-arm64-links/Cargo.toml tools/steam-arm64-links/Cargo.lock tools/steam-arm64-links/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/steam-arm64-links/Cargo.toml --target-dir build/rust-steam-arm64-links
	install -m 755 build/rust-steam-arm64-links/release/steamarm-steam-arm64-links $@

native-runner: build/steamarm-run-native
build/steamarm-run-native: tools/native-runner/Cargo.toml tools/native-runner/Cargo.lock tools/native-runner/src/main.rs tools/common/safeguard.rs
	cargo build --offline --locked --release --manifest-path tools/native-runner/Cargo.toml --target-dir build/rust-native-runner
	install -m 755 build/rust-native-runner/release/steamarm-run-native $@

steam-arm64-runner: build/steamarm-run-steam-arm64
build/steamarm-run-steam-arm64: tools/steam-arm64-runner/Cargo.toml tools/steam-arm64-runner/Cargo.lock tools/steam-arm64-runner/src/main.rs tools/common/safeguard.rs
	cargo build --offline --locked --release --manifest-path tools/steam-arm64-runner/Cargo.toml --target-dir build/rust-steam-arm64-runner
	install -m 755 build/rust-steam-arm64-runner/release/steamarm-run-steam-arm64 $@

fex-proton-tool: build/steamarm-fex-proton-host build/steamarm-fex-proton-arm64 build/steamarm-ffx-x86_64

build/steamarm-fex-proton-host: tools/fex-proton-tool/Cargo.toml tools/fex-proton-tool/Cargo.lock tools/fex-proton-tool/src/main.rs tools/fex-proton-install/src/compat_path.rs
	cargo build --offline --locked --release --manifest-path tools/fex-proton-tool/Cargo.toml --target-dir build/rust-fex-proton-tool-host
	install -m 755 build/rust-fex-proton-tool-host/release/steamarm-fex-proton-tool $@

build/steamarm-fex-proton-arm64: tools/fex-proton-tool/Cargo.toml tools/fex-proton-tool/Cargo.lock tools/fex-proton-tool/src/main.rs tools/fex-proton-install/src/compat_path.rs
	@test -f "$(STEAMARM_ARM64_SYSROOT)/usr/lib/libc.so" || { echo "no Steam Frame/Arch ARM64 sysroot at $(STEAMARM_ARM64_SYSROOT); build the Frame root first or set STEAMARM_ARM64_SYSROOT" >&2; exit 1; }
	CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER="$(STEAMARM_LINUX_CLANG)" \
	RUSTFLAGS="-C link-arg=--target=aarch64-linux-gnu -C link-arg=--sysroot=$(STEAMARM_ARM64_SYSROOT) -C link-arg=-fuse-ld=lld" \
	    cargo build --offline --locked --release --target aarch64-unknown-linux-gnu \
	    --manifest-path tools/fex-proton-tool/Cargo.toml --target-dir build/rust-fex-proton-tool-arm64
	install -m 755 build/rust-fex-proton-tool-arm64/aarch64-unknown-linux-gnu/release/steamarm-fex-proton-tool $@

build/steamarm-ffx-x86_64: tools/fex-proton-tool/Cargo.toml tools/fex-proton-tool/Cargo.lock tools/fex-proton-tool/src/main.rs tools/fex-proton-install/src/compat_path.rs
	CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_LINKER="$(STEAMARM_LINUX_CLANG)" \
	RUSTFLAGS="-C link-arg=--target=x86_64-linux-musl -C link-arg=-fuse-ld=lld" \
	    cargo build --offline --locked --release --target x86_64-unknown-linux-musl \
	    --manifest-path tools/fex-proton-tool/Cargo.toml --target-dir build/rust-fex-proton-tool-x86_64
	install -m 755 build/rust-fex-proton-tool-x86_64/x86_64-unknown-linux-musl/release/steamarm-fex-proton-tool $@

fex-proton-install: fex-proton-tool build/steamarm-fex-proton-install build/steamarm-fexserver-manager-arm64 build/steamarm-fex-arm64

build/steamarm-fex-proton-rust-arm64.stamp: tools/fex-proton-install/Cargo.toml tools/fex-proton-install/Cargo.lock tools/fex-proton-install/src/compat_path.rs tools/fex-proton-install/src/fexserver_manager.rs tools/fex-proton-install/src/fex_wrapper.rs
	@test -f "$(STEAMARM_ARM64_SYSROOT)/usr/lib/libc.so" || { echo "no Steam Frame/Arch ARM64 sysroot at $(STEAMARM_ARM64_SYSROOT); build the Frame root first or set STEAMARM_ARM64_SYSROOT" >&2; exit 1; }
	CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER="$(STEAMARM_LINUX_CLANG)" \
	RUSTFLAGS="-C link-arg=--target=aarch64-linux-gnu -C link-arg=--sysroot=$(STEAMARM_ARM64_SYSROOT) -C link-arg=-fuse-ld=lld" \
	    cargo build --offline --locked --release --target aarch64-unknown-linux-gnu \
	    --manifest-path tools/fex-proton-install/Cargo.toml --target-dir build/rust-fex-proton-install-arm64
	touch $@

build/steamarm-fexserver-manager-arm64: build/steamarm-fex-proton-rust-arm64.stamp
	install -m 755 build/rust-fex-proton-install-arm64/aarch64-unknown-linux-gnu/release/steamarm-fexserver-manager $@

build/steamarm-fex-arm64: build/steamarm-fex-proton-rust-arm64.stamp
	install -m 755 build/rust-fex-proton-install-arm64/aarch64-unknown-linux-gnu/release/steamarm-fex $@

build/steamarm-fex-proton-install: tools/fex-proton-install/Cargo.toml tools/fex-proton-install/Cargo.lock tools/fex-proton-install/src/main.rs tools/fex-proton-install/src/compat_path.rs tools/fex-proton-install/src/fexserver_manager.rs tools/fex-proton-install/src/fex_wrapper.rs
	cargo build --offline --locked --release --manifest-path tools/fex-proton-install/Cargo.toml --target-dir build/rust-fex-proton-install
	install -m 755 build/rust-fex-proton-install/release/steamarm-fex-proton-install $@

audio: build/steamarm-audio
build/steamarm-audio: tools/audio/Cargo.toml tools/audio/Cargo.lock tools/audio/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/audio/Cargo.toml --target-dir build/rust-audio
	install -m 755 build/rust-audio/release/steamarm-audio $@

x11-control: build/steamarm-x11-control
build/steamarm-x11-control: tools/x11-control/Cargo.toml tools/x11-control/Cargo.lock tools/x11-control/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/x11-control/Cargo.toml --target-dir build/rust-x11-control
	install -m 755 build/rust-x11-control/release/steamarm-x11-control $@

image-volume: build/steamarm-image-volume
build/steamarm-image-volume: tools/image-volume/Cargo.toml tools/image-volume/Cargo.lock tools/image-volume/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/image-volume/Cargo.toml --target-dir build/rust-image-volume
	install -m 755 build/rust-image-volume/release/steamarm-image-volume $@

env-links: build/steamarm-env-links
build/steamarm-env-links: tools/env-links/Cargo.toml tools/env-links/Cargo.lock tools/env-links/src/main.rs
	@if [ -n "$${STEAMARM_ENV_LINKS_BIN:-}" ] && [ -x "$$STEAMARM_ENV_LINKS_BIN" ]; then \
	    install -m 755 "$$STEAMARM_ENV_LINKS_BIN" $@; \
	else \
	    cargo build --offline --locked --release --manifest-path tools/env-links/Cargo.toml --target-dir build/rust-env-links; \
	    install -m 755 build/rust-env-links/release/steamarm-env-links $@; \
	fi

steamroot-build: build/steamarm-steamroot-build
build/steamarm-steamroot-build: tools/steamroot-build/Cargo.toml tools/steamroot-build/Cargo.lock tools/steamroot-build/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/steamroot-build/Cargo.toml --target-dir build/rust-steamroot-build
	install -m 755 build/rust-steamroot-build/release/steamarm-steamroot-build $@

frame-root-utils: build/steamarm-frame-root-vulkan build/steamarm-frame-root-atomupd build/steamarm-frame-root-build
build/steamarm-frame-root-vulkan: tools/frame-root-utils/Cargo.toml tools/frame-root-utils/Cargo.lock tools/frame-root-utils/src/main.rs
	cargo build --offline --locked --release --bin steamarm-frame-root-vulkan --manifest-path tools/frame-root-utils/Cargo.toml --target-dir build/rust-frame-root-utils
	install -m 755 build/rust-frame-root-utils/release/steamarm-frame-root-vulkan $@
build/steamarm-frame-root-atomupd: tools/frame-root-utils/Cargo.toml tools/frame-root-utils/Cargo.lock tools/frame-root-utils/src/atomupd.rs
	cargo build --offline --locked --release --bin steamarm-frame-root-atomupd --manifest-path tools/frame-root-utils/Cargo.toml --target-dir build/rust-frame-root-utils
	install -m 755 build/rust-frame-root-utils/release/steamarm-frame-root-atomupd $@

build/steamarm-frame-root-build: tools/frame-root-utils/Cargo.toml tools/frame-root-utils/Cargo.lock tools/frame-root-utils/src/frameroot.rs
	cargo build --offline --locked --release --bin steamarm-frame-root-build --manifest-path tools/frame-root-utils/Cargo.toml --target-dir build/rust-frame-root-utils
	install -m 755 build/rust-frame-root-utils/release/steamarm-frame-root-build $@

steamframe-image: build/steamarm-frame-image
build/steamarm-frame-image: tools/steamframe-image/Cargo.toml tools/steamframe-image/Cargo.lock tools/steamframe-image/src/main.rs tools/steamframe-image/src/extractor.rs tools/steamframe-image/src/diagnostics.rs tools/steamframe-image/src/inventory.rs tools/steamframe-image/src/compare.rs
	cargo build --offline --locked --release --manifest-path tools/steamframe-image/Cargo.toml --target-dir build/rust-steamframe-image
	install -m 755 build/rust-steamframe-image/release/steamarm-frame-image $@

heroic-asar: build/steamarm-heroic-asar
build/steamarm-heroic-asar: tools/heroic-asar/Cargo.toml tools/heroic-asar/Cargo.lock tools/heroic-asar/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/heroic-asar/Cargo.toml --target-dir build/rust-heroic-asar
	install -m 755 build/rust-heroic-asar/release/steamarm-heroic-asar $@

androidroot-tools: build/steamarm-androidroot-tools
build/steamarm-androidroot-tools: tools/androidroot/Cargo.toml tools/androidroot/Cargo.lock tools/androidroot/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/androidroot/Cargo.toml --target-dir build/rust-androidroot
	install -m 755 build/rust-androidroot/release/steamarm-androidroot-tools $@

arch-audit: build/steamarm-arch-audit
build/steamarm-arch-audit: tools/arch-audit/Cargo.toml tools/arch-audit/Cargo.lock tools/arch-audit/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/arch-audit/Cargo.toml --target-dir build/rust-arch-audit
	install -m 755 build/rust-arch-audit/release/steamarm-arch-audit $@

wine-builtin-mark: build/steamarm-wine-builtin-mark
build/steamarm-wine-builtin-mark: tools/wine-builtin-mark/Cargo.toml tools/wine-builtin-mark/Cargo.lock tools/wine-builtin-mark/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/wine-builtin-mark/Cargo.toml --target-dir build/rust-wine-builtin-mark
	install -m 755 build/rust-wine-builtin-mark/release/steamarm-wine-builtin-mark $@

safeguard: build/steamarm-safeguard
build/steamarm-safeguard: tools/safeguard/Cargo.toml tools/safeguard/Cargo.lock tools/safeguard/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/safeguard/Cargo.toml --target-dir build/rust-safeguard
	install -m 755 build/rust-safeguard/release/steamarm-safeguard $@

android-pm: build/steamarm-android-pm
build/steamarm-android-pm: tools/android-pm/Cargo.toml tools/android-pm/Cargo.lock tools/android-pm/src/main.rs
	cargo build --offline --locked --release --manifest-path tools/android-pm/Cargo.toml --target-dir build/rust-android-pm
	install -m 755 build/rust-android-pm/release/steamarm-android-pm $@

launcher: $(LAUNCHER_BIN) launcher/Info.plist.in release-check frame-client-install setup-runner settings-env app-plan app-runner compat-status wine-prefix-options session proton-command fex-host-build fex-thunks-build steam-install steamroot-gfx-install vnc-control steam-launcher steam-arm64-links native-runner steam-arm64-runner fex-proton-tool fex-proton-install ffx-metalfx-install audio x11-control image-volume env-links steamroot-build frame-root-utils steamframe-image heroic-asar androidroot-tools android-pm arch-audit fex-runner inputd safeguard build/x11spawn
	@rm -rf $(LAUNCHER_APP)
	@mkdir -p $(LAUNCHER_APP)/Contents/MacOS $(LAUNCHER_APP)/Contents/Resources \
	    $(LAUNCHER_APP)/Contents/Frameworks
	@cp $(LAUNCHER_BIN) $(LAUNCHER_APP)/Contents/MacOS/SteamARM
	@sed 's|@PROJECT_DIR@|$(CURDIR)|' launcher/Info.plist.in > $(LAUNCHER_APP)/Contents/Info.plist
	@cp resources/AppIcon.icns $(LAUNCHER_APP)/Contents/Resources/AppIcon.icns
	@if [ -x build/rust-release-check/release/steamarm-release-check ]; then \
	    install -m 755 build/rust-release-check/release/steamarm-release-check \
	        $(LAUNCHER_APP)/Contents/Resources/steamarm-release-check; \
	fi
	@if [ -x build/rust-frame-client-install/release/steamarm-frame-client-install ]; then \
	    install -m 755 build/rust-frame-client-install/release/steamarm-frame-client-install \
	        $(LAUNCHER_APP)/Contents/Resources/steamarm-frame-client-install; \
	fi
	@if [ -x build/rust-setup/release/steamarm-setup ]; then \
	    install -m 755 build/rust-setup/release/steamarm-setup \
	        $(LAUNCHER_APP)/Contents/Resources/steamarm-setup; \
	fi
	@install -m 755 build/steamarm-settings-env \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-settings-env
	@install -m 755 build/steamarm-app-plan \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-app-plan
	@install -m 755 build/steamarm-app-runner \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-app-runner
	@install -m 755 build/steamarm-frame-image \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-frame-image
	@install -m 755 build/steamarm-heroic-asar \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-heroic-asar
	@install -m 755 build/steamarm-androidroot-tools \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-androidroot-tools
	@install -m 755 build/steamarm-android-pm \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-android-pm
	@install -m 755 build/x11spawn \
	    $(LAUNCHER_APP)/Contents/Resources/x11spawn
	@install -m 755 build/steamarm-vnc-control \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-vnc-control
	@install -m 755 build/steamarm-steam-arm64-links \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-steam-arm64-links
	@install -m 755 build/steamarm-run-native \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-run-native
	@install -m 755 build/steamarm-run-steam-arm64 \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-run-steam-arm64
	@install -m 755 build/steamarm-fex-proton-install \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-fex-proton-install
	@install -m 755 build/steamarm-fexserver-manager-arm64 \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-fexserver-manager-arm64
	@install -m 755 build/steamarm-fex-arm64 \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-fex-arm64
	@install -m 755 build/steamarm-fex-proton-arm64 \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-fex-proton-arm64
	@mkdir -p $(LAUNCHER_APP)/Contents/Resources/steamarm-fex-linux \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-fex-proton \
	    $(LAUNCHER_APP)/Contents/Resources/ffx-metalfx
	@install -m 644 tools/steamarm-fex-linux/toolmanifest.vdf \
	    tools/steamarm-fex-linux/compatibilitytool.vdf \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-fex-linux/
	@install -m 644 tools/steamarm-fex-proton/toolmanifest.vdf \
	    tools/steamarm-fex-proton/compatibilitytool.vdf \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-fex-proton/
	@install -m 644 tools/ffx-metalfx/anticheat-appids.txt \
	    $(LAUNCHER_APP)/Contents/Resources/ffx-metalfx/anticheat-appids.txt
	@install -m 755 build/steamarm-ffx-x86_64 \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-ffx-x86_64
	@install -m 755 build/steamarm-ffx-metalfx-install \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-ffx-metalfx-install
	@mkdir -p $(LAUNCHER_APP)/Contents/Resources/ffx-metalfx/x86_64-windows
	@install -m 644 $(FFX_DLLS) \
	    $(LAUNCHER_APP)/Contents/Resources/ffx-metalfx/x86_64-windows/
	@install -m 644 tools/ffx-metalfx/anticheat-appids.txt \
	    $(LAUNCHER_APP)/Contents/Resources/ffx-metalfx/anticheat-appids.txt
	@install -m 755 build/steamarm-compat-status \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-compat-status
	@install -m 755 build/steamarm-wine-prefix-options \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-wine-prefix-options
	@install -m 755 build/steamarm-session \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-session
	@install -m 755 build/steamarm-proton-command \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-proton-command
	@install -m 755 build/steamarm-steam-launcher \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-steam-launcher
	@install -m 755 build/steamarm-fex-host-build \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-fex-host-build
	@install -m 755 build/steamarm-fex-thunks-build \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-fex-thunks-build
	@install -m 755 build/steamarm-steam-install \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-steam-install
	@install -m 755 build/steamarm-steamroot-gfx-install \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-steamroot-gfx-install
	@install -m 755 build/steamarm-audio \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-audio
	@install -m 755 build/steamarm-x11-control \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-x11-control
	@install -m 755 build/steamarm-image-volume \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-image-volume
	@install -m 755 build/steamarm-env-links \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-env-links
	@install -m 755 build/steamarm-steamroot-build \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-steamroot-build
	@install -m 755 build/steamarm-frame-root-vulkan \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-frame-root-vulkan
	@install -m 755 build/steamarm-frame-root-atomupd \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-frame-root-atomupd
	@install -m 755 build/steamarm-frame-root-build \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-frame-root-build
	@install -m 755 build/steamarm-arch-audit \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-arch-audit
	@install -m 755 build/steamarm-fex-runner \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-fex-runner
	@install -m 755 build/steamarm-inputd \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-inputd
	@install -m 755 build/steamarm-inputctl \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-inputctl
	@install -m 755 build/steamarm-safeguard \
	    $(LAUNCHER_APP)/Contents/Resources/steamarm-safeguard
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
