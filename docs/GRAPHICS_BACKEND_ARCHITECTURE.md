# Graphics backends

How a Linux program's Vulkan calls reach Metal, which drivers exist on the
Mac, and which backends the launcher may offer. Line numbers are at
`dbd1657`.

Labels: MEASURED (a command and its result), VERIFIED IN SOURCE (file:line),
UPSTREAM DOCUMENTED, HYPOTHESIS, UNKNOWN.

## The path today: the shim over MoltenVK

```
aarch64 program ─────────────────────────────┐
                                             ├─ shim libvulkan.so.1 (aarch64 ELF) ─ MoltenVK ─ Metal
x86-64 / i386 program ─ libvulkan-guest.so ─ FEX thunk ─ libvulkan-host.so ─┘
   (DXVK, VKD3D-Proton, winevulkan)
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
| `gen.py` → `vulkan_shim.{c,S}` | one exported entry point per Vulkan name (431 in `shim/entrypoints.txt`); a table filled by `dlsym` on MoltenVK. An empty slot traps at `brk #2` (`shim/gen.py:44-52`) |
| `wsi.c` | X11/xcb surfaces: a `CAMetalLayer` per X window from the runtime, published as `_STEAMARM_LAYER` (`shim/wsi.c:185-189`); `docs/WINDOWING_AND_PRESENTATION.md` |
| `features.c` | reports `geometryShader` and `shaderCullDistance` and strips them from `vkCreateDevice`; claims `VK_EXT_depth_clip_enable` (emulated with depth clamp), `VK_EXT_transform_feedback` (commands are no-ops) and `VK_EXT_dynamic_rendering_unused_attachments`, only when the driver lacks them. `LXRT_VK_NO_SPOOF=1` turns it off |
| `fallback.c` | replaces a compute pipeline MoltenVK cannot compile by an empty one (VKD3D-Proton's DirectStorage decompression); `LXRT_VK_NO_STUB_PIPELINES=1` turns it off |
| `memcap.c` | caps the reported device-local heap and memory budget (`LXRT_VK_MAX_VRAM_MB`, the VRAM setting) |
| `map32.c` | `vkMapMemory` for 32-bit guests: host pages aliased into FEX's guest window |
| `gen_rebase.py` → `vk_rebase.c` | rebases guest pointers from FEX's low window before they reach the driver; forwards only, when there is no guest base |

The spoofs are workarounds: a program that really uses a geometry shader or
stream output fails or draws wrong (HYPOTHESIS: few games do;
`shim/features.c`).

### How MoltenVK is loaded

- By absolute path, first found wins: `/opt/homebrew/lib/libMoltenVK.dylib`,
  then `/usr/local/lib/libMoltenVK.dylib` (VERIFIED IN SOURCE,
  `shim/gen.py:75-78`, generated into `shim/vulkan_shim.c:448-451`). No ICD
  JSON, no loader, no environment variable selects the driver.
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
  (MEASURED, `nm -gU`). So pointing the shim's path at it would leave 428
  table slots empty and trap on the first call (HYPOTHESIS by construction).
  The shim needs an ICD mode that resolves names through
  `vk_icdGetInstanceProcAddr`. That work is in progress elsewhere; at
  `dbd1657` the shim loads MoltenVK only.
- **Differences that matter for DXVK/VKD3D-Proton** (MEASURED, `vulkaninfo`
  diff): KosmicKrisp has `fillModeNonSolid` false, `logicOp` true,
  `shaderCullDistance` true, `robustBufferAccess2` and `nullDescriptor` true;
  MoltenVK the opposite on each. Only MoltenVK has `VK_KHR_present_wait`,
  `VK_KHR_present_id`, `VK_KHR_portability_subset` and
  `VK_KHR_incremental_present`. Neither has `depth_clip_enable` or
  `transform_feedback`, so `features.c` is still needed; it queries the
  driver, so it is not MoltenVK-specific. Whether DXVK needs
  `fillModeNonSolid`: UNKNOWN.
- Launcher state: unavailable (`launcher/ApplicationCore.swift:336`).

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
- Launcher state: unavailable (`launcher/ApplicationCore.swift:337`); the
  OpenGL setting is disabled with its reason.

## Present modes and V-Sync

- On a Metal surface (`VK_EXT_metal_surface`) both MoltenVK and KosmicKrisp
  offer **FIFO and IMMEDIATE only**; no MAILBOX, no FIFO_RELAXED (MEASURED,
  `vulkaninfo`). So `dxvk.tearFree` and MAILBOX requests do nothing.
  MoltenVK has no environment variable for the present mode; it comes from
  the program's `VkSwapchainCreateInfoKHR`.
- The shim forwards `vkCreateSwapchainKHR` without reading the mode
  (`vk_rebase.c`, generated), so it cannot report requested versus
  effective V-Sync today.
- The V-Sync setting (`scripts/settings-env.py:61-65`):
  - DXVK: `dxgi.syncInterval` and `d3d9.presentInterval`, options that exist
    in Proton's DXVK (MEASURED, `strings`).
  - VKD3D-Proton: **broken.** The script writes `VKD3D_SWAPCHAIN_PRESENT_MODE`
    as `fifo` / `immediate`. VKD3D-Proton compares the value with `strcmp`
    against upper-case names and logs "Ignoring unrecognized value"
    (MEASURED, disassembly of Proton Experimental's `d3d12core.dll`). Proton
    10.0's VKD3D-Proton has no such variable. Open at `dbd1657`.
  - Whether IMMEDIATE changes pacing through the cross-process layer:
    UNKNOWN (the FIFO probes run at the display's 165 Hz).

## Backend model in the launcher

`launcher/ApplicationCore.swift` (VERIFIED IN SOURCE; no UI reads it yet):

| backend | state | reason in the model |
|---|---|---|
| Vulkan · MoltenVK | ready | D3D9/11/12 probes (stages 14, 16) |
| Vulkan · KosmicKrisp | unavailable | the shim loads MoltenVK by path |
| OpenGL · WineD3D | unavailable | no host GL path for guest GL |

Fallback order (`ApplicationCore.swift:349-354`): KosmicKrisp, then
MoltenVK, then WineD3D, only among usable backends; never a VM.

## Measured results

| what | result | record |
|---|---|---|
| Vulkan from a Linux ELF, triangle, present | works, no VM | stage 4 |
| x86-64 `vulkaninfo` through the thunk | Apple M4, MoltenVK | stage 10 |
| D3D11 (DXVK) / D3D12 (VKD3D-Proton), 64-bit | 161.6 / 161.9 fps (600 frames) | stage 14 |
| D3D9/11/12, 64- and 32-bit, direct and through Steam's container | 159.9-163.4 fps | stage 16 |
| GPU submit and readback through the thunks | x86-64 and i386 ok | `tests/elf/run_vk_device.sh`, stage 16 |

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
