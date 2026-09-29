# Entorno Steam actual: auditoría de migración

Fecha de observación: 2026-09-28. Objetivo: fijar la línea base antes de
migrar el userspace ARM64 a Holo Core. Todo el camino sigue siendo macOS
nativo y ZERO-VM: no hay kernel Linux invitado ni hipervisor.

## Lifecycle observado

La biblioteca SwiftUI invoca `scripts/run-app.sh <id>` desde
`launcher/LauncherModel.swift`. La app integrada Steam resuelve a
`steam.sh`; una app Windows resuelve Proton instalado mediante
`scripts/proton-command.py`, que asigna un prefijo separado bajo
`$STEAMARM_STATE/steamroot/tmp/fexhome/.steamarm/prefixes/<id>`.

`scripts/run-app.sh` prepara pantalla, sonido, entrada y entorno por app;
`scripts/run-fex.sh` inicia `build/lxrun` con el FEX ARM64 adecuado. `lxrun`
es proceso Mach-O de macOS: carga ELF Linux ARM64 y traduce sus syscalls a
Darwin/XNU. FEX ejecuta el código x86/x86-64/i386 en el mismo proceso/runtime;
no existe arranque de kernel ni guest. FEXServer persiste entre procesos
FEX. `--stop` detiene apps guest; X y FEXServer tienen lifecycle separado.

Presentación principal: X11 rootless nativo en `:2`, cuyas ventanas aparecen
como ventanas macOS. Xvnc/Screen Sharing es ruta alternativa de diagnóstico.
Audio cruza PulseAudio a CoreAudio; `steamarm-inputd` y el runtime exponen
mandos como evdev. Vulkan cruza FEX thunks y `shim/libvulkan.so.1` hasta
MoltenVK 1.4.2.

## Roots y componentes

| Ubicación lógica | Construcción/contenido actual | Clasificación |
|---|---|---|
| `$STEAMARM_STATE/lxrt-root` (`/tmp/lxrt-root`) | Root ARM64 mínimo derivado de RPM Fedora 43; FEX, FEXServer, loader/glibc y utilidades | `REPLACE_WITH_HOLO` |
| `$STEAMARM_STATE/armroot` | Segundo root Fedora 43, ampliado con GTK/X11/audio para el experimento de Steam ARM64 directo | `REPLACE_WITH_HOLO`, experimento `REMOVE_LATER` tras migración validada |
| `$STEAMARM_STATE/x86-rootfs` | Ubuntu 24.04.4 LTS descargado de FEX; contiene ABI x86-64 e i386 y fixups locales | `GUEST_X86_REQUIRED`, conservar hasta probar sustituto |
| `$STEAMARM_STATE/steamroot` (`/tmp/lxrt-steamroot`) | Ubuntu x86-64 fusionado con utilidades y bibliotecas ARM64; Steam, herramientas Proton y runtimes instalados en `$HOME` guest | Mixto: x86 `GUEST_X86_REQUIRED`; capa ARM Fedora `REPLACE_WITH_HOLO` |
| `$STEAMARM_BUILD` | FEX compilado en macOS para ELF Linux ARM64, thunks, toolchains y cachés | `KEEP` durante migración; revisar al cerrar build reproducible |

El root Ubuntu contiene el Steam client `ubuntu12_32/steam` como ELF i386.
Steam administra varias instalaciones Proton y Steam Linux Runtime. Las
instalaciones observadas incluyen Proton Experimental/10/Hotfix x86,
Proton 11 ARM64 y Experimental ARM64; también existen
`SteamLinuxRuntime_4`, `SteamLinuxRuntime_4-arm64`, `sniper` y `soldier`.
Esto prueba presencia de payloads descargados, no que la ruta ARM64 ya pueda
ejecutarse desde el cliente x86 bajo FEX.

El inventario del launcher busca manifiestos Valve en `steamapps/common` y
herramientas comunitarias en `compatibilitytools.d`. Sólo muestra tools
instalados; Bannerlator aparece cuando se instala allí. Proton 10 bajo Sniper
quedó detenido en `wineboot` y agotó el timeout de 180 s el 2026-09-28. Los
probes de cuadro limpio D3D11/D3D12 con Proton Experimental sí presentaron
imagen el 2026-09-28: 157.3 y 158.8 fps (600 cuadros cada uno), 2/2. No son
FPS de juego. Persisten avisos del selector FS de Wine; VKD3D/MoltenVK también
informó un compute shader sustituido por uno vacío durante D3D12. Log:
`~/SteamARM-roots/logs/proton-graphics-retest-20260928.log`.

## Clasificación de dependencias

| Clase | Elementos y criterio |
|---|---|
| `KEEP` | `runtime/` y `build/lxrun`; supervisor SwiftUI; FEX x86 adaptado a guest-base/x18/W^X; pantalla nativa; audio/mandos; MoltenVK 1.4.2. Mantener la ruta funcional mientras se migra. |
| `REPLACE_WITH_HOLO` | Fedora ARM64 de `mkroot-rpm.sh` y `mkarmroot.sh`; libraries/loader ARM64 de los dos roots. Sustituir sólo después de validar loader, glibc, pthread, `dlopen`, FEX y fallback. |
| `GUEST_X86_REQUIRED` | Steam x86/x86-64, Ubuntu x86-64 base, Proton x86 y ejecutables/juegos Windows x86/x86-64. FEX debe seguir traduciendo este código. |
| `GUEST_I386_REQUIRED` | Steam client i386, partes i386 de Proton/Wine y juegos/overlay que aún las requieran. Conservar FEX 32-bit y Vulkan thunk 32-bit. |
| `THUNK_CANDIDATE` | Vulkan guest-to-host hacia el shim/MoltenVK; medir por API, extensión y ABI antes de ampliar a OpenGL u otras bibliotecas. |
| `STEAM_RUNTIME_OWNED` | `SteamLinuxRuntime_4`, `_4-arm64`, `sniper`, `soldier` y manifests/payloads administrados por Steam. El launcher los detecta; no debe reemplazar sus archivos. |
| `REMOVE_LATER` | Constructor Fedora ARM y su root alternativo, sólo tras migrar Holo y demostrar fallback. No borrar aún. VNC queda como diagnóstico, no base gráfica. |
| `UNKNOWN` | Paridad de packages Holo frente a Steam Frame, ABI completa de SLR ARM64, semánticas pressure-vessel que sobreviven sin namespaces y necesidades i386 por juego. Resolver mediante auditoría binaria y smoke tests. |

## Proton ARM64: límite actual

Los tool-manifests ARM64 están instalados, pero `scripts/proton-command.py`
los rechaza deliberadamente; aparecen en el inventario con estado no compatible.
El Steam client en esta ruta es x86/i386 bajo FEX; Proton ARM64 requiere
procesos ELF Linux ARM64 y el runtime aún no puede reservar las direcciones
bajas que Wine necesita. Además, Darwin borra `x18` entre excepciones, mientras
ARM64EC usa ese registro para el TEB. El probe documentado en
`benchmarks/stage18-settings-audio-controllers.txt` falló en 2 s. Ocultar el
CPUID de FEX es necesario para Steam/pressure-vessel x86; mostrarlo hace que
Steam seleccione ARM64 y rompe el camino x86 medido. El selector sólo habilita
tools x86 que están instalados. Una prueba local el 2026-09-28 redujo
`__PAGEZERO` a 16 KiB, pero `MAP_FIXED` en `0x7ffe0000` terminó en `SIGKILL`
(exit 137); cambiar sólo el tamaño del segmento no elimina el bloqueo de baja
dirección.

La corrección de `arch_prctl(ARCH_GET_FS/GS)` de esta auditoría ya está
versionada como `patches/fex-lxrt-arch-prctl.patch` y se instala en ambos
roots mediante `scripts/install-fex-host.sh`.

## Puntos de entrada de auditoría

- `docs/ARCHITECTURE.md`: arquitectura y límites de `lxrun`/FEX.
- `scripts/run-app.sh`, `scripts/run-fex.sh`, `scripts/run-steam.sh`: lifecycle.
- `scripts/mkroot-rpm.sh`, `scripts/mkarmroot.sh`, `scripts/mksteamroot.sh`:
  creación de roots actuales.
- `scripts/fetch-x86-rootfs.sh`: Ubuntu/FEX x86 root y fixups.
- `scripts/proton-command.py`: Proton por app, selección de SLR y prefijos.
- `scripts/build-fex-host.sh`, `scripts/install-fex-host.sh`: build e instalación
  reproducibles de FEX con rollback binario.
- `scripts/install-steamroot-gfx.sh`: Vulkan thunks/shim y MoltenVK.
