# steamwebhelper (CEF) under lxrun

Steam's whole UI is Chromium: `steamwebhelper`, built on CEF (`libcef.so`).
The client waits for it and gives up with "Timed out waiting for webhelper
init" (2400 × 50 ms, `benchmarks/stage8-steam-zero-vm.txt:224-225`). This page
covers the x86 webhelper, the working route, and the arm64 one of Valve's
native client. The arm64 webhelper reaches BrowserReady and shows the
client's "Sign in to Steam" window since stage 22 (with `--jitless`) and
stage 23 (with V8's JIT), on the Fedora armroot and on the Steam Frame root
(MEASURED, `benchmarks/stage22-native-arm64-bringup.txt`,
`benchmarks/stage23-native-arm64-jit.txt`,
`benchmarks/stage23-frame-root.txt`). Sign-in was not attempted, so nothing
past that window is known. The 2026-09-28/29 notes below are kept as the
history of how it got there, updated where later runs answered them.

Labels: MEASURED (a command and its result), VERIFIED IN SOURCE (file:line),
UPSTREAM DOCUMENTED, HYPOTHESIS, UNKNOWN. Logs are in `~/SteamARM-roots/logs/`
and in the client's `Steam/logs/`.

## x86-64 webhelper under FEX (the working route)

Each step below was a measured failure, fixed in the runtime or the launch
setup:

| stage | failure | fix |
|---|---|---|
| 5 | FEX re-executes itself with `execveat("/proc/self/exe")` | mapped to the loaded image; CEF then starts 3 processes (`stage5-fex.txt:250-270`) |
| 8 | `MADV_DONTNEED` sometimes left memory intact; threads jumped into freed FEX code | decided by the region's own pager (`dispatch.c`, `subpage.c`) |
| 8 | Chromium timestamps garbled (Darwin's 32-bit `tv_usec`) | fields copied one by one |
| 8 | Chromium's sandbox helper waited for `/proc/self/task/<tid>` to disappear | removed at thread exit |
| 8 | pressure-vessel took 142 s to reach the webhelper | one fd link refreshed per lookup: 27 s |
| 8 | V8: "Failed to reserve virtual memory for CodeRange" | guest mappings placed top-down like Linux; RWX outside MAP_JIT granted as RW (FEX never executes that memory) |
| 15 | hiding FEX's CPUID leaves broke pressure-vessel, and every zygote died | leaves hidden from the `steam` executable only (FEX `AppConfig/steam.json`) |
| 15 | the GPU process died after MoltenVK started Objective-C and the zygote forked | no Vulkan thunks for `steamwebhelper` (`AppConfig/steamwebhelper.json`); `OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES` on every exec |

Result: the Steam UI, store, library and login work; the login window
appears 85-86 s after start (MEASURED, stage 18 and stage 21). The x86 GPU
process renders in software (stage 8).

## arm64 webhelper (native client)

`steamrtarm64/steamwebhelper` is an aarch64 PIE with a 4 KiB-aligned
`libcef.so` of about 218 MB.

What stage 21 changed for it (MEASURED):

- trampoline pools per reachable range, for libcef's ~162 MiB of code;
- `blr x18` rewritten;
- libcef mapped from its file instead of copied: webhelper start
  2.52 s → 1.49 s;
- x18 rewriting over all of libcef's text (`LXRT_X18_ALL_TEXT=libcef.so`),
  because it has x18 uses outside its FDEs. `scripts/run-steam-arm64.sh`
  and both ARM64 Steam entries in `scripts/builtin-apps.json` set it.

Coverage of that rewrite: 76,048 x18 sites per process, 5 of them poisoned
in the 2026-09-28 trace; all 76,048 are rewritten since `926b89a`
(`docs/ARM64_REWRITE_COVERAGE.md`). Until `dbd1657` those 5 were live
`svc #1`; stage 22 then saw two of them trap as `brk #1` (below).

What stages 22 and 23 changed for it (MEASURED, each in its record):

| change | effect on the webhelper |
|---|---|
| X locale data, glibc locales, `libnssckbi.so`, SDL3 in the Fedora root (`15608cb`) | the webhelper initialises: 27 of 27 launches reached BrowserReady (stage 22 E2) |
| `mrs x18, nzcv` / `msr nzcv, x18` rewritten (`4534360`) | the browser process no longer traps 3-4 s after start (E4 → E5) |
| LDAR/STLR on x18 rewritten (`c86b633`) | the renderers no longer die on the poisoned `ldar w18, [x16]` (E7 → E9) |
| aarch64 `lsof` in the root (`cb257b3`) | the client stops rejecting the webhelper's transport connections with 403 (E13 → E14) |
| mmap at a 4 KiB file offset that is not on a 16 KiB host page (`6644fc7`) | the browser's shared-memory growth stops failing with EINVAL, and the X connection stops dying with "Fatal IO error 22" (E15 → E16) |
| RWX pages split W^X per 16 KiB page (`0519fb5`), and the runtime's SIGSEGV/SIGBUS handler kept when the zygote sets SIG_DFL (`d19704b`) | V8 runs with its JIT: 5 renderer launches in 180 s instead of 1137 in 90 s, and `--jitless` is no longer needed (stage 23 E1, E2, E5, E7) |

### Timeline (MEASURED)

| when | what |
|---|---|
| 2026-09-28 17:15:32 | BrowserReady in `steamui_html.txt` (run `native-arm64-steam-cef-fulltext-after-update-20260928.log`) |
| 2026-09-28 17:17:18 | `cef_log.txt`: D-Bus system bus socket `/run/dbus/system_bus_socket` missing |
| 2026-09-28 17:17:19 | `cef_log.txt`: the last NSS FATAL, `libsoftokn3.so: cannot open shared object` |
| after that | `libsoftokn3.so` and `libfreeblpriv3.so` are in the Fedora root's `/usr/lib64`; the DT_NEEDED closure of NSS, and of the client and libcef, resolves with nothing missing (read-only ELF walk, audit) |
| 2026-09-29 02:15:08-02:17:10 | 13 webhelper launches, 10 s apart, each with "Disabling sandbox due to a previous crash in CefInitialize"; no BrowserReady; `steamwebhelper.log` stops after the crash-key warnings |
| 2026-09-29 02:15 | `cef_log.txt` not written since 2026-09-28 17:17: the webhelpers of that early run never reached CEF logging |
| 2026-09-29 06:10 | stage 22 E2, rebuilt root (X locale data, locales, `libnssckbi.so`, SDL3), `lxrun` at `dbd1657`: 27 launches, 27 BrowserReady, 26 restarts; 1 FATAL in `cef_log.txt` ("GPU process isn't usable. Goodbye."), no NSS FATAL, no "Timed out waiting for webhelper init" |
| 2026-09-29 06:16-06:29 | E4, E7: the restarts were traps on poisoned libcef sites (`mrs x18, nzcv` in the browser, `ldar w18, [x16]` in every renderer); rewritten in E5, E8 |
| 2026-09-29 06:54-07:00 | E16, E18 (`--jitless`): the "Sign in to Steam" window, 1 webhelper launch, 0 restarts, 0 FATAL, up for the whole 150-180 s run; a screen capture shows the login form and a live sign-in QR code |
| 2026-09-29 07:56-08:39 | stage 23 E2, E5, E7 (JIT on): BrowserReady in 7-9 s, the window at every 15 s sample, 5 renderer launches in 180 s, 0 FATAL |
| 2026-09-29 09:03-09:32 | stage 23 F3-F6, N1 (Steam Frame root): BrowserReady in 15-17 s, the window in 5 of 5 runs, 0 FATAL; the first webhelper is restarted within a second of its start (cause UNKNOWN) |

So the stop before CEF initialisation seen at 02:15 did not come back after
the root was rebuilt (E2, and every later run). Which change removed it,
the root's new packages or the runtime at `dbd1657`, is UNKNOWN: both
changed between the two runs. The main client's `free()` abort, which
followed the webhelper timeout, is not reached any more
(`docs/STEAM_ARM64_BRINGUP.md`).

### What is known about the GPU process

In the traced 2026-09-28 run it loaded Mesa's software Vulkan and GL:
`libvulkan_lvp`, `libgallium-25.3.6.so`, `libLLVM.so.21.1` (MEASURED,
trace). It does not use SteamARM's Vulkan shim or MoltenVK. In that trace
libgallium had 1 refused x18 site (`mov sp, x18`, `writes sp`) and libLLVM
none (34,697 of 34,697 rewritten); the 2 `writes sp` sites earlier notes
gave libLLVM were in the main process's `steamclient.so`
(`docs/ARM64_REWRITE_COVERAGE.md`). All are rewritten since `926b89a`.

On XQuartz the GPU process cannot initialise GL: ANGLE reports "Cannot
create an OpenGL ES platform on GLX without the
GLX_ARB_create_context extension" on the Fedora root (stage 22), and
"... without the GLX_EXT_create_context_es_profile extension" on the Steam
Frame root (stage 23). The webhelper then composites in software
("Disabling GPU acceleration due to runtime detect") and the sign-in
window still comes (MEASURED).

## Open items

| item | state |
|---|---|
| why the 2026-09-29 02:15 webhelpers stopped before CEF init | not reproduced after the root rebuild (stage 22 E2 and every later run); cause UNKNOWN |
| NSS FATAL | not seen since the root rebuild: E2 logged one FATAL, the GPU process's, and E5, E16, E18 and stage 23's runs 0 (MEASURED, `cef_log.txt`) |
| `libnssckbi.so`, which libcef dlopens by name | in the Fedora root's seeds since stage 22 (a link to p11-kit-trust); present in the Steam Frame root |
| D-Bus system bus | no socket in either root; not needed to reach the sign-in window (both roots reach it, MEASURED). On the Frame root, Valve's `atomupd-manager` reads address 0x8 without it (4 fault reports per run) and the client carries on (stage 23) |
| the first webhelper's restart on the Frame root, and its two zygotes left alive with parent 1 | cause UNKNOWN (`benchmarks/stage23-frame-root.txt`, open items 1-2) |
| a webhelper zygote's SIGBUS inside a host `memset` at start | 1 of 10 launcher cycles in stage 23 (L2), no window in that cycle; cause UNKNOWN. The fault report now names the address, `sp` and `lr` (`ecca552`) and host frames (`b3f64c8`) for the next occurrence |
| "Timed out waiting for webhelper init" at once, no window (x86 client) | FIXED in stage 31: the client waits with 2400 x `ThreadSleep(50)`, each one `nanosleep` with no retry, and a realtime signal's carrier landing on its thread with nothing queued for it made every sleep return EINTR; the runtime now lets such a call go on (`tests/elf/quiet_interrupt.c`) |
| the browser waiting forever for a zygote's hello | FIXED in stage 31 for the waiting side: `read()` on the seqpacket pair now ends at a dead peer, as on Linux, so the web helper fails and is restarted instead of hanging. Why that zygote died (once in 8 starts, under load) is UNKNOWN |
| the native web helper frozen before the sign-in window, first start of a series | FIXED in stage 31: a deadlock in the runtime between `fork()`'s prepare handlers (which took the shared-page mirror's lock, then the page lock) and a thread placing a sub-page mapping (page lock, then the mirror's); `tests/elf/fork_mirror.c`. 8 of 8 starts afterwards |
| SIGTRAP in `pthread_jit_write_protect_np` (x86 client or helper) | FIXED in stage 31: every JIT mode flip runs with signals blocked (Apple's function traps when the register it wrote reads back different) |
| the x86 client jumping to an unmapped address (0x14c0_0000-0x1540_0000) from the same call site in `steamclient.so`, then its crash handler | open: 3 times on 09-30, always the same stack; cause UNKNOWN |
| GPU acceleration | none on XQuartz (above); software compositing only |
| V8's JIT under lxrun | works since stage 23 (MEASURED, `benchmarks/stage23-native-arm64-jit.txt`): RWX code pages are split W^X per 16 KiB page by the fault handler and scanned before they execute (`runtime/wxsplit.c`); the renderers had also been dying because the zygote resets SIGSEGV/SIGBUS to SIG_DFL, which switched off the runtime's own fault handling (fixed in `runtime/signal.c`). The earlier HYPOTHESIS that an unpatched V8 cannot JIT here is refuted |
| RWX-as-RW rule | applies to FEX guests only since stage 23; a native guest's RWX range is split W^X (`runtime/dispatch.c` do_mprotect_inner, `runtime/wxsplit.c`) |

## Next measurement

The standalone run this section proposed is no longer needed: the client's
own runs answered it (stage 22). What is not measured yet, in order:

1. Sign-in, then the main Steam window and the library (step 11 of
   `docs/ARM64_FIRST_MIGRATION.md`). Nothing past the sign-in window has
   been attempted.
2. A traced run for the next zygote SIGBUS at start, if it comes back:
   `scripts/run-steam-arm64.sh` with `LXRT_TRACE_MATCH=--type=zygote
   LXRT_TRACE_FILE=<file>`, then read the fault report's host frames.
3. Why the Frame root restarts the first webhelper
   (`benchmarks/stage23-frame-root.txt`, open item 1).
