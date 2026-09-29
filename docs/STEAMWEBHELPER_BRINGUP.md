# steamwebhelper (CEF) under lxrun

Steam's whole UI is Chromium: `steamwebhelper`, built on CEF (`libcef.so`).
The client waits for it and gives up with "Timed out waiting for webhelper
init" (2400 × 50 ms, `benchmarks/stage8-steam-zero-vm.txt:224-225`). This page
covers the x86 webhelper that works today and the arm64 one that does not
yet.

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
  because it has x18 uses outside its FDEs. No script sets this yet.

Coverage of that rewrite: 76,048 x18 sites, 5 poisoned, per process
(`docs/ARM64_REWRITE_COVERAGE.md`). Until `dbd1657` those 5 were live
`svc #1`.

### Timeline (MEASURED)

| when | what |
|---|---|
| 2026-09-28 17:15:32 | BrowserReady in `steamui_html.txt` (run `native-arm64-steam-cef-fulltext-after-update-20260928.log`) |
| 2026-09-28 17:17:18 | `cef_log.txt`: D-Bus system bus socket `/run/dbus/system_bus_socket` missing |
| 2026-09-28 17:17:19 | `cef_log.txt`: the last NSS FATAL, `libsoftokn3.so: cannot open shared object` |
| after that | `libsoftokn3.so` and `libfreeblpriv3.so` are in the Fedora root's `/usr/lib64`; the DT_NEEDED closure of NSS, and of the client and libcef, resolves with nothing missing (read-only ELF walk, audit) |
| 2026-09-29 02:15:08-02:17:10 | 13 webhelper launches, 10 s apart, each with "Disabling sandbox due to a previous crash in CefInitialize"; no BrowserReady; `steamwebhelper.log` stops after the crash-key warnings |
| 2026-09-29 | `cef_log.txt` not written since 2026-09-28 17:17: the 2026-09-29 webhelpers never reached CEF logging |

So BrowserReady was reached once, and the next day's webhelpers stopped
before CEF initialised. Why is the open blocker. It also causes the main
client's abort (`docs/STEAM_ARM64_BRINGUP.md`).

### What is known about the GPU process

In the traced 2026-09-28 run it loaded Mesa's software Vulkan and GL:
`libvulkan_lvp`, `libgallium-25.3.6.so`, `libLLVM.so.21.1` (MEASURED,
trace). It does not use SteamARM's Vulkan shim or MoltenVK. libgallium and
libLLVM carry 1 and 2 refused x18 sites (`writes sp`).

## Open items

| item | state |
|---|---|
| why the 2026-09-29 webhelpers stop before CEF init | UNKNOWN. Whether `LXRT_X18_ALL_TEXT` was set for that run is not recorded |
| NSS FATAL | UNKNOWN whether gone: the files are there now, but no later run reached CEF logging |
| `libnssckbi.so`, which libcef dlopens by name | absent from the Fedora root at the audit; present in the Steam Frame root |
| D-Bus system bus | no socket in either root; whether CEF needs one to start: UNKNOWN |
| V8's JIT under lxrun | HYPOTHESIS: an unpatched arm64 V8 cannot JIT on Apple Silicon under lxrun (RWX is refused, and a W^X flip cannot be done behind its back; `docs/CURRENT_STEAM_ENVIRONMENT.md` §7). BrowserReady comes from the browser process and does not show that a renderer ran JIT code. Ways out listed there: `--js-flags=--jitless`, a V8 that asks for the flip, or store emulation in the runtime |
| RWX-as-RW rule | right for x86 V8 under FEX, wrong for native code (`runtime/dispatch.c:1306-1321`); should apply to FEX guests only |

## Next measurement

A standalone webhelper run, compared with the 2026-09-28 run that reached
BrowserReady (from the native-arm64 audit, not run here):

```sh
LXRT_ROOT=/tmp/lxrt-armroot LXRT_GUEST_PAGE=4096 HOME=/tmp/armhome DISPLAY=:2 \
LXRT_X18_ALL_TEXT=libcef.so LXRT_TRACE_MATCH=steamwebhelper LXRT_TRACE_FILE=/tmp/wh.trace \
    build/lxrun /tmp/armhome/.local/share/Steam/steamrtarm64/steamwebhelper.sh <arguments from webhelper.txt>
```

Read `/tmp/wh.trace` for the last system calls before exit, and for a
SIGTRAP at a poisoned site (possible only after `dbd1657`).
