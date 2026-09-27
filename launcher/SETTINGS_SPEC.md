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
| timezone | "auto" or an IANA name ("UTC", "America/Montevideo", …) | "auto" (= the Mac's) | TZ of the guest |
| vsync | "game" \| "on" \| "off" | "game" | DXVK dxgi.syncInterval / d3d9.presentInterval (-1 / 1 / 0) |
| dramGB | 0 = automático, or N GB | 0 | memory ceiling for everything SteamARM runs (Steam + game): the memory guard stops the guests above it instead of letting the Mac run out |
| vramGB | 0 = automático, or N GB | 0 | video memory reported to games: Vulkan heap size in the shim (LXRT_VK_MAX_VRAM_MB) and DXVK dxgi.maxDeviceMemory |
| esync | bool | true | PROTON_NO_ESYNC when false |
| fsync | bool | true | PROTON_NO_FSYNC when false |
| fexDiskCache | bool | false | FEX_DISKCACHE (translation cache on disk, like PPTC) |
| fexTSO | "full" \| "fast" \| "off" | "full" | full: FEX_TSOENABLED=1; fast: TSO on, FEX_VECTORTSOENABLED=0, FEX_MEMCPYSETTSOENABLED=0; off: FEX_TSOENABLED=0 (help: "rápido, puede romper juegos multihilo") |
| fexMultiblock | bool | true | FEX_MULTIBLOCK |
| fexSMC | "none" \| "mtrack" \| "full" | "mtrack" | FEX_SMCCHECKS 0/1/2 |
| fexX87Reduced | bool | false | FEX_X87REDUCEDPRECISION |
| graphicsBackend | "vulkan" | "vulkan" | DXVK / VKD3D-Proton over Vulkan→MoltenVK→Metal. Show "OpenGL (WineD3D)" as a disabled option: "no disponible: sin OpenGL en este sistema" |
| shaderCache | bool | true | DXVK/VKD3D shader cache (DXVK_SHADER_CACHE=0 / VKD3D_SHADER_CACHE_PATH=0 when false) |
| anisotropy | 0 (automático) \| 2 \| 4 \| 8 \| 16 | 0 | DXVK d3d11.samplerAnisotropy and d3d9.samplerAnisotropy |
| frameRateLimit | 0 (sin límite) \| 30 \| 60 \| 90 \| 120 \| 144 | 0 | DXVK dxgi.maxFrameRate / d3d9.maxFrameRate, VKD3D_FRAME_RATE |
| dxvkHud | "off" \| "fps" \| "full" | "off" | DXVK_HUD (fps / full) |
| metalHud | bool (existing) | false | MTL_HUD_ENABLED |
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
   vramGB. "Hacks (pueden causar inestabilidad)": esync, fsync.
4. Procesador — "Caché de CPU": fexDiskCache. "Emulación x86 (FEX)": fexTSO,
   fexMultiblock, fexSMC, fexX87Reduced.
5. Gráficos — "API de gráficos": graphicsBackend. "Funcionalidades y
   mejoras": shaderCache, anisotropy, frameRateLimit, dxvkHud, metalHud.
   Disabled with reason: "Escala de resolución / FSR" ("Proton 11 ya no
   incluye el escalado FSR de pantalla completa"), "Suavizado de bordes".
6. Sonido — audioBackend, volume slider.
7. Atajos — hotkeys.
8. Registros — protonLog, wineDebug, dxvkLogLevel, vkd3dLogLevel, button
   "Abrir carpeta de registros" ($STEAMARM_STATE/logs).
9. Depuración — guestFaults, traceMatch, vulkanDebug, extraEnv editor
   (existing EnvEditor).

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
