# Graphics backends

How a Linux program's Vulkan calls reach Metal, which drivers exist on the
Mac, and which backends the launcher may offer. Written at `dbd1657`;
updated 2026-09-29 for the driver selection (`4ccb914`), the present-mode
report (`97aaf17`), `fillModeNonSolid` (`8a716c9`), the V-Sync fix
(`7ad4919`) and the launcher's capability model (`1de9fd1`, `774f590`).
Line numbers are at `b3f64c8`.

Labels: MEASURED (a command and its result), VERIFIED IN SOURCE (file:line),
UPSTREAM DOCUMENTED, HYPOTHESIS, UNKNOWN.

## The path today: the shim over MoltenVK (or KosmicKrisp)

```
aarch64 program ─────────────────────────────┐
                                             ├─ shim libvulkan.so.1 (aarch64 ELF) ─ MoltenVK ─ Metal
x86-64 / i386 program ─ libvulkan-guest.so ─ FEX thunk ─ libvulkan-host.so ─┘   (or KosmicKrisp,
   (DXVK, VKD3D-Proton, winevulkan)                                               STEAMARM_VK_ICD)
```

- **aarch64 callers** link `build/libvulkan.so.1` directly, as the
  system's Vulkan loader (MEASURED, `benchmarks/stage4-shim.txt`;
  `tests/elf/run.sh`). Fedora's unmodified `vulkaninfo` runs through it:
  apiVersion 1.4.357, MoltenVK 1.4.2, 296 extensions
  (`benchmarks/stage4-vulkaninfo.txt:11-20`).
- **x86 and i386 callers** reach the same shim through FEX's Vulkan thunks,
  64-bit (`benchmarks/stage10-vulkan-thunks.txt`, 17 ns per call,
  stage 14) and 32-bit (`benchmarks/stage16-32bit-vulkan.txt`).
- Nothing else is thunked: no GL, no X11, no audio library
  (`docs/CURRENT_STEAM_ENVIRONMENT.md` §6).

### The shim (`shim/`)

| part | does |
|---|---|
| `gen.py` → `vulkan_shim.{c,S}` | one exported entry point per Vulkan name (431 in `shim/entrypoints.txt`); a table filled by `dlsym` on MoltenVK, or through `vk_icdGetInstanceProcAddr` in ICD mode (KosmicKrisp). An empty slot traps at `brk #2`; in ICD mode `vkGetInstanceProcAddr` answers NULL for it instead, as the Khronos loader does (`benchmarks/stage22-kosmickrisp.txt` §1) |
| `wsi.c` | X11/xcb surfaces: a `CAMetalLayer` per X window from the runtime, published as `_STEAMARM_LAYER` (`shim/wsi.c:185-189`); `docs/WINDOWING_AND_PRESENTATION.md` |
| `features.c` | reports `geometryShader`, `shaderCullDistance` and, since `8a716c9`, `fillModeNonSolid` and strips them from `vkCreateDevice`; claims `VK_EXT_depth_clip_enable` (emulated with depth clamp), `VK_EXT_transform_feedback` (commands are no-ops) and `VK_EXT_dynamic_rendering_unused_attachments`, only when the driver lacks them. `LXRT_VK_NO_SPOOF=1` turns it off |
| `fallback.c` | replaces a compute pipeline MoltenVK cannot compile by an empty one (VKD3D-Proton's DirectStorage decompression); `LXRT_VK_NO_STUB_PIPELINES=1` turns it off |
| `memcap.c` | caps the reported device-local heap and memory budget (`LXRT_VK_MAX_VRAM_MB`, the VRAM setting) |
| `map32.c` | `vkMapMemory` for 32-bit guests: host pages aliased into FEX's guest window |
| `gen_rebase.py` → `vk_rebase.c` | rebases guest pointers from FEX's low window before they reach the driver; forwards only, when there is no guest base |

The spoofs are workarounds: a program that really uses a geometry shader or
stream output fails or draws wrong (HYPOTHESIS: few games do;
`shim/features.c`).

### How the driver is loaded

- By absolute path, first found wins: `/opt/homebrew/lib/libMoltenVK.dylib`,
  then `/usr/local/lib/libMoltenVK.dylib` (VERIFIED IN SOURCE,
  `shim/gen.py:80-83`, generated into `shim/vulkan_shim.c:453-456`). No ICD
  JSON and no loader.
- `STEAMARM_VK_ICD` picks another driver (since `4ccb914`; `shim/gen.py:85-92`
  and `:165-170`, `shim/vulkan_shim.c:538`): unset or `moltenvk` is
  MoltenVK; `kosmickrisp` is `/opt/homebrew/lib/libvulkan_kosmickrisp.dylib`,
  then `/usr/local/lib/...`; a value starting with `/` is that dylib. A
  driver that cannot be used falls back to MoltenVK with one line on
  stderr. The variable is read with the host's `getenv`, so it must be in
  the environment the process was started with (MEASURED and VERIFIED IN
  SOURCE, `benchmarks/stage22-kosmickrisp.txt` §1, §4).
- Installed: MoltenVK 1.4.2 from Homebrew
  (`/opt/homebrew/lib/libMoltenVK.dylib` → `Cellar/molten-vk/1.4.2`), 431
  exported `vk*` symbols (MEASURED, `readlink`, `nm -gU`, 2026-09-29).
- No version pin: it is whatever Homebrew installed; everything was measured
  on 1.4.2.
- **driverVersion is passed through unchanged** (`AGENTS.md`). MoltenVK
  reports `10402`; Steam decodes it as a Vulkan packed version and shows
  "0.2.2210". The launcher's "MoltenVK instalado" asks the library itself
  (`vkGetVersionStringsMVK` in `scripts/compat-status.py`, same search order
  as the shim) and shows 1.4.2 (MEASURED).

## KosmicKrisp

Mesa's Vulkan-on-Metal driver.

- **Installed on this Mac** (MEASURED, 2026-09-29): Homebrew `mesa` 26.2.3;
  ICD JSON `/opt/homebrew/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json`,
  API 1.4.354; the dylib's minimum OS is macOS 27.0 (`otool -l`).
- **Conformant on the M4**: `VK_DRIVER_FILES=<that JSON> vulkaninfo --summary`
  gives Apple M4, `DRIVER_ID_MESA_KOSMICKRISP`, driverInfo "Mesa 26.2.3",
  apiVersion 1.4.354, conformanceVersion 1.4.3.2 (MEASURED).
- **It exports only the ICD entry points**: 3 `vk*` symbols
  (`vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr`,
  `vk_icdNegotiateLoaderICDInterfaceVersion`), against MoltenVK's 431
  (MEASURED, `nm -gU`). So the shim resolves its names through
  `vk_icdGetInstanceProcAddr` (ICD mode, `4ccb914`): after
  `vk_icdNegotiateLoaderICDInterfaceVersion(7)`, every slot from
  `gipa(NULL, name)`, and the ones still empty again after every
  `vkCreateInstance`. No dispatch pointer is needed in the handles: the
  driver dispatches through its own tables (UPSTREAM DOCUMENTED, Mesa
  `vk_object.h`, `vk_instance.c`; MEASURED, host probe;
  `benchmarks/stage22-kosmickrisp.txt` §2).
- **Selectable, MEASURED with probes** (`benchmarks/stage22-kosmickrisp.txt`
  §4-5): with `STEAMARM_VK_ICD=kosmickrisp`, aarch64, x86-64 and i386
  guests (FEX thunks) create an instance and a device, submit, read back
  and present to an X window; `vulkaninfo` (x86-64) reports KosmicKrisp,
  Mesa 26.2.3, apiVersion 1.4.354. The D3D11 (DXVK) and D3D12
  (VKD3D-Proton) probes under Proton Experimental run on it at 158.1 and
  154.1 fps (MoltenVK in the same session: 153.7 and 158.4; other guests
  were running). These are clear-and-present probes near the 165 Hz
  refresh, not game performance. Not run on KosmicKrisp: D3D9, the 32-bit
  D3D probes, Steam's own launch path (whether pressure-vessel passes
  `STEAMARM_VK_ICD` to a game is UNVERIFIED), any game.
- **Differences that matter for DXVK/VKD3D-Proton** (MEASURED, `vulkaninfo`
  diff): KosmicKrisp has `fillModeNonSolid` false, `logicOp` true,
  `shaderCullDistance` true, `robustBufferAccess2` and `nullDescriptor` true;
  MoltenVK the opposite on each. Only MoltenVK has `VK_KHR_present_wait`,
  `VK_KHR_present_id`, `VK_KHR_portability_subset` and
  `VK_KHR_incremental_present`. Neither has `depth_clip_enable` or
  `transform_feedback`, so `features.c` is still needed; it queries the
  driver, so it is not MoltenVK-specific. DXVK needs `fillModeNonSolid`:
  without it DXVK logs "Skipping: Device does not support required feature
  'fillModeNonSolid'" and finds no adapter, so both D3D probes fail; the
  shim has reported it since `8a716c9` (MEASURED, stage 22 §5). Whether
  wireframe drawing then works on KosmicKrisp: UNKNOWN.
- Launcher state: `unavailable` in the static table
  (`launcher/ApplicationCore.swift:538`); `experimental` once detection
  finds KosmicKrisp installed, macOS 26 or later, loadable as an ICD, and
  an installed shim that contains `STEAMARM_VK_ICD`
  (`launcher/ApplicationCore.swift:554-565`, `scripts/compat-status.py`).
  Then **Configuración → Gráficos → Motor → Vulkan (KosmicKrisp)** makes
  `scripts/settings-env.py` export `STEAMARM_VK_ICD=kosmickrisp`
  (`scripts/settings-env.py:143-147`). `scripts/setup.sh` installs the
  shim this checkout builds (`scripts/install-steamroot-gfx.sh`); KosmicKrisp
  itself comes from Homebrew's `mesa`, which setup does not install.

## OpenGL and WineD3D

- Guest OpenGL exists only as Mesa in the x86 FEX rootfs: llvmpipe,
  software rendering (`glxinfo` under FEX reported llvmpipe OpenGL 4.6,
  `benchmarks/stage8-steam-zero-vm.txt`). There is no FEX GL thunk: only the
  Vulkan thunks are built and installed (VERIFIED IN SOURCE,
  `scripts/build-fex-thunks.sh`, `scripts/install-steamroot-gfx.sh`).
- The native X server has indirect GLX 1.4 (`+iglx`), measured with a test
  window (`benchmarks/stage11-native-x11.txt`). No game used it.
- So `PROTON_USE_WINED3D` would render in software: **unsupported for
  games**. Two hardware routes are untested (HYPOTHESIS): WineD3D's Vulkan
  renderer (`WINE_D3D_CONFIG=renderer=vulkan`) over the existing thunk, or
  Zink over it.
- Launcher state: unsupported (`launcher/ApplicationCore.swift:539`); the
  OpenGL (WineD3D) choice is disabled with its reason, and detection never
  makes it usable.

## Present modes and V-Sync

- On a Metal surface (`VK_EXT_metal_surface`) both MoltenVK and KosmicKrisp
  offer **FIFO and IMMEDIATE only**; no MAILBOX, no FIFO_RELAXED (MEASURED,
  `vulkaninfo`). So `dxvk.tearFree` and MAILBOX requests do nothing.
  MoltenVK has no environment variable for the present mode; it comes from
  the program's `VkSwapchainCreateInfoKHR`.
- The shim reports it since `97aaf17`: with `LXRT_VK_DEBUG=1` each
  `vkCreateSwapchainKHR` logs the driver, the requested mode and the mode
  passed on, and `LXRT_VK_PRESENT_MODE=FIFO|IMMEDIATE` overrides the
  request when the surface offers that mode (`shim/present.c:90-144`).
  What the probes ask for (MEASURED, stage 22 §6): DXVK's D3D11 probe
  IMMEDIATE on both drivers; VKD3D-Proton's D3D12 probe FIFO on MoltenVK
  and IMMEDIATE on KosmicKrisp (why they differ: UNKNOWN). The effective
  Metal state (`CAMetalLayer.displaySyncEnabled`) is still not reported.
- The V-Sync setting (`scripts/settings-env.py:100-107`):
  - DXVK: `dxgi.syncInterval` and `d3d9.presentInterval`, options that exist
    in Proton's DXVK (MEASURED, `strings`).
  - VKD3D-Proton: `VKD3D_SWAPCHAIN_PRESENT_MODE`, written as `FIFO` /
    `IMMEDIATE` since `7ad4919`. Up to then it was written in lower case,
    which VKD3D-Proton ignores: it compares the value with `strcmp` against
    upper-case names and logs "Ignoring unrecognized value" (MEASURED,
    disassembly of Proton Experimental's `d3d12core.dll`). The fix is
    VERIFIED IN SOURCE; no run has checked VKD3D-Proton honouring the
    upper-case value. Proton 10.0's VKD3D-Proton has no such variable.
  - IMMEDIATE through the cross-process layer (MEASURED, stage 22 §6,
    `vk_x11_present`, 570 frames): the median stayed at the 165 Hz
    refresh (6.04-6.07 ms) on both drivers; the mean dropped below it in
    some runs and not in others. Whether IMMEDIATE tears or unlocks there
    is UNKNOWN: that probe waits for its fence every frame. What the
    launcher's V-Sync setting changes in a game is not measured.

## Backend model in the launcher

`launcher/ApplicationCore.swift` (VERIFIED IN SOURCE). **Configuración →
Gráficos** and the read-only **Runtime** page show it
(`launcher/SettingsView.swift`, `774f590`):

| backend | state | reason in the model |
|---|---|---|
| Vulkan · MoltenVK | ready | D3D9/11/12 probes (stages 14, 16) |
| Vulkan · KosmicKrisp | unavailable until detected; experimental when installed, on macOS 26 or later, loadable as an ICD and the installed shim reads `STEAMARM_VK_ICD` | `ApplicationCore.swift:538`, `:554-565` |
| OpenGL · WineD3D | unsupported | guest GL is software llvmpipe and there is no GL thunk (`:539`) |

AUTO means MoltenVK. Fallback order (`ApplicationCore.swift:617-623`):
KosmicKrisp, then MoltenVK, then WineD3D, only among usable backends; never
a VM.

## Measured results

| what | result | record |
|---|---|---|
| Vulkan from a Linux ELF, triangle, present | works, no VM | stage 4 |
| x86-64 `vulkaninfo` through the thunk | Apple M4, MoltenVK | stage 10 |
| D3D11 (DXVK) / D3D12 (VKD3D-Proton), 64-bit | 161.6 / 161.9 fps (600 frames) | stage 14 |
| D3D9/11/12, 64- and 32-bit, direct and through Steam's container | 159.9-163.4 fps | stage 16 |
| GPU submit and readback through the thunks | x86-64 and i386 ok | `tests/elf/run_vk_device.sh`, stage 16 |
| KosmicKrisp through the shim: aarch64, x86-64 and i386 device, readback and X11 present | ok | stage 22 (`stage22-kosmickrisp.txt`) |
| D3D11 / D3D12 probes, MoltenVK against KosmicKrisp, same session | 153.7 / 158.4 fps against 158.1 / 154.1 fps | stage 22 |

These probes clear the screen at the display's refresh rate. They show that
the path works, not how fast games run (`docs/PERFORMANCE_BASELINE.md`).

## Open gaps

- Geometry shaders and transform feedback are claimed, not implemented.
- Some Vulkan extensions are missing from the 32-bit thunks (`README.md`).
- The remote layer's drawable is 1× on Retina screens
  (`benchmarks/stage12-native-present.txt`).
- The only Apple GPU data recorded are `vulkaninfo` and the CPU ID
  registers (`benchmarks/stage5-idregs.txt`); there is no Metal GPU-family
  probe.
