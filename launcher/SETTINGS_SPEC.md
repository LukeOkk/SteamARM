# SteamARM settings window — specification

Layout copied from Ryujinx's configuration window (dark theme): a sidebar of
sections on the left, the selected section on the right, and a bottom bar
with "Restablecer la configuración" (enabled only while the checkbox
"Quiero restablecer mi configuración" is ticked), and "Aplicar", "Cancelar",
"Aceptar". Settings are edited on a copy and written to
`$STEAMARM_STATE/launcher/settings.json` (~/SteamARM-roots) on Aplicar/Aceptar
(Cancelar discards). All labels in Spanish.

Every control below is wired to something real. `scripts/settings-env.py`
turns settings.json into the environment of every program SteamARM starts
(apps started by the launcher and Steam; games inherit Steam's environment, so
Steam must be restarted for changes to reach games — say so under the bottom
bar when Steam is running). Controls marked "disabled" have no real backend in
this stack: show them greyed out with the reason as help text, never fake.

## settings.json (new keys; existing ones kept: metalHud, display,
## resolution, projectDir, extraEnv)

| key | type / values | default | effect (implemented in settings-env.py etc.) |
|---|---|---|---|
| launchSteamOnStart | bool | false | launcher starts Steam when it opens |
| confirmStop | bool | true | ask before stopping a running app |
| guestLanguage | "auto" or a locale like "es_ES.UTF-8", "en_US.UTF-8", "pt_BR.UTF-8", "fr_FR.UTF-8", "de_DE.UTF-8", "it_IT.UTF-8", "ja_JP.UTF-8" | "auto" (= the Mac's) | LANG / LC_ALL of the guest |
| timezone | "auto" or an IANA name ("UTC", "Europe/Berlin", …) | "auto" (= the Mac's) | TZ of the guest |
| vsync | "game" \| "on" \| "off" (shown AUTO / ON / OFF) | "game" | on/off: DXVK dxgi.syncInterval and d3d9.presentInterval 1/0, and VKD3D_SWAPCHAIN_PRESENT_MODE FIFO/IMMEDIATE (uppercase: vkd3d-proton compares with strcmp; Proton 10.0's vkd3d has no such variable). game: nothing set. MoltenVK offers only FIFO and IMMEDIATE; the effect on screen is not measured |
| dramGB | 0 = automático, or N GB | 0 | memory ceiling for everything SteamARM runs (Steam + game): the memory guard stops the guests above it instead of letting the Mac run out |
| vramGB | 0 = automático, or N GB | 0 | video memory reported to games: Vulkan heap size in the shim (LXRT_VK_MAX_VRAM_MB) and DXVK dxgi.maxDeviceMemory |
| synchronization | "auto" \| "wineserver" \| "msync" \| "fsync" \| "esync" (AUTO / DEFAULT / MSYNC / FSYNC / ESYNC) | "auto" | esync: PROTON_NO_FSYNC=1 only. Everything else resolves to wineserver: PROTON_NO_ESYNC=1 and PROTON_NO_FSYNC=1. FSYNC is shown disabled (lxrun has no futex_waitv, syscall 449), MSYNC disabled (no MSync-capable Wine), ESYNC experimental (only Proton 10.0 has it; eventfd across processes under lxrun is not verified) and disabled when no installed Proton has it. AUTO follows RuntimeCapabilities.effectiveSynchronization: wineserver today. Replaces the old "esync"/"fsync" booleans: a settings.json that still has them and no "synchronization" is translated exactly as before; the launcher migrates esync:true to "esync" and esync:false to "wineserver" |
| fallbackPolicy | "auto" \| "strict" \| "ask" | "auto" | what a launch does when a setting it asks for cannot work (VNC for a program outside the x86 root; a stored synchronization/graphics value that is no longer usable): AUTO launches with the fallback, STRICT refuses, ASK asks. No effect when every setting can work. Never a VM |
| fexDiskCache | bool | false | FEX_DISKCACHE (translation cache on disk, like PPTC) |
| fexTSO | "full" \| "fast" \| "off" | "full" | full: FEX_TSOENABLED=1; fast: TSO on, FEX_VECTORTSOENABLED=0, FEX_MEMCPYSETTSOENABLED=0; off: FEX_TSOENABLED=0 (help: "rápido, puede romper juegos multihilo") |
| fexMultiblock | bool | true | FEX_MULTIBLOCK |
| fexSMC | "none" \| "mtrack" \| "full" | "mtrack" | FEX_SMCCHECKS 0/1/2 |
| fexX87Reduced | bool | false | FEX_X87REDUCEDPRECISION |
| graphicsBackend | "auto" \| "vulkanMoltenVK" \| "vulkanKosmicKrisp" \| "openGLWineD3D" | "auto" | AUTO and MoltenVK: nothing set (the shim loads MoltenVK). KosmicKrisp: STEAMARM_VK_ICD=kosmickrisp, enabled only when scripts/compat-status.py finds it (ICD manifest, macOS ≥ 26, loads as an ICD) and the installed shim contains the string STEAMARM_VK_ICD; settings-env.py checks the shim again before exporting. OpenGL (WineD3D): disabled, unsupported (guest GL is software llvmpipe, no GL thunk). The old value "vulkan" reads as "vulkanMoltenVK" |
| shaderCache | bool | true | DXVK/VKD3D shader cache (DXVK_SHADER_CACHE=0 / VKD3D_SHADER_CACHE_PATH=0 when false) |
| anisotropy | 0 (automático) \| 2 \| 4 \| 8 \| 16 | 0 | DXVK d3d11.samplerAnisotropy and d3d9.samplerAnisotropy |
| frameRateLimit | 0 (sin límite) \| 30 \| 60 \| 90 \| 120 \| 144 | 0 | DXVK dxgi.maxFrameRate / d3d9.maxFrameRate, VKD3D_FRAME_RATE |
| dxvkHud | "off" \| "fps" \| "full" | "off" | DXVK_HUD (fps / full) |
| metalHud | bool (existing) | false | MTL_HUD_ENABLED |
| display (existing) | "native" \| "vnc" | "native" | shown as "Backend de la aplicación": Ventanas nativas ↔ native, VNC ↔ vnc. Lightning JIT and Apple Hypervisor are listed disabled with their reason and are never selectable; the caption says "Modo de sesión: ZERO-VM". run-app.sh uses native windows for a program outside the x86 root even when VNC is chosen |
| audioBackend | "coreaudio" \| "none" | "coreaudio" | sound through the Mac's output ("none": muted) |
| volume | 0…100 | 100 | output volume of games |
| hotkeys | { "screenshot": "F8", "stopApp": "", "toggleMetalHud": "" } | as shown | global shortcuts handled by the launcher: screenshot of the game window (saved to ~/Pictures/SteamARM), stop the running app. Recorded like Ryujinx: click the field, press a key; "Sin asignar" when empty |
| protonLog | bool | false | PROTON_LOG=1 (log in the guest home, button "Abrir carpeta de registros") |
| wineDebug | string | "" | WINEDEBUG |
| dxvkLogLevel | "none" \| "error" \| "warn" \| "info" \| "debug" | "warn" | DXVK_LOG_LEVEL |
| vkd3dLogLevel | "none" \| "err" \| "warn" \| "info" | "err" | VKD3D_DEBUG |
| guestFaults | bool | true | LXRT_GUEST_FAULTS=1 (fault reports in the app log) |
| traceMatch | string | "" | LXRT_TRACE_MATCH (runtime trace of matching processes; warning: huge logs) |
| vulkanDebug | bool | false | LXRT_VK_DEBUG=1 |

### Memory choices (Sistema → DRAM / VRAM)

Total RAM T (GB, from the Mac). The top of the list is the usable maximum
M = T − 2 when T ≤ 8, otherwise M = T − 4 (8 → 6, 16 → 12, 24 → 20,
32 → 28, 64 → 60, 128 → 124). The list is "Automático (N GiB)" (N = M)
followed by M and then every value of [48, 32, 24, 16, 12, 8, 6, 4, 2] below
M. Stored as 0 for automático.

## Sections (sidebar, Ryujinx order and SF Symbols)

1. Interfaz — launchSteamOnStart, confirmStop, display mode and resolution
   (existing DisplayTab content), project folder (existing).
2. Entrada — the controller page (below).
3. Sistema — "Núcleo": guestLanguage, timezone, vsync. "Memoria": dramGB,
   vramGB. "Sincronización (Proton)": synchronization. "Alternativas":
   fallbackPolicy.
4. Procesador — "Caché de CPU": fexDiskCache. "Emulación x86 (FEX)": fexTSO,
   fexMultiblock, fexSMC, fexX87Reduced.
5. Gráficos — "API de gráficos": graphicsBackend (with the detected
   MoltenVK and KosmicKrisp versions). "Funcionalidades y
   mejoras": shaderCache, anisotropy, frameRateLimit, dxvkHud, metalHud.
   Disabled with reason: "Escala de resolución / FSR" ("Proton 11 ya no
   incluye el escalado FSR de pantalla completa"), "Suavizado de bordes".
6. Sonido — audioBackend, volume slider.
7. Atajos — hotkeys.
8. Registros — protonLog, wineDebug, dxvkLogLevel, vkd3dLogLevel, button
   "Abrir carpeta de registros" ($STEAMARM_STATE/logs).
9. Depuración — guestFaults, traceMatch, vulkanDebug, extraEnv editor
   (existing EnvEditor).
10. Runtime — read-only: every capability of RuntimeCapabilities
    (presentation, execution, synchronization, graphics) with its state
    (Listo / Experimental / No soportado / No disponible) and reason, and
    what scripts/compat-status.py detected on this Mac (MoltenVK,
    KosmicKrisp, the shim's ICD selection, esync/fsync/ntsync per Proton,
    native X / Xvnc / Screen Sharing), with "Volver a comprobar".

## Per-app overrides (apps.json → overrides)

A user app (not the built-ins) may carry `"overrides": {key: value}` for
display, vsync, synchronization and graphicsBackend, edited in "Ajustes de
esta app" of its edit sheet ("Global" removes the key). scripts/run-app.sh
merges them into the settings with settings-env.py `with_overrides()`
before the environment is computed; other keys are ignored. Games started
by Steam inherit Steam's environment, so they follow the global settings.

## Library state (library.json)

`$STEAMARM_STATE/launcher/library.json` maps an app id to its launch
count, total time, last launch (ISO 8601), last exit status and outcome,
and favourite flag. It is separate from apps.json because the built-ins
(scripts/builtin-apps.json) are not stored there; every field is optional.

## Entrada (controllers) — like Ryujinx's input page

Top row: Jugador (1–4), Perfil (named profiles saved in
`$STEAMARM_STATE/launcher/controller-profiles/<name>.json`, buttons
load/save/delete), Dispositivo de entrada (SDL game controllers +
"Desactivado", refresh button), Tipo de mando.

Tipo de mando (stored per player as `controllerType`; it is also the identity
the game sees — vendor/product IDs — so games show the matching button
prompts):

| controllerType | shown as | USB id |
|---|---|---|
| xbox360 | Xbox 360 | 045e:028e |
| xboxone | Xbox One | 045e:02ea |
| xboxseries | Xbox Series X\|S | 045e:0b12 |
| xboxelite2 | Xbox Elite Series 2 | 045e:0b00 |
| ds3 | DualShock 3 (PS3) | 054c:0268 |
| ds4 | DualShock 4 (PS4) | 054c:09cc |
| dualsense | DualSense (PS5) | 054c:0ce6 |
| dualsenseedge | DualSense Edge (PS5) | 054c:0df2 |
| steamcontroller | Steam Controller (2015) | 28de:1102 |
| steamcontroller2 | Steam Controller (2026) | 28de:1302 |
| switchpro | Nintendo Switch Pro | 057e:2009 |

Centre: a vector drawing of the selected controller type (its real shape,
button layout and face-button glyphs: Xbox A/B/X/Y coloured; PlayStation
✕ ○ □ △; Steam Controller trackpads and grip buttons; Switch Pro), drawn in
SwiftUI (Canvas/Path), highlighting pressed controls live from the selected
device, and the two stick positions below it (as Ryujinx). Clicking a button
on the drawing starts remapping that control.

Side panels (as Ryujinx): triggers/bumpers, left stick (button, stick, invert
X, invert Y, rotate 90°, deadzone 0–1, range 0–2), D-pad, face buttons, right
stick (same options), trigger threshold, "Vibración" (rumble on/off +
strength), "LED" (colour, DualShock 4 / DualSense only). Button captions
follow the controller type (Xbox: A B X Y LB RB LT RT View Menu; PlayStation:
✕ ○ □ △ L1 R1 L2 R2 Share/Create Options; Switch: B A Y X L R ZL ZR − +; Steam
Controller: A B X Y, LB RB LT RT, Back Start, left/right grip, pads).

Per-player JSON (controllers.json → players[i]): deviceGUID, deviceName,
controllerType, mapping (slot → SDL control name), invertLX, invertLY,
rotateL, invertRX, invertRY, rotateR, deadzoneLeft, deadzoneRight, rangeLeft,
rangeRight, triggerThreshold, rumble, rumbleStrength (0–1), ledColor
("#RRGGBB" or null), motion (bool, DualShock 4 / DualSense / Switch Pro).
Keep reading the old fields (controllerType "ProController" → "switchpro",
"Xbox" → "xboxseries").

Delivery to games is done by the input service (runtime + scripts), which
reads controllers.json; the launcher only edits the file (and keeps the live
test view).
