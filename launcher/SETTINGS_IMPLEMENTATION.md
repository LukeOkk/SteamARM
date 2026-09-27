# Configuración de SteamARM: entrega

## Mapeo de la especificación

| Archivo | Requisitos implementados |
| --- | --- |
| `launcher/Models.swift` | Todos los campos de LauncherSettings, valores por defecto y decodeIfPresent; conservación de campos anteriores; memoria automática y lista con el máximo físico; escritura JSON atómica con errores propagados para la ventana. |
| `launcher/SettingsView.swift` | Nueve secciones, sidebar con SF Symbols, tema oscuro, formularios agrupados, copia editable, Aplicar/Cancelar/Aceptar, restablecimiento protegido, aviso de reinicio, selector de proyecto, pantalla/VNC, variables de entorno, registros y nota de Accesibilidad. |
| `launcher/Controllers.swift` | Campos por jugador, migración ProController/Xbox, valores por defecto para JSON antiguo, sesión editable sin escritura anticipada, SDL/refresh, remapping, prueba de vibración con intensidad. |
| `launcher/ControllersView.swift` | Cuatro jugadores, dispositivos, diez tipos, perfiles JSON con cargar/guardar/eliminar, importación Ryujinx, controles laterales, inversiones, rotación, zonas muertas, rangos, umbral, vibración, color LED y movimiento según tipo. |
| `launcher/ControllerDrawing.swift` | Dibujo vectorial interactivo por familia, sticks asimétricos/simétricos, botones coloreados y glifos, touchpad/lightbar, DualSense de dos tonos, paneles Steam Controller, indicación de palancas y estado SDL en vivo. |
| `launcher/Hotkeys.swift` | Grabación con modificadores y Escape para borrar; F8 por defecto; monitor local/global; captura de la primera ventana visible ajena al launcher; carpeta Pictures/SteamARM; detener por LauncherModel; guardar alternancia de Metal HUD. |
| `launcher/LauncherModel.swift` | Confirmación de detención usando confirmStop y continuación por el camino existente run-app.sh --stop. |
| `launcher/SteamARMApp.swift` | Registro de atajos e inicio opcional de Steam usando launch(.steam), sin duplicar una sesión adoptada. |
| `launcher/tests/SettingsTests.swift` | Defaults, JSON antiguo, round trips, ejemplos de memoria, migración de mando y semántica Aplicar/Cancelar en estado temporal. |

La edición de mandos tampoco escribe controllers.json hasta Aplicar/Aceptar. Cancelar o cerrar descarta cambios posteriores al último Aplicar. Guardar/eliminar un perfil son operaciones explícitas sobre archivos de perfiles, independientes de Aplicar.

OpenGL, FSR y suavizado global permanecen deshabilitados con explicación. No se añadieron fuentes al Makefile porque ya usa wildcard de launcher/*.swift. No se modificaron runtime/, scripts/, shim/ ni Makefile durante esta tarea.

## Verificación

- `make launcher`: correcto; genera build/SteamARM.app. Sin nuevas advertencias Swift.
- Advertencia del entorno existente: SDL2 de Homebrew fue construido para macOS 27.0 y el launcher apunta a macOS 14.0.
- Ejecutable de pruebas: PASS (defaults, legacy decoding, round trips, memory ceilings, controller apply/cancel).
- `git diff --check -- launcher`: correcto.
- Dibujos de mandos: inspección visual de PNG generados fuera de pantalla con ImageRenderer; sin ScreenCaptureKit, ventanas, Steam ni juegos. No se probó hardware físico.

Reproducción de las pruebas desde la raíz:

```sh
swiftc -parse-as-library -swift-version 5 \
  -import-objc-header launcher/SDLShim.h \
  -Xcc -I/opt/homebrew/opt/sdl2/include/SDL2 \
  -L/opt/homebrew/opt/sdl2/lib -lSDL2-2.0.0 \
  launcher/Models.swift launcher/Controllers.swift \
  launcher/tests/SettingsTests.swift -o /tmp/steamarm-settings-tests
STEAMARM_STATE="$(mktemp -d /tmp/steamarm-settings-test-XXXXXX)" /tmp/steamarm-settings-tests
```

## Límites y conexiones externas pendientes

- La ventana persiste todos los ajustes. El entorno de juegos lo construye el scripts/settings-env.py existente; no se cambió ni se verificó mediante una sesión de juego.
- En el árbol inspeccionado no hay lector de controllers.json en runtime/, scripts/ o shim/. Por tanto la entrega de mandos a juegos, identidad USB, remapping, transformaciones, LED, movimiento y vibración solicitada por juegos dependen del servicio de entrada externo descrito en la especificación. La detección/prueba SDL local sí está conectada.
- Los paneles táctiles y palancas solo se detectan si SDL expone controles equivalentes; el dibujo no implementa un transporte de datos táctiles o sensores.
- guestFaults=false se guarda, pero scripts/run-steam.sh fuerza LXRT_GUEST_FAULTS=1 y la entrada Steam del script conserva ese valor. Hace falta corregir ese backend para que desactivar el control tenga efecto. Ese cambio está fuera del alcance autorizado.
- Metal HUD se alterna y guarda; el proceso ya abierto conserva su entorno hasta reiniciarse.
- Los atajos globales necesitan Accesibilidad y las capturas necesitan el permiso de grabación de pantalla. No se cambiaron permisos del sistema.
- No se probaron en hardware capturas, vibración, LED, sensores ni las diez identidades de mando.


## Dibujos de mandos y render reproducible

Los contornos por familia, acabados, símbolos y controles distinguen los diez modelos.
Series lleva textura y Share; Elite, anillos metálicos y P1–P4; Edge, Fn y palancas.
Switch usa A derecha/B abajo/X arriba/Y izquierda. Los LED usan el color del jugador
(o azul); los dos indicadores de posición permanecen debajo del dibujo.
Cada contorno asignable comparte geometría y escala con su región de clic, conserva
`pads.capturing = slot` y `pads.isActive(slot)`. El centro de cada stick asigna L3/R3;
su anillo asigna el eje. Guide/PS/Home, Share, Capture, mute y Fn son decorativos:
no tienen PadSlot. Solo P1/P2 asignan leftGrip/rightGrip; P3/P4 son decorativos.
Las cuatro direcciones grabadas del panel Steam conservan sus slots; el área restante
asigna leftPad. No se dibuja un stick derecho en Steam Controller.

El harness usa ImageRenderer para los mandos y NSHostingView.cacheDisplay para la página
completa (incluidos controles nativos, sin ventana ni lectura de pantalla). Genera diez PNG de 900×600, variantes compactas y de captura,
tres variantes LED y `controllers-view.png` (1500×1200): 34 PNG sin captura de pantalla.
Verifica cobertura de slots, límites, tamaños, variantes distintas, captura, LED,
posiciones A/B de Switch y ausencia de asignaciones duplicadas a las palancas Elite.
No sustituye pruebas físicas de entradas. La sesión de estado es temporal.

Desde la raíz, regenerar:

```sh
export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
export CLANG_MODULE_CACHE_PATH=/tmp/steamarm-module-cache
export SWIFT_MODULECACHE_PATH=/tmp/steamarm-module-cache
swiftc -target arm64-apple-macos14 -parse-as-library -swift-version 5 -warnings-as-errors \
  -import-objc-header launcher/SDLShim.h \
  -Xcc -I/opt/homebrew/opt/sdl2/include/SDL2 \
  -L/opt/homebrew/opt/sdl2/lib -lSDL2-2.0.0 \
  launcher/Models.swift launcher/Controllers.swift launcher/ControllerDrawing.swift \
  launcher/ControllersView.swift launcher/tests/RenderControllers.swift \
  -o /tmp/steamarm-render-controllers
STEAMARM_STATE="$(mktemp -d /tmp/steamarm-render-state-XXXXXX)" \
  /tmp/steamarm-render-controllers /tmp/steamarm-render/
```

El SDK 26.5 instalado evita el fallo de sandbox del plugin SwiftUIMacros del SDK 27;
el target sigue siendo macOS 14. No se cambian ajustes del sistema ni el Makefile.
Con esas variables, `make launcher LAUNCHER_BIN=launcher/tests/artifacts/SteamARM
LAUNCHER_APP=launcher/tests/artifacts/SteamARM.app CURDIR=.` verifica el target original,
sin insertar rutas personales en Info.plist. SettingsTests usa el comando anterior.
Permanece la advertencia de SDL2 construido para macOS 27; no se admiten warnings Swift.

PNG: `/tmp/steamarm-render/{xbox360,xboxone,xboxseries,xboxelite2,ds3,ds4,dualsense,
dualsenseedge,steamcontroller,switchpro}.png`, sus variantes `-compact` y `-capture`,
`{ds4,dualsense,dualsenseedge}-led.png` y `controllers-view.png`.

Verificación final: make launcher y SettingsTests correctos; 34 PNG generados e inspección
visual de los diez modelos y la página. Sin nuevas advertencias Swift. No se inició Steam
ni juegos, ni se hicieron commits.
