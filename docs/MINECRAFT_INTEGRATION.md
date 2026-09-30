# Minecraft Java through Prism Launcher, ARM64-first

> **Status note (2026-09-29, after this record):** the owner set Minecraft aside in favour of
> Android (Play Store, APK install). The runtime fixes this work found (inotify FIONREAD,
> eventfd across fork, the opt-in `LXRT_KEEP_X18=1` build) are merged; the Prism launcher
> integration described below (the aarch64 installer, entry and tests) is **not** merged and
> the launcher still offers the x86_64 Prism AppImage under FEX. The measurements stay valid as a record.
>
> **Stage 28 (2026-09-30):** the SDK-12.3 lxrun is the **default** now (`make lxrt`;
> `LXRT_KEEP_X18=0` opts out), after the whole test matrix passed with it. Two things this page
> did not know: a guest signal handler used to overwrite a JIT's live x18 on return (HotSpot takes
> SIGSEGV in compiled code; fixed in `cc54e05`), and a **forked** child loses the kernel's x18 on
> either build, so a JVM started by fork without exec would still compute wrong results
> (`benchmarks/stage28-keep-x18.txt`, `docs/X18_VIRTUALIZATION.md`). Below, "opt-in" and
> `make lxrt LXRT_KEEP_X18=1` are as of stage 24.

**Status, 2026-09-29 (stage 24).** Prism Launcher's native aarch64 build and
an aarch64 Java 21 run under lxrun in the Fedora armroot, with no FEX and no
virtual machine: Prism's window, its settings and its Java detection work,
and it closes and reopens cleanly (MEASURED). **Minecraft does not run yet**,
for two reasons measured here:

1. Linux Java's JIT output uses the x18 register, which macOS zeroes, so
   with the default runtime compiled Java code computes wrong results and
   crashes. An lxrun linked as built against the macOS 12.3 SDK keeps x18
   (`make lxrt LXRT_KEEP_X18=1`), and with it the same code is correct
   (MEASURED); that build is opt-in until the rest of SteamARM is measured
   with it.
2. No OpenGL 3.2 core path both renders correctly and shows its frames in an
   X window yet: Zink on the M4's GPU renders a correct 3.2 core context off
   screen, and presenting it fails.

Minecraft itself was never started and no account was signed into. The
record is `benchmarks/stage24-minecraft-prism.txt`; the probes are
`tests/arm64/minecraft_probes.sh`.

Labels: MEASURED, VERIFIED IN SOURCE, UPSTREAM DOCUMENTED, HYPOTHESIS,
UNKNOWN.

## What the launcher installs

**+ → Minecraft Java (Prism Launcher)** (`launcher/Installers.swift`,
`installPrism`), ARM64 first:

| piece | from | where (host) | guest |
|---|---|---|---|
| Prism Launcher, `Linux-aarch64-Qt6-Portable` tarball (11.1.1 measured) | GitHub releases API, `PrismLauncher/PrismLauncher` latest; sha256 checked against GitHub's published digest | `~/SteamARM-roots/armroot/opt/apps/prism` | `/opt/apps/prism` |
| Eclipse Temurin 21 JRE, aarch64 Linux (21.0.12.1+1 measured) | Adoptium API v3 (`PrismARM64.javaQuery`); sha256 checked against Adoptium's checksum | `.../opt/apps/prism/java` | `/opt/apps/prism/java/bin/java` |
| Prism's data (instances, settings) | created empty | `.../opt/apps/.prism-home` | `HOME=/opt/apps/.prism-home` |

Licences: Prism Launcher GPL-3.0-only; Temurin GPL-2.0 with the Classpath
Exception. Neither is put in the repository or the `.dmg`; the launcher
downloads them from their upstream release URLs, and the app directory gets a
`STEAMARM-INSTALL.txt` with the URLs, the sha256 and the licences.
`opt/apps` is kept by a rebuild of the armroot (`scripts/roots.sh`), and the
data directory is outside the app directory, so reinstalling keeps it.

The entry (VERIFIED IN SOURCE, `PrismARM64` in `launcher/ApplicationCore.swift`;
MEASURED through `scripts/run-app.sh --dry-run` in
`tests/launcher/run_app_dispatch.sh`):

- `architecture: aarch64`, root `/tmp/lxrt-armroot`, so `run-app.sh` sends it
  to `scripts/run-native.sh`: translator none, session ZERO-VM.
- command `/bin/bash /opt/apps/prism/steamarm-sharun-run.sh /opt/apps/prism
  prismlauncher`;
- environment `HOME_IN_GUEST=/opt/apps/.prism-home`, `LANG=C.UTF-8`,
  `PRISMLAUNCHER_DISABLE_GLVULKAN=1` (what Prism's own start script sets for
  this build), `PRISMLAUNCHER_JAVA_PATHS=/opt/apps/prism/java/bin/java`;
- `readiness: experimental`. Opening it first says what works and that
  Minecraft does not (`LauncherModel.confirmExperimental`).

If the ARM64 install cannot be done (no armroot, no aarch64 asset in the
release, a digest that does not match, a package whose layout is not the
measured one), the installer falls back to the x86_64 AppImage under FEX, as
before, and names that entry "Minecraft (Prism Launcher, x86_64)". Installing
the ARM64 build over an earlier x86 entry removes the old program directory
(`LauncherModel.upsertReplacingInstall`); the x86 Prism's data, in the x86
root's `/tmp/fexhome/.local/share/PrismLauncher`, is not moved.

### Why the bundle is not run as shipped

MEASURED, `benchmarks/stage24-minecraft-prism.txt` ("The bundle"):

- The tarball is a sharun bundle. Its launcher, `bin/prismlauncher`, is sharun
  itself: a static non-PIE (ET_EXEC) binary linked at 0x400000, which lxrun
  refuses (macOS keeps the low 4 GiB). Every other ELF in it is a PIE or
  shared object aligned at 64 KiB, so no `LXRT_GUEST_PAGE=4096` is needed.
- `scripts/guest/sharun-run.sh` does sharun's job inside the guest: it runs
  the bundle's own `ld-linux-aarch64.so.1` with the library path from
  `shared/lib/lib.path` on `shared/bin/prismlauncher`. The bundle's glibc,
  Qt 6.10.2 and X libraries are used; nothing was added to the root.
- With the loader as the program, `/proc/self/exe` names the loader, so Prism
  takes `shared/` as its root and looks for its jars in `shared/share`. The
  install links `shared/share -> ../share`.
- The install removes Prism's self-updater: started without the bundle's
  library path it exits 127 and Prism shows an error dialog, and SteamARM's
  installer decides versions anyway.
- The data directory is outside `/tmp`: under it Prism warns on every start
  that the instance folder is in a temporary folder. `TMPDIR` cannot be moved
  instead: lxrun uses it as a host path for its `/proc`.

## Acceptance (MEASURED)

| step | result |
|---|---|
| start | first window (Quick Setup) in 1.03-1.05 s, in each of the 11 timed starts after the inotify fix |
| interaction | Finish → main window → Settings → Java → Detect finds `"21.0.12.1" "aarch64" "/opt/apps/prism/java/bin/java"`; Test Settings: "Java test succeeded! Platform reported: aarch64"; `prismlauncher.cfg`: `JavaPath=/opt/apps/prism/java/bin/java` |
| close | SIGTERM to Prism's PID: exits at once (status -15) |
| leftovers | 0 processes in its process group, 0 Prism windows; lxrun's `/tmp/lxrt-proc-<pid>` is swept by a later lxrun start |
| relaunch | the window again, 6 of 6 cycles |

Without a signed-in Microsoft account every start opens Prism's "Add
Microsoft account" page first (upstream behaviour); **Finish** skips it.
Nothing here signed in.

Three runtime defects had to be fixed on the way (MEASURED, with tests in
`tests/elf/run.sh`): inotify's FIONREAD returned the readiness byte instead
of the queued events' size (Qt's GUI thread blocked, blank window); an
eventfd was two objects after `fork()` (every Qt `QProcess` child spun at
100 % CPU and never exec'd, so Prism could not run Java); and a fault outside
the guest image now reports the instruction and x18.

## Java

Temurin 21 aarch64 runs: `java -version` in 1.0 s, threads, class data
sharing, and HotSpot's JIT is active: C2 compiles a hot loop, 33.7 ms against
264.3 ms interpreted for the same work (MEASURED). Prism's Java checker and
"Test Settings" run it through `QProcess`.

**The JIT's output is not correct under lxrun** (MEASURED): a loop that keeps
twenty values live gives 664 wrong results of 800 with the JIT, 474 with C1
only, 646 with C2 only, 0 of 80 interpreted and 0 on the Mac's own JVM; with
one thread it died in C1 code at `ldr w4, [x18, #256]` with x18 = 0. The
cause: HotSpot reserves x18 only when built for macOS or Windows
(`R18_RESERVED`, UPSTREAM DOCUMENTED in openjdk/jdk21u), so a Linux JVM's
C1 and C2 allocate it; Darwin zeroes x18 on every exception (preemption,
signals: `docs/X18_VIRTUALIZATION.md`), and lxrun rewrites x18 only in the
images it loads, not in code a JIT generates. Short runs (Prism's Java check,
`-version`) pass; a game would not. `-Xint` is correct but about 8 times
slower on that loop.

**The fix, measured and opt-in.** xnu keeps x18 for a program built against
a macOS SDK older than 13, and does so on this M4 under macOS 27
(`tests/x18_preserve/run.sh`, MEASURED). `make lxrt LXRT_KEEP_X18=1` links
lxrun that way (`LC_BUILD_VERSION sdk 12.3`). With it: 0 wrong results of
800 in 9 runs (tiered, C1 only, C2 only, 20 threads and 1), the same values
as the Mac's own JVM; llvmpipe's JIT runs clean too; `tests/elf/run.sh`
69/0; Prism behaves the same (window, Java detection, SIGTERM, relaunch).
It is not the default because the x86 Steam client, Proton, the Windows
probes, Vulkan presentation and the native arm64 Steam client have not been
measured with it (`docs/X18_VIRTUALIZATION.md`). To use it:

```sh
rm -f build/lxrun && make lxrt LXRT_KEEP_X18=1
tests/arm64/minecraft_probes.sh     # reports "x18 kept by the kernel" and expects the JIT checks to pass
```

Other ways, not tried (HYPOTHESIS): a Linux aarch64 JDK built with
`R18_RESERVED` (HotSpot's macOS path: C++ and ADLC), or the runtime
rewriting x18 in JIT output when it becomes executable (HotSpot patches its
own code, so trampolines there need care).

## OpenGL

Minecraft 1.17 and later renders with OpenGL 3.2 core (UPSTREAM DOCUMENTED,
minecraft.wiki "Java Edition 1.17") and gets its context from GLFW, which on
X11 uses GLX unless the program asks for EGL. `tests/arm64/gl_core_probe.c`
asks for exactly that context in the armroot (Mesa 25.3.6), draws, samples a
texture and reads back:

| path | context | draws correctly | in an X window |
|---|---|---|---|
| GLX, default or `LIBGL_ALWAYS_SOFTWARE=1` | none: Mesa's software screen finds no matching config of this X server and falls back to indirect GLX, which has no `GLX_ARB_create_context_profile` (GLXBadFBConfig) | - | - |
| GLX with Zink | none: "DRI3 not available" (the X server has no DRI3) | - | - |
| EGL, llvmpipe | 4.5 core | yes, but with the default runtime its LLVM JIT code dies on x18 (10 of 10 runs of 3000 frames: `stp xzr, xzr, [x18]`); with `LXRT_KEEP_X18=1` 5 of 5 clean | no: the frames do not reach the window (cause UNKNOWN; the same with `LXRT_KEEP_X18=1`) |
| EGL, Zink over SteamARM's Vulkan shim (MoltenVK, Apple M4) | 3.2 core, GLSL 1.50 | yes, texture sampling too; 5 of 5 runs of 3000 frames | no: MoltenVK cannot compile Zink's present shader ("must use 'struct' tag to refer to type 'sampler'") and Zink then crashes in MoltenVK |
| EGL, Zink over KosmicKrisp | - | no: its shaders fail to compile ("type 'float3' ... is not valid for attribute 'position'") | - |

So the renderer exists: Zink on the M4's GPU gives the 3.2 core context
Minecraft asks for and draws correctly off screen (MEASURED). What is missing
is getting its frames into a window, and getting GLFW to that context. Ways
forward (HYPOTHESIS): make Zink's X11 present work on MoltenVK (the shader
name collision, or presenting through the shim's own swapchain), then either
a GLX that can hand out that context (DRI3 in the X server, or a GLX vendor
library in the root backed by EGL) or GLFW's EGL context API in the game.
llvmpipe, a slower software fallback, renders correctly with
`LXRT_KEEP_X18=1` and has the same presentation problem.

Also available, not tried: LWJGL's aarch64 Linux natives (Prism's metadata
lists `natives-linux-arm64` jars for LWJGL 3.3.3, UPSTREAM DOCUMENTED).
Sound (OpenAL through PulseAudio) is UNKNOWN.

## Reproduce

```sh
make lxrt shim
tests/elf/run.sh                       # inotify FIONREAD, eventfd across fork
tests/arm64/minecraft_probes.sh        # JVM and GL probes; needs Prism installed
tests/launcher/run_app_dispatch.sh     # the Prism ARM64 entry goes native
tests/x18_preserve/run.sh              # does the kernel keep x18 for an SDK 12.3 build?
```

Prism itself: install it from the launcher (**+ → Minecraft Java**), or run
the entry's command with `scripts/run-native.sh` and the environment above
(`LXRT_ROOT=/tmp/lxrt-armroot`, `DISPLAY=:2`).
