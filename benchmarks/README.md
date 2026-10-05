# benchmarks

What was measured at each stage of taking SteamARM from a Linux VM to
ZERO-VM, including what failed. Each `stage*.txt` file is the record of its
stage and is not rewritten afterwards; later corrections go in a later file
or are marked inside the old one (for example `stage2-tls.txt`).

Host for every Mac measurement unless a file says otherwise: Apple M4,
16 GB, macOS 27. Files marked **VM-era** were measured inside, or against,
the Fedora VM that was deleted on 2026-09-27; they are history, and
`docs/PERFORMANCE_BASELINE.md` quotes only the ZERO-VM numbers.

## Stage index

| stage | file(s) | date | what it established |
|---|---|---|---|
| 0 | (none left) | 2026-09-23 | frame-time capture for the VM app; its tools were deleted with the VM (see below) |
| 1 | `stage1-syscall-cost.txt` | 09-23 | Darwin ignores the `svc` immediate and runs the call in x16, so Linux `svc` cannot be trapped; a rewritten `svc` costs about as much as a native one, a trap 40-90× more |
| 1 | `stage1-syscall-mix.txt` | 09-23 | **VM-era**: about 4,300 syscalls/s from Steam idle at the library, measured with ftrace in the guest |
| 2 | `stage2-dynamic.txt` | 09-23 | ld.so maps libraries itself, so their `svc` sites must be rewritten at mmap time |
| 2 | `stage2-elf-alignment.txt`, `stage2-elf-survey.txt` | 09-23 | Fedora aarch64 images use `p_align` 0x10000 and are PIE; the PIE-only rule costs little |
| 2 | `stage2-pagezero.txt` | 09-23 | the low 4 GiB (`__PAGEZERO`) cannot be used or reclaimed, so `ET_EXEC` images cannot load |
| 2 | `stage2-rewrite-validation.txt` | 09-23 | a linear scan for `svc #0` matches the disassembler on real libraries |
| 2 | `stage2-tls.txt` | 09-23 | superseded by `stage3-tls.txt` (its conclusion was wrong) |
| 3 | `stage3-tls.txt` | 09-23 | Darwin clobbers `TPIDR_EL0` on context switch: guest TLS moves to a TSD slot |
| 3 | `stage3-threads.txt`, `stage3-signals.txt` | 09-23/24 | glibc threads, futexes and signals work; errno numbers differ between Darwin and Linux |
| 4 | `stage4-vulkan.txt`, `stage4-shim.txt`, `stage4-vulkaninfo.txt`, `stage4-triangle.txt`, `stage4-present.txt` | 09-24 | Vulkan from a Linux ELF to MoltenVK through the shim; unmodified `vulkaninfo`; a drawn triangle; presentation with no copy |
| 5 | `stage5-fex.txt`, `stage5-process.txt`, `stage5-selfread-deadlock.txt`, `stage5-subpage.txt`, `stage5-va.txt`, `stage5-sysreg.txt`, `stage5-idregs.txt`, `stage5-jit.txt`, `stage5-x18.txt` | 09-24/25 | FEX runs under lxrun: a process model, 4 KiB guest pages on 16 KiB, the address space, trapped system registers, W^X flips requested by the guest, and x18 zeroed by Darwin on every exception |
| 5 | `stage5-x86-throughput.txt` | 09-25 | x86-64 through FEX under lxrun runs at 1.0× native aarch64 on `x86_bench.c`; also has **VM-era** rows |
| 6 | `stage6-steam-gap.txt`, `stage6-bwrap-plan.txt`, `stage6-vm-lockups.txt` | 09-23..25 | **VM-era**: what Steam needs (86 distinct syscalls), the bwrap plan pressure-vessel generates, and two guest-kernel lockups |
| 7 | `stage7-guest-base.txt` | 09-25 | a guest address base for 32-bit guests, since the low 4 GiB is unavailable |
| 8 | `stage8-steam-zero-vm.txt` | 09-26 | the x86 Steam client runs with no VM: pressure-vessel, the webhelper, x86 memory ordering kept in software |
| 9 | `stage9-vmfree-build.txt` | 09-26 | FEX builds on the Mac from Fedora RPMs, with no VM |
| 10 | `stage10-vulkan-thunks.txt` | 09-26 | x86-64 Vulkan reaches MoltenVK through FEX's thunks |
| 11 | `stage11-native-x11.txt` | 09-26 | SteamARM's own XQuartz build, rootless on `:2`: X windows are macOS windows |
| 12 | `stage12-native-present.txt` | 09-27 | Vulkan frames hosted over X windows across processes (CALayerHost), no copy; 165 Hz pacing |
| 13 | `stage13-wine-and-host-safety.txt` | 09-27 | Proton's Wine runs `cmd.exe`; two Mac hangs and the memory guard that followed |
| 14 | `stage14-d3d11-d3d12.txt` | 09-27 | D3D11 (DXVK) and D3D12 (VKD3D-Proton) probes at ~162 fps |
| 15 | `stage15-steam-proton-path.txt` | 09-27 | Windows programs launched the way Steam launches them (pressure-vessel, SLR 4) |
| 16 | `stage16-32bit-vulkan.txt` | 09-27 | 32-bit D3D9/11/12 through the i386 Vulkan thunk |
| 17 | `stage17-clean-install.txt` | 09-27 | a clean install up to Steam's sign-in window, with no VM |
| 18 | `stage18-settings-audio-controllers.txt` | 09-27 | settings, sound in Steam's container, controllers; Proton ARM64 measured not viable on macOS |
| 19 | `stage19-steamframe-base-and-arm64-limits.txt` | 09-28 | reading the Steam Frame image without btrfs-progs; the macOS limits on ARM64 Proton (written without a Mac; labelled) |
| 20 | `stage20-ci-macos-runner.txt` | 09-29 | first build and tests on a GitHub macOS runner (a virtual M1, not the M4); x18 preserved for a pre-13 SDK binary there |
| 21 | `stage21-native-arm64-client.txt` | 09-27..29 | Valve's native arm64 client under lxrun: starts, self-updates, loads its UI libraries; no window in that stage (it aborted with `free(): invalid pointer`) |
| 22 | `stage22-native-arm64-bringup.txt` | 09-29 | the native arm64 client from that abort to its "Sign in to Steam" window, with `--jitless`: X locale data and locales in the Fedora root (the abort's mechanism confirmed by probe), `lsof`, libcef's `mrs x18, nzcv` and `ldar w18` rewritten instead of poisoned (and measured trapping before), mmap at 4 KiB file offsets; the Steam Frame root still failed at GLX |
| 22 | `stage22-kosmickrisp.txt` | 09-29 | KosmicKrisp as a second Vulkan driver through the shim (`STEAMARM_VK_ICD`, ICD mode) for aarch64, x86-64 and i386 guests; D3D11/D3D12 probes on it once the shim reports `fillModeNonSolid`; present modes reported and overridable, IMMEDIATE measured |
| 23 | `stage23-native-arm64-jit.txt` | 09-29 | V8's JIT in the native arm64 client: RWX pages split W^X per page and scanned before they execute, the host SIGSEGV/SIGBUS handler kept under a guest SIG_DFL; the login window with the JIT on, also from the launcher |
| 23 | `stage23-runtime-fixes.txt` | 09-29 | the W/X livelock of 4 KiB sub-pages (stores from a split page into itself are emulated) and a relative `/proc/self/exe`; `tests/elf/run.sh` 55/0 with 2 xfail → 63/0 with 0 xfail |
| 23 | `stage23-frame-root.txt` | 09-29 | the native arm64 client on the Steam Frame root (`scripts/mkframeroot.sh`): indirect GLX through the X server, a resolv.conf, the client's own steamdeck_stable branch; the login window 5 of 5 runs, 5-7 s later than on the Fedora root |
| 24 | `stage24-holo-vs-frame.txt` | 09-29 | `holo-core-aarch64-preview` (`mash-20251118.3`) against the Frame 0.3.0 root, package by package, with no guest: 81 of 887 shared names at the same version (Holo newer for 764), so the Frame is not built from it; Holo has no GTK 2, which the client's `steamui.so` needs; `steamframe-image.py compare` gains pacman's vercmp and PROVIDES/REPLACES counterparts |
| 24 | `stage24-fex-game-boundary.txt` | 09-29 | the native arm64 client's compatibility tool for x86 Proton through FEX (`docs/FEX_GAME_BOUNDARY.md`): D3D11/D3D12 probes at ~160 fps through x86 Proton Experimental and SLR 4, as on the x86 client's path, invoked the way the client invokes a tool, on both ARM64 roots; both clients register it before a sign-in; 10 of 29 launches did not exit (Xalia; 1 of 10 on the x86 path); Proton 10.0 with sniper does not start under FEX on either path |
| 24 | `stage24-heroic.txt` | 09-29 | Heroic Games Launcher as a native linux-arm64 program (Electron 43, assembled from official release files, identical to an arm64 build of its source tag) in the Fedora ARM64 root: window, Settings, clean close and reopen in 7 of 7 cycles; six runtime fixes (prlimit EFAULT, partial-page mprotect, deferred SysV IPC_RMID, execve ENOENT, epoll dup, SOCK_SEQPACKET end of file); V8's TurboFan crashes under lxrun (off with `--no-opt`, cause unknown); Amazon's nile is non-PIE |
| 24 | `stage24-minecraft-prism.txt` | 09-29 | Prism Launcher's aarch64 build and a Temurin aarch64 JRE natively under lxrun (window 1 s, Java detected, clean close/reopen); Linux HotSpot's C1/C2 and llvmpipe use x18, which macOS zeroes: wrong results with the default lxrun, correct with the opt-in `LXRT_KEEP_X18=1` build (SDK 12.3 link: the kernel keeps x18 on this M4, macOS 27); no GL 3.2 core path to an X window; runtime fixes for inotify FIONREAD and eventfd across fork. The Prism launcher integration was not merged (Minecraft set aside by the owner) |
| 25 | `stage25-android-research.txt` | 09-29 | research for Android with no VM (`docs/ANDROID_ZERO_VM_FEASIBILITY.md`, `docs/PLAY_STORE_RESEARCH.md`, `docs/LEPTON_REUSE_ANALYSIS.md` §0); no Android code was run: Valve's Lepton GitLab reachable from the Mac and the GitHub mirror `6135b53` confirmed as its ancestor; Darwin refuses `PROT_EXEC` on an unsigned file mapping but runs code through a `mach_vm_remap` read-execute alias of read-write memory (ART's dual-view JIT shape, `dual_view_jit.c`); Waydroid's GAPPS/VANILLA channel metadata; the uncertified-device page needs a Google sign-in (not done) |
| 25 | `stage25-apk-install.txt` | 09-29 | Android APKs read, installed, updated and uninstalled with no runtime (`docs/APK_SUPPORT.md`): `scripts/apk-inspect.py` agrees with F-Droid's `index-v2.json` on 9 fields for 10 real APKs (signer SHA-256 included); `scripts/android-pm.py` keeps data across an update and refuses a downgrade; the launcher shows Android cards that cannot be opened, with the reason; Termux's arm64 libraries are 4 KiB-aligned. Nothing Android ran |
| 25 | `stage25-binder.txt` | 09-29 | Android binder in userspace under lxrun, no VM: `/dev/binder`, `/dev/hwbinder`, `/dev/vndbinder` from a hub process (`runtime/binder_hub.c`, the Linux driver's semantics) and the guest side (`runtime/binder.c`); Android 11's servicemanager as context manager with `service list`/`check`/`call` and `dumpsys -l`, a native service registered from another process (int and fd calls, death notification), idmap2d and credstore on libbinder's thread pool, vndservicemanager; hwservicemanager stops at properties; 36-38 us per round trip (host socket floor 8-9 us) |
| 27 | `stage27-android-display.txt` | 09-29 | A display for Android with no VM: Weston (Fedora aarch64, `scripts/mkwestonroot.sh`) under lxrun with its X11 backend, a macOS window on :2; wl_shm clients at 57 frames/s, aarch64 and x86-64 under FEX (memfd buffers shared across an FEX guest and a native one, pixels checked with xwd); the x86_64 image's own SurfaceFlinger (FEX, SwiftShader GLES, gralloc default) presents the LineageOS boot animation through Waydroid's hwcomposer into it at 57 page flips/s. Runtime: mremap of MAP_SHARED file mappings (Weston's wl_shm pools), the syscall trampoline now keeps v0-v31/FPSR/NZCV as Linux does (18.8 -> 23.5 ns per getpid), `LXRT_BINDER_UID`; binder works for x86-64 guests. Next: input, a GPU path |
| 26 | `stage26-android-properties.txt` | 09-29 | Android system properties with no init and no VM: `lxrun --property-service` (`runtime/propsvc.c`) builds `/dev/__properties__` from the image's own property_contexts and .prop files as init does (property_info byte-identical to AOSP's serializer) and serves setprop; getprop lists the image's 224 properties, setprop/getprop across processes, `ro.*` once, types checked, `ctl.*` refused, `persist.*` kept; `__system_property_wait` in a program linked against the image's libc wakes on the service's cross-process ulock wake (18.6 us median); hwservicemanager sets `hwservicemanager.ready`, `lshal` lists it and three HIDL HALs register; no start-up cost. Next wall for native services: init |
| 25 | `stage25-android-userspace.txt` | 09-29 | Android 11 arm64 userspace (Waydroid LineageOS 18.1, `scripts/mkandroidroot.sh`) under lxrun with no VM: bionic's linker64, toybox, mksh, getprop and ART's dexdump run; seven runtime fixes (sub-page mremap for bionic's CFI shadow, tagged-address prctl refused, BoringSSL FIPS module's TPIDR_EL0 reads kept byte-for-byte, msync, rt_tgsigqueueinfo, clone TLS inheritance, no thread without CLONE_FILES); ART cannot start: its heap must be below 4 GiB, which macOS forbids (`docs/ANDROID_RUNTIME_ARCHITECTURE.md`) |
| 25 | `stage25-art-x86-fex.txt` | 09-29 | x86-64 Android (Waydroid's LineageOS 18.1 x86_64, `scripts/mkandroidroot.sh --arch x86_64`) under FEX with its 64-bit low window forced on: ART's heap and boot image map below 4 GiB of guest space and `dalvikvm64` runs Java, interpreter and JIT, with the Mac JVM's results (Hello 0.98 s; JIT'd loop 1.23x HotSpot C2, interpreter 3.4x); `dex2oat64` compiles and its AOT code runs. Six runtime fixes (getrlimit, startstack, RLIM_INFINITY, madvise in the window, big unix datagrams, getgroups) and three FEX patches: low mmap hints, SMC through a dual-mapped JIT cache (stale code after code cache collections), and a 16 KiB interrupt fault page (deferred signals were never delivered to any FEX guest). No arm64 native bridge in the x86 image |
| 27 | `stage27-android-framework.txt` | 09-29 | Android's services and framework in the x86_64 root, zero VM: binder and properties work for x86-64 guests through FEX unchanged (service managers, `service`, `dumpsys`, `lshal`, an x86-64 native service receiving a descriptor, `__system_property_wait`); `scripts/android-boot.py`, a minimal init that runs the image's .rc files for a headless profile of 23 services; Android ids (`runtime/android_ids.c`: per-process virtual credentials with Linux's rules); zygote64 preloads 12,100 classes and forks system_server (uid 1000, seccomp under FEX), which runs its bootstrap services up to LightsService and then waits for SurfaceFlinger, which waits for a composer (Waydroid's is a Wayland client; no display here): no boot_completed, no PackageManagerService. Five runtime fixes (AF_UNIX names, `/proc/self/fd` without runtime/FEX descriptors, `attr/current`, stale SO_PEERCRED senders, ctl.* to an init) and a FEX seccomp patch; i386 (audio HAL, dex2oat32) and netd are the next walls |
| 28 | `stage28-art-heap-sites.txt` | 09-30 | ART with its heap above 4 GiB (`docs/ART_HEAP_ABOVE_4GB.md`, `patches/art-heap-base/`): 232 source lines outside ART's central reference conversion in LineageOS 18.1 / AOSP android11 ART, the design (a fixed 4 GiB window at 0x4000000000, references kept 32-bit, `orr` of the base after each load), a 9-patch prototype series that applies with `git am` to both trees; nothing built or run (UNTESTED) |
| 28 | `stage28-keep-x18.txt` | 09-30 | lxrun linked as SDK 12.3 (the kernel keeps x18) passes the whole matrix as the current-SDK build does (tests/elf, i386, arm64, vk_device, Windows probes at 150-162 fps, Android, the native arm64 Steam client's sign-in window, Heroic) and is the default now (`LXRT_KEEP_X18=0` opts out); the x18 rewriter stays, since a forked child loses the kernel's x18 (with the pass off Steam and Heroic lose their forked GPU process); V8's TurboFan crash of stage 24 was the `br x18` trampoline clobbering a jump table's live x16: fixed, Heroic runs without `--no-opt`; guest signal handlers no longer overwrite a JIT's hardware x18 |
| 28 | `stage28-android-apk.txt` | 09-30 | An APK on the Mac's screen, zero VM: the x86_64 root's display profile reaches `sys.boot_completed=1` (122.6 s first boot, 17-27 s warm), `pm install` of an F-Droid APK (Success, ~6 s), `am start` (TotalTime ~2.5-2.9 s), its window focused and in Weston's X window, a macOS window (xwd); pointer motion from the X window reaches InputDispatcher. `scripts/android-session.py` (start/stop/launch/run); the launcher's Android cards open in it for dex-only and x86_64 apps (arm64-v8a-only stay disabled: the ART heap wall). Fixed on the way: pm install (sendfile/splice, the services' cwd), lmkd's 3 s stalls (a socket stand-in), AlarmManager's false wakeups (BOOTTIME timerfds), binder "Bad file descriptor" (descriptors in flight kept referenced), the safeguard's process limit during a session |
| 28 | `stage28-android-reliability.txt` | 09-30 | Android reliability, zero VM: the intermittent `sh -c 'x=$(toybox ...)'` hang of x86-64 Android under FEX had two causes -- XNU leaves a process-directed signal that every thread blocks on the host main thread (`runtime/signal.c` hands it to a guest thread) and FEX kept a SIGCHLD in PendingSignals that sigsuspend never looked at (`patches/fex-lxrt-sigsuspend-pending.patch`); 0 hangs in 720 runs with both, 3/240 on main, 1/240 and 6/240 with one fix each. i386 Android under FEX's 32-bit mode (`patches/fex-lxrt-i386-bionic.patch`, runtime socket/msync fixes, binder for 32-bit guests): dalvikvm32, dex2oat32, zygote_secondary, an i386 binder service, the 32-bit audio HAL (with `--linkerconfig`); still limited by 32-bit bionic's 16-bit tids (main-thread tid = the Mac's pid, refused above 65535; other threads' tids above 200000 break their own recursive mutexes: `tests/android/i386_rmutex.c`). `LXRT_INPUT_DIR`: a /dev/input per Android stack. The FEX build reproduces the previous session's binary bit for bit; the Steam/Proton route (i386 probes, Proton D3D probes within ~1 fps, Vulkan device) unchanged with it |
| 29 | `stage29-android-input-network.txt` | 09-30 | Clicks, keys and the network for the Android session, zero VM: XTEST on SteamARM's X server (`run-x11-native.sh`) and Linux keycodes from it (`patches/xquartz-evdev-keycodes.patch`: macOS code 0, the A key, had reached Android as the unmappable scan code 0); a click opens a game in Simple Solitaire, Escape is BACK, A is KEYCODE_A, with Android's own key layouts. netd's dnsproxyd served from the Mac (`scripts/android_dnsproxy.py`) and an Ethernet network agent in the netd stand-in (`NetworkAgentStandIn.java`): Android's default network, VALIDATED by the network stack's own probes; `curl` https in Android answers 204. The Mac's time zone (`persist.sys.timezone`). The boot now stops the root's FEXServer; the i386 service test is bounded (it spun at 99% CPU on a pid above 65535) |
| 30 | `stage30-android-media-windows.txt` | 09-30 | Compressed sound in Android: mediaserver and the OMX store (i386, possible since the 16-bit ids) started after the boot; the store with the image's generated linker configuration through LD_CONFIG_FILE and a no-op stand-in for its minijail setup (FEX's seccomp emulation of its 32-bit filter crashed); OMX codecs (`debug.stagefright.ccodec=0`: Codec2 crashed at 0x0): an .ogg decodes with OMX.google.vorbis.decoder, MediaPlayer plays to PulseAudio. One Mac window per Android app on steamarm-wlmac (Waydroid's multi-window mode; windows cut to the app's layers; attach(NULL) unmaps): 3 of 3 launches. The runtime ends a process whose SIGSEGV its handler never fixes (it spun at 100% CPU) |
| 31 | `stage31-runtime-waits-clipboard.txt` | 09-30 | Three runtime causes of x86 Steam starts without a window: a signal between Apple's JIT-permission write and its read-back (SIGTRAP in `pthread_jit_write_protect_np`: every flip now runs with signals blocked), a realtime signal's carrier that ran no handler still cutting sleeps and futex waits short with EINTR (the client's two-minute wait for the web helper ended in under a second: such calls now go on), and `read()` on a seqpacket pair never seeing a dead zygote (now end of file). `rt_sigpending`, `readv`, `preadv`, `pwritev`, `preadv2`, `pwritev2` implemented (were ENOSYS). The clipboard between the Mac and Android, both ways, through steamarm-wlmac |
| 32 | `stage32-runtime-private-fds.txt` | 09-30 | Android's zygote aborted one boot in about ten ("Unsupported st_mode for FD 40: DIR"): getdents64's duplicate of a directory the guest reads took the lowest free number and showed in /proc/self/fd. The runtime's own descriptors now sit high, close-on-exec and hidden: 10 of 10 quick restarts clean |
| 33 | `stage33-android-webview.txt` | 09-30 | WebView works in Android apps: Chromium's renderer, forked by WebView's own zygote and sandboxed with seccomp, runs under FEX. Five walls: mount(tmpfs) for app data isolation (emulated), abstract socket names given back by getsockname, rt_tgsigqueueinfo's siginfo carried and no_new_privs remembered (FEX's seccomp SIGSYS), FEX running a trapped syscall anyway (patched), memfd seals lost across processes and EFAULT for 4 KiB read-only pages (Chromium's own checks) |
| 34 | `stage34-android-termux.txt` | 09-30 | Termux works: its bootstrap installs, its shell runs, `apt update` and `apt install` work. Walls: shared storage (StorageManagerService's canonical-path check), exec with a guest environment and a guest TMPDIR, the runtime's chatter in captured stderr, FEX reading every exec's program from fd -100 under an app's seccomp filter (patched), pseudo-terminals (Darwin's master termios before the slave opens), and the DNS proxy reading bionic's AI_ADDRCONFIG as glibc's AI_NUMERICSERV |
| 35 | `stage35-android-apps-windows.txt` | 10-01 | Six F-Droid apps with x86_64 native code (VLC, Organic Maps, NewPipe, KeePassDX, Wikipedia, Element) install and open; Android's MediaPlayer plays H.264/AAC in real time (VLC's own player pauses itself, not explained). One launch in five had no Mac window: Waydroid's composer races its own wayland thread in wl_display_roundtrip and hangs SurfaceFlinger (and system_server's watchdog); steamarm-wlmac now pings after every sync: 12 of 12. traced started (Traceur crashed every boot) |
| 36 | `stage36-android-files.txt` | 10-01 | Apps could not delete directories (Darwin answers unlink of one with EPERM; remove() and Java's File.delete() need EISDIR); files an app made stated as the Mac user's, now the caller's (git works in Termux); a last low, transient directory descriptor (nftw on thread exit) removed from under the zygote's descriptor check |
| 37 | `stage37-sync-fsync-esync.txt` | 10-01 | fsync works in lxrun: `futex_waitv` on a board of shared Darwin ulocks (probe, EAGAIN, timeouts, waits across processes on a region mapped at different addresses, 1.3-1.5 us round trips; also under FEX). esync: an eventfd sent over SCM_RIGHTS, as wineserver sends them, is now the same eventfd in the receiver (it was a bare pipe). Both "Experimental" until the game round; Lightning JIT and Apple Hypervisor no longer listed in Settings |
| 38 | `stage38-opengl-zink.txt` | 10-01 | OpenGL on the GPU with no GL thunk: Mesa's Zink in the guest over the Vulkan thunk to MoltenVK (OpenGL 3.2, x86-64 and i386, pixels verified). The native X server's GLX fbconfigs could not pair with Mesa's, so every GL client was indirect (Apple GL 2.1): patched, direct GLX works (glxgears ~3000 fps). MoltenVK refused a sampler named `sampler`: the shim renames it. WineD3D D3D9/D3D11 probes render on Zink; "OpenGL (WineD3D)" becomes Experimental when detected. KosmicKrisp 26.2.3 cannot compile Zink's shaders |
| 39 | `stage39-wined3d-gl45.txt` | 10-01 | WineD3D on Zink stopped making its context current at OpenGL 3.2 after an X server restart (cause not explained); Zink on MoltenVK lacks only two format extensions for 3.3 and 4.0, and announcing them gives OpenGL 4.5: WineD3D D3D9/D3D11 (11_0) and the 32-bit probe pass again, and OpenGL 4.5 programs draw correctly. A process that loaded libvulkan.so.1 a second time crashed in FEX's thunk links (Steam's i386 client with Zink): the guest thunks are now never unloaded |
| 40 | `stage40-android-audio-timestamps.txt` | 10-01 | Waydroid's audio HAL never reported a presentation position (AudioFlinger timestamp err=440, AudioTrack.getTimestamp() empty for every app); a wrapper HAL for audioserver adds it (n=433, err=0). VLC still pauses itself: not the cause |
| 41 | `stage41-frame-root-vulkan.txt` | 10-01 | The native client on the Steam Frame root gets a Vulkan device: the shim installed as an ICD for the image's own loader (freedreno set aside), vulkaninfo and the client's steamsysinfo report Apple M4; sign-in window 10 of 12 with it, 4 of 4 without |
| 42 | `stage42-settings-scale-msaa.txt` | 10-01 | Settings: MSAA 2x/4x/8x for Direct3D 9 games through DXVK (measured on MoltenVK); per-entry settings for the built-in Steams; resolution scale/FSR stays unavailable (no Proton has its upscaler; a smaller reported surface does not survive Wine) |
| 43 | `stage43-settings-presets.txt` | 10-01 | Resolution scale through Wine's display-mode emulation (EmulateModeset): a 1280x720 program stretched over a 1600x900 screen; Lightning JIT not added (FEX speed settings: no measurable gain), MSync impossible under Linux Proton (fsync already waits on Darwin ulocks), Apple Hypervisor against Zero-VM |
| 44 | `stage44-scaling-frame-disk.txt` | 10-01 | Scaling filters of the Vulkan shim (FSR 1 with sharpness, MetalFX spatial, nearest) over a 1280x720 game on a 1920x1050 window; the Steam Frame and Android disk images grown to the Mac's disk, free space capped by it; SteamOS's update client answered in the Steam Frame root |
| 45 | `stage45-ntsync.txt` | 10-01 | /dev/ntsync emulated across processes: the kernel's selftests 12/12, tests/elf/ntsync.c 17/17 (aarch64, x86_64 under FEX), Proton Experimental "ntsync: up and running"; event round trip 20.9 us (fsync 14.4, wineserver 50.5) |
| 46 | `stage46-steam-ui-gpu.txt` | 10-01 | The ARM64 clients' web helper: GPU process dead 3 times then software; with ANGLE/Vulkan switches (LXRT_EXEC_ARGS) it stays up; offered off by default. FEX settings reach games started by an ARM64 client; FEXServer started with it |
| 47 | `stage47-kosmickrisp.txt` | 10-01 | KosmicKrisp through the shim: Vulkan device 6/6, Direct3D 9/11/12 probes 150-158 fps; 32-bit D3D12 fails (mutable-descriptor struct not thunked); scaling made DXVK recreate swapchains (315 in 2.5 s) until the scaler served every driver |
| 48 | `stage48-native-arm64-games.txt` | 10-01 | The Frame client gives CS2 to Steam Linux Runtime 4 arm64; its pressure-vessel is non-PIE and cannot load, so the runtime stands in for pressure-vessel-wrap; Vulkan through the root's loader recursed into it until the shim was linked -Bsymbolic; an aarch64 Vulkan program runs through the runtime's entry point on KosmicKrisp |
| 49 | `stage49-frame-ui-smooth.txt` | 10-01 | The Frame client's window went from 1 fps to 157-160 fps (165 Hz display) on KosmicKrisp: GPU switches only for the browser process, Metal's shader compiler registered again in fork children (Chromium's zygote-forked GPU process), and XQuartz no longer tells clients its desktop is a 1 Hz display |
| 50 | `stage50-games-from-frame-client.txt` | 10-02 | No game started from the Steam Frame client: ~/.steam's links were missing, and the x86 Proton path had five regressions (exec of a host ELF, carried LXRT_* variables, the host's malloc at 0x140000000 and under the break, ENOMEM where Wine expects EEXIST, a cross-process signal nested in the carrier handler under FEX); D3D11/D3D12 probes through the tool at 153-158 fps after |
| 51 | `stage51-cs2-linux-native.txt` | 10-02 | Counter-Strike 2's native Linux build from the Steam Frame client (new tool `steamarm-fex-linux`): no frame (a futex wait of 0xffffffff ms timed out at once), then 29 GB of command memory, a query pool that could not be created and vertex shaders refused every frame in KosmicKrisp (patches 01-03), 9.5 fps minimized, and the GPU at 99 % for 50 fps because of ~200 render passes a frame (patch 04: passes on the same attachments share an encoder; a Metal pass costs 44-70 us whatever its size). Bot match on de_dust2: median 72-76 fps; main menu 80-82. What limits it now: presentation tied to half the refresh (82.5), ~135 passes a frame, the translated main thread. |
| 52 | `stage52-mailbox-present.txt` | 10-02 | The cross-process layer gives up one drawable per refresh with FIFO and IMMEDIATE alike (165 fps both), and `nextDrawable` blocks: a game just over one refresh per frame fell to half the rate (82.5). IMMEDIATE/MAILBOX swapchains are now the shim's (`shim/mailbox.c`): own images, a thread acquiring the driver's, frames dropped instead of waited for. `vk_x11_present` 0.31 ms a frame on KosmicKrisp (was 6.06); Counter-Strike 2 bot match median 75.5-83 fps, peaks 91-107, GPU at 99 %. |
| 53 | `stage53-fullscreen-layer-order.txt` | 10-02 | Counter-Strike 2 played by hand: the X window came in front of the game's picture on clicks and drags (white, and once for good, with the hidden layer at 1.5 fps): the overlay is now one window level above its frame while its window is on top. A real fullscreen: the X screen is the whole display (1920x1080), menu bar and Dock hidden under a layer that covers it. The mailbox's driver swapchain is FIFO with three images: every frame shown, in step with the display. Highest settings at 1080p: 32.5-34.5 fps |
| 54 | `stage54-drawable-retain.txt` | 10-02 | "The window freezes or goes black when I change the video settings": KosmicKrisp kept a retain on every drawable it presented or was given; after two swapchains with an image in hand the layer had none left (reproduced without the game, tests/elf/vk_x11_modes.c). Patch 07 releases them: eight remakes without a stall, (the 80 fps it reported was not comparable: stage 55) |
| 55 | `stage55-confine-alpha-metalfx.txt` | 10-03 | Mouse-look confined on macOS (the cursor stays in the window); cut-out foliage at MSAA (palm cards drawn opaque) and vertex-only pipelines in the cache key; exclusive fullscreen no longer captures the displays; MetalFX on KosmicKrisp (patch 10) with an automatic render scale: 46.5 fps against 29.5 native at the highest settings, like the game's own FSR quality (45); KosmicKrisp pipeline cache and command memory (footprint 8.9 -> 7.4 GB). Corrects stage 54's 80 fps |
| 56 | `stage56-ultra-quality-input.txt` | 10-05 | Bilinear MetalFX spatial silently skipped whole low-resolution inputs: patch 21 accepts both allocation layouts at 77%, with actual-encode logs and readbacks. Blend/scissor and native-resolution draws remain the original shader. Render regressions passed on both drivers; no measured +150% game FPS gain. |
| 57 | `stage57-depth-cache-and-frame-pacing.txt` | 10-05 | Optional bounded depth/stencil cache: 39.4% less recording time in a paired microbenchmark, pixels/FEX/Proton probes passed; default off. CS2 and native Metal traces still show hitches and no established stable 100 FPS. Current display link exposes fixed refresh rates only. |
| 58 | `stage58-adaptive-sync-and-drawable-wait.txt` | 10-05 | bounded notification wait tested on ARM64/FEX; actual display/native fullscreen eligibility and fixed-refresh fallback; <10 ms CS2 and physical VRR remain unverified |

## After stage 21 (no stage file)

- `dbd1657`: the rewriter's poison word is now `brk #1`. It was
  `0xD4000021`, which is `svc #1`, so a refused or unreachable site ran a
  Darwin system call instead of trapping. `tests/elf/run.sh` 37/37.
  `docs/X18_VIRTUALIZATION.md`.
- `e4047ef`: on memory pressure the memory guard stops guests only on
  critical pressure (2 checks) or under 12 % free (3 checks), largest guest
  first. The old rule (under 35 % free, once) closed Steam on a 16 GB Mac
  with 2.7 GB of guests. Its other limits (guests over the DRAM setting,
  fseventsd, process count, kernel VM objects and map entries) still stop
  every guest at once. `tests/launcher/safeguard.sh`, 7 checks.
- 2026-09-29: the Steam Frame image diagnosed, extracted and inventoried
  (`docs/STEAM_FRAME_INVENTORY.md`).
- Stage 21's "153 packages" for the Fedora armroot: `scripts/mkarmroot.lock`
  has 150 root and 5 build-only entries (MEASURED count).

## After stage 23 (no stage file)

The post-merge check of 2026-09-29, on the Mac, after the stage 23 branches
were merged (`c636fcc`, `c8bba0f`) and the fixes that followed (`5d9760a`,
`9468028`, `b3f64c8`). MEASURED; the numbers are the ones recorded in those
commit messages and by the check, the launcher logs are
`~/SteamARM-roots/logs/steam-20260929-10*.log`,
`steam-arm64-20260929-10*.log` and `steam-arm64-frame-20260929-100701.log`.

- Tests: `tests/elf/run.sh` 64 passed, 0 failed, 0 expected failures;
  `tests/elf/run_i386.sh` 18 passed, 0 failed (plus its existing
  `smc_subpage` expected failure); `tests/win/run.sh` 11 passed, 0 failed,
  158.7-161.9 fps; `tests/win/run_steam_path.sh` 2/0; `make test-arm64`
  4/0; `tests/elf/run_vk_device.sh` 2/0; `make test-launcher-core` passes
  (`5d9760a`).
- Native arm64 client from the launcher (`scripts/run-app.sh`, V8's JIT
  on): "Steam ARM64 (experimental)" showed the sign-in window at 15-16 s
  (Fedora armroot), "Steam ARM64 · Steam Frame (experimental)" at 21 s
  (Frame root, its first recorded start through `run-app.sh`); **Detener**
  left 0 guest processes. 0 FEX lines in the three logs; every image load
  is an aarch64 PIE (35, 35 and 60). Sign-in not attempted.
- x86 Steam client from the launcher: the main window in 88-93 s in 4 of 5
  starts. The fifth died at start: "SIGTRAP at pc 0x19df7ec58 = outside
  the guest image", which `atos` names `pthread_jit_write_protect_np`+388
  in libsystem_pthread (`steam-20260929-100114.log`, status 133). Cause
  UNKNOWN; since `b3f64c8` such a report names the host pc, lr and up to
  7 callers. Every image load in the x86 logs of the check is `FEX-gb`
  (35 of 35 in each log from `steam-20260929-100948.log` on; 2 of 2 in the
  one that died).

## Before 0.3.5 (no stage file)

After the review fixes (`154bb33`) and the stale-fault recheck (`913fbad`),
on the Mac, 2026-09-29. MEASURED:

- Tests: `tests/elf/run.sh` 68 passed, 0 failed, 0 expected failures
  (`WX_MPROTECT_RACE` stress: 0 and 0 faults, against 52 and 184 before
  `913fbad`); `tests/elf/run_i386.sh` 18/0 (plus `smc_subpage`'s expected
  failure); `tests/win/run.sh` 11/0, 159.2-161.7 fps;
  `tests/win/run_steam_path.sh` 2/0; `make test-arm64` 4/0;
  `tests/elf/run_vk_device.sh` 2/0; `tests/arm64/frame_glx.sh` 1/0;
  `tests/steamframe_image/redact.sh` 5/0; `make test-launcher-core` passes.
- From the launcher: "Steam ARM64 (experimental)" sign-in window at 16 s,
  "Steam ARM64 · Steam Frame (experimental)" at 20 s, both still mapped 20 s
  later, and **Detener** left 0 guest processes.
- x86 Steam client from the launcher, same session: with this runtime the
  main window came in 84-118 s in 5 of 6 starts; the sixth (the first
  start after the Windows probes) had no window in 300 s, after the web
  helper's usual GPU-process restarts ("Triggering shutdown due to GPU
  process restarts", also 27 times on 2026-09-27 with the old runtime) and
  no fault report. With the runtime from before this work (`e4047ef`),
  interleaved: 3 of 3 starts, main window in 124-199 s. Together with the
  earlier check: 9 of 11 x86 starts reached the window on the new runtime.
  Too few runs to call a difference in reliability either way.

## Tools in this directory

**Stage 1, syscall cost:**

```
clang -O2 -arch arm64 -o build/syscall_cost benchmarks/syscall_cost.c
./build/syscall_cost
```

`stage1-syscall-cost.txt` has the raw output of one run (0.8 ns function
call, 71.5 ns `svc #0x80`, 69.3 ns rewritten `svc`, 0.9 ns answered in user
space, 2789 ns signal trap, 6111 ns Mach exception trap). An earlier run gave
68.1, 114.2, 2794.2 and 6317.0 ns for the rewritten, native, signal and Mach
rows. Two findings decided the design:

1. **`svc` cannot be trapped.** `svc #0` with `x16 = 20` returned the pid:
   Darwin dispatches on x16 and ignores the immediate. A Linux binary's `svc`
   silently runs whatever Darwin call x16 holds. So every `svc` must be
   rewritten at load, and FEX's JIT must call the runtime instead of emitting
   `svc`.
2. **Traps are possible but slow.** An out-of-range number in x16 raises a
   catchable SIGTRAP; at 2.8-6.3 µs per call it is not a usable main path.
   It is why a poisoned site must be a real `brk`, not an `svc`
   (`dbd1657`).

**Stage 5, x86 throughput:** `scripts/run-fex.sh [--trace] <program> [args]`
runs an x86-64 Linux program through FEX under lxrun. `x86_bench.c` is the
freestanding benchmark behind `stage5-x86-throughput.txt`; it builds as
x86-64 and as aarch64 from the same source.

**Stage 5, a W^X flip inside a signal handler:**

```
clang -O1 -o build/wx_in_handler benchmarks/wx_in_handler.c
codesign -s - --entitlements resources/lxrt.entitlements build/wx_in_handler
./build/wx_in_handler
```

Result (`stage5-jit.txt`): a flip made inside a handler holds for the
handler's own stores, but not past its return. So the guest asks for each
flip itself (private syscall `0x4C580020`; `tests/elf/jit_wx.c`).

**Stage 25, a dual-view JIT code cache on Darwin:**

```
clang -O1 -Wall -o build/dual_view_jit benchmarks/dual_view_jit.c
./build/dual_view_jit
codesign -f -s - --entitlements resources/lxrt.entitlements build/dual_view_jit
./build/dual_view_jit
```

Result (`stage25-android-research.txt`): an unlinked temporary file (what
lxrun's memfd is) cannot be mapped executable, shared or private
(`EPERM`); anonymous memory aliased with `mach_vm_remap` can be
read-execute in one view and read-write in the other, and code written
through the second runs through the first. The shape Android's ART JIT
needs (`docs/ANDROID_ZERO_VM_FEASIBILITY.md` §3.2).

**Deleted with the VM (2026-09-27):** the VM app's frame-time trace
(`STEAMARM_TRACE`), `frametimes.py`, `capture.sh`, the `runs/vm-baseline-*`
captures, the libkrun VM-exit counters and `resources/SteamARM.entitlements`.
The VM-era results they produced are in `docs/history/PERFORMANCE_BASELINE.md`.

## Method

Fix resolution, settings, FPS cap, V-Sync, display, game version, scene and
duration across every compared run. Report median, 1% low, 0.1% low and the
histogram, never the average alone: smoothness is a claim about the tail.
Compare ZERO-VM against CrossOver and against a native macOS build where one
exists; the VM baseline is history. No game has been measured this way yet
(`docs/PERFORMANCE_BASELINE.md`, "Not measured").
