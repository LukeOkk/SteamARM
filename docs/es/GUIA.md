# SteamARM: guía en español

SteamARM ejecuta el cliente de Steam para Linux y los juegos de Windows que
Steam lanza con Proton en un Mac con Apple Silicon, **sin máquina virtual**.
Los programas x86 se traducen con FEX. El runtime `lxrun` implementa Linux
sobre macOS. Los gráficos van de Direct3D a Vulkan (DXVK / VKD3D-Proton) y
de ahí a Metal (MoltenVK).

> **Experimental.** Steam funciona: tienda, biblioteca, descargas e inicio de
> sesión. Los programas de Windows de prueba corren con Direct3D 9/11/12,
> sonido y mandos. **Los juegos reales aún no están verificados.**

## Requisitos

- Mac con Apple Silicon (M1 o posterior) y macOS 14 o superior.
- Command Line Tools de Xcode (`xcode-select --install`) y
  [Homebrew](https://brew.sh).
- Unos 25 GB libres y entre 20 y 60 minutos para la primera instalación.

## Instalar

1. Descarga `SteamARM-<versión>-macOS-arm64.dmg` desde
   [Releases](https://github.com/LukeOkk/SteamARM/releases) y arrastra
   **SteamARM** a **Aplicaciones**.
2. La primera vez, macOS bloquea la app porque no está notarizada. Ábrela,
   cierra el aviso y ve a **Ajustes del Sistema → Privacidad y seguridad →
   Abrir igualmente**. También puedes quitar la cuarentena desde Terminal:
   `xattr -dr com.apple.quarantine /Applications/SteamARM.app`.
3. En SteamARM pulsa **Instalar**. Se abre Terminal con `scripts/setup.sh`,
   que:
   - instala con Homebrew lo que necesita;
   - compila el runtime, FEX, el servidor X y el servicio de mandos;
   - descarga los sistemas Linux y Steam;
   - hace unas pruebas rápidas.

   Si se corta, vuelve a pulsar **Instalar**: sigue donde quedó.
4. Pulsa **Steam**. El primer arranque actualiza el cliente (unos minutos) e
   inicia sesión.
5. En Steam, activa **Ajustes → Compatibilidad → Steam Play** para todos los
   títulos y elige **Proton Experimental**.

Las tarjetas **Steam ARM64 (experimental)** y **Steam ARM64 · Steam Frame
(experimental)** abren el cliente ARM64 nativo de Valve, sin FEX. Llega a
su ventana de inicio de sesión, pero el inicio de sesión, la biblioteca y
los juegos no están verificados, y Proton ARM64 no funciona en macOS.
**Instalar** no las prepara: cada tarjeta sale desactivada, con el motivo,
mientras falten su raíz o el cliente. Para jugar, usa **Steam**.

Con **+ → Heroic Games Launcher (ARM64, experimental)** se instala la
versión linux-arm64 de Heroic en la raíz ARM64, sin FEX: su ventana, sus
páginas y el cierre están medidos; el inicio de sesión en las tiendas, las
descargas y los juegos no están verificados, y Amazon Games aún no funciona
(`docs/HEROIC_INTEGRATION.md`).

Con **+ → Añadir APK (Android)** eliges un `.apk`: SteamARM muestra su
icono, nombre, paquete, versión, SDK, código nativo, permisos y firmas, y lo
instala en `~/SteamARM-roots/android/`. Una actualización conserva los datos
de la app y se rechaza si el APK nuevo está firmado con otro certificado o
es una versión anterior. **Abrir** arranca la sesión Android de SteamARM
(Android 11 x86-64 bajo FEX, sin VM, `scripts/android-session.py`): la app
se instala con el propio `pm install` de Android y se abre en una ventana de
macOS propia, con su nombre, dibujada por `steamarm-wlmac`. El primer
arranque tarda uno o dos minutos; los siguientes, alrededor de medio minuto.
Cerrar esa ventana, o **Detener**, termina la sesión. Funcionan el ratón, el
teclado (con la distribución del Mac: una tecla ñ escribe ñ), la red del Mac,
el sonido (también el comprimido, Ogg y similares), el portapapeles de
texto en los dos sentidos (lo copiado en el Mac se pega en Android y al
revés), las páginas web dentro de las apps (WebView), el almacenamiento
compartido (/sdcard) y las apps que ejecutan programas o abren una terminal:
Termux instala sus paquetes base, abre su shell y `apt update` / `apt install`
funcionan. Ejecuta apps sin código nativo y apps con código x86-64; la tarjeta
queda desactivada, y dice por qué, si su código nativo es solo arm64-v8a (el
runtime ARM64 de Android no arranca en macOS), solo ARM o x86 de 32 bits, o
si necesita un Android más nuevo que el 11. Los paquetes XAPK, APKS y APKM
se instalan con todas sus partes (XAPK medido con un paquete real; APKS y
APKM, con paquetes de prueba); AAB no (es un formato de publicación, no de
instalación). Con clic derecho en la tarjeta: **Información…**, **Abrir
carpeta de datos** y **Desinstalar…** (conservando o borrando sus datos).
Detalles: `docs/APK_SUPPORT.md`.

## Configuración

La ventana de configuración sigue el diseño de Ryujinx:

| Sección | Qué ajusta |
|---|---|
| Interfaz | Abrir Steam al iniciar, confirmar al detener, ventanas nativas o VNC |
| Entrada | Mandos por jugador (ver abajo) |
| Sistema | Idioma y zona horaria, sincronización vertical, **DRAM/VRAM**, sincronización de Proton (esync experimental; fsync y MSync desactivados), qué hacer si un ajuste no puede funcionar |
| Procesador | Opciones de FEX: caché de traducción (experimental), orden de memoria TSO, multibloque, detección de código automodificable, x87 reducido |
| Gráficos | Motor: MoltenVK (por defecto) o KosmicKrisp (experimental; solo si está instalado el Mesa de Homebrew y el shim instalado puede cargarlo). Caché de sombreadores, filtrado anisotrópico, límite de FPS, HUD de DXVK y de Metal |
| Runtime | Solo lectura: cada backend, su estado y el motivo, y lo detectado en este Mac |
| Sonido | Salida y volumen |
| Atajos | Captura (F8), detener la app, alternar el HUD de Metal |
| Registros | Registro de Proton, `WINEDEBUG`, niveles de DXVK y VKD3D |
| Depuración | Informes de fallos, trazas, depuración de Vulkan, variables de entorno |

**DRAM y VRAM**: el máximo deja memoria a macOS. Con 8 GB llega a 6; con
16 GB, a 12; con 64 GB, a 60. **Automático** usa ese máximo.

- La DRAM es el tope de todo lo que ejecuta SteamARM. El vigilante de
  memoria detiene los programas antes de que el Mac se quede sin memoria,
  por dos tipos de motivo:
  - **Presión de memoria**: presión crítica de macOS durante 2 s, o menos
    del 12 % libre durante 3 s. Primero detiene el programa más grande
    (normalmente el juego); el resto solo si la presión sigue 5 s después.
    Hasta la 0.3.4 lo detenía todo con menos del 35 % libre, y cerraba
    Steam en Macs de 16 GB con otras apps abiertas.
  - **Límites fijos**, aunque quede memoria libre: todos los programas
    Linux juntos pasan de la DRAM elegida (8 GB si el launcher aún no
    escribió ese límite), fseventsd pasa de 1500 MB, hay más de 80
    procesos Linux, o los objetos o entradas de mapa de memoria del kernel
    pasan de 1.500.000. Entonces detiene todos los programas Linux a la
    vez, Steam incluido.

  En los dos casos el launcher dice que fue él y por qué, en lugar de
  "terminó con la señal 9".
- La VRAM es la memoria de vídeo que ven los juegos.

## Mandos

Conecta el mando al Mac (USB o Bluetooth). En **Configuración → Entrada**
elige:

- **Jugador**;
- **Dispositivo**;
- **Tipo de mando**, que es la identidad que verá el juego.

Tipos disponibles:

- Xbox 360, One, Series X|S y Elite Series 2;
- DualShock 3 y 4, DualSense y DualSense Edge;
- Nintendo Switch Pro;
- Steam Controller (2015) y **Steam Controller (2026)**, con sus cuatro
  botones traseros (L4 R4 L5 R5), sus dos trackpads y el botón de acceso
  rápido.

El dibujo del centro muestra el diseño real del mando elegido y se ilumina
al pulsar. Haz clic en un botón del dibujo y pulsa el control que quieras
para reasignarlo. Al lado tienes:

- zonas muertas e inversión o rotación de sticks;
- umbral de gatillos;
- vibración (activa por defecto) e intensidad;
- color del LED;
- perfiles, e importación desde Ryujinx.

Consejos:

- **Xbox Series X|S** es la opción más compatible.
- Con el tipo **Switch Pro** los juegos usan las etiquetas de Nintendo: el
  botón inferior es **B**. Con un mando de otra marca, A y B quedan
  cambiados.
- Todavía no llegan a los juegos el giroscopio ni la posición del dedo en
  los trackpads.

## Desinstalar

```sh
rm -rf /Applications/SteamARM.app "$HOME/Library/Application Support/SteamARM" \
       ~/SteamARM-build ~/SteamARM-roots
```

`~/SteamARM-roots` contiene tus juegos instalados.

## Más información

- [INSTALL.md](../INSTALL.md): instalación completa.
- [USAGE.md](../USAGE.md): uso detallado.
- [ARCHITECTURE.md](../ARCHITECTURE.md): cómo funciona.
- [TROUBLESHOOTING.md](../TROUBLESHOOTING.md): problemas frecuentes.

Estos documentos están en inglés.
