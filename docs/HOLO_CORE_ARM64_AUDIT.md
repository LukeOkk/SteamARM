# Auditoría Holo Core ARM64

Consulta inicial: 2026-09-28. Holo Core es el candidato de userspace ARM64;
ningún kernel ni mecanismo de virtualización forma parte del runtime final.

## Fuentes y snapshot

- Fuente oficial: <https://gitlab.steamos.cloud/holo/holo-core-aarch64-preview>
- Repositorio oficial de paquetes: <https://holo-packages.steamos.cloud/holo-core-aarch64-preview/>
- Revisión clonada para lectura: `67f0d559c82cdc5c94317bad53ae45409720ef59`
  (commit `readme: fix name ordering`, 2026-06-10).
- El README fuente identifica la base como el estado de Arch del 2025-11-18,
  más cambios ARM64 y correcciones de licencia; lo declara technology preview,
  no sistema estable con garantías.
- El índice de paquetes presenta `core` y `extra` ARM64; la revisión visible
  más nueva fue `mash-20251118.3` (árbol listado en 2026-07-10). Conteo del
  índice: 258 entradas en `core` y 4,312 en `extra`, incluyendo bases de datos
  y metadatos `FILES`; no son conteos de firmas. Los `.sig` del paquete
  `btrfs-progs` y de `core.db` no estaban publicados (404). Guardar SHA-256 y
  licencia no equivale a verificar una firma. Verificar más reciente en cada
  auditoría.
- El README ofrece imágenes de contenedor `base` y `base-devel` para construir.
  Este proyecto no las usa: paquetes oficiales se descargarán y extraerán como
  archivos de userspace; cero VM, cero kernel invitado, cero contenedor runtime.

## Encaje en ZERO-VM

**Candidato:** filesystem ARM64 de Arch, glibc/loader, librerías y paquetes
necesarios para FEX, Steam ARM64 cuando proceda y dispatch Linux→Darwin.

**Excluir del root mínimo:** kernel/initramfs, system boot, systemd services
que requieran kernel completo, firmware y drivers Steam Frame, VR/dashboard,
dispositivo Snapdragon, y cualquier imagen arrancable. Mantener archivos de
licencia y manifests de package.

El repo de fuentes es una receta de packages, no el userspace instalado ni un
Steam Frame OS completo. Comparar su package DB/binarios con la recovery antes
de tomar versiones o archivos. No asumir que Proton ARM64, FEX upstream,
Gamescope ni `SteamLinuxRuntime_4-arm64` forman parte del repositorio Holo.

## Bootstrap mínimo por resolver

Resolver dependencias desde la base de packages de Holo; no copiar una lista
ad hoc sin metadatos de package DB. Primer conjunto a evaluar: `filesystem`,
`glibc`, `gcc-libs`, `bash`, `coreutils`, certificates, `zlib`/`zstd`,
`libx11`/XCB sólo si el smoke lo requiere. Conservar manifests, checksums y
licencias; verificar firma cuando el proveedor la publique. El root debe poder ejecutar un binario AArch64 trivial, cargar
glibc/pthread y `dlopen`, y ejecutar FEX x86-64 e i386 bajo `lxrun`.

## Diferencia conocida frente a Steam Frame

Steam Frame recovery observada es un disco de dispositivo con ESP FAT32,
`rootfs-A` Btrfs y `var-A`/`home` ext4; Holo es un repositorio Arch ARM64
genérico. La comparación real de packages, ELF, FEX, Proton ARM64, runtimes y
Gamescope sigue pendiente: `btrfs check` pasa, pero `btrfs restore` falla al
descomprimir extents ZSTD. Ver `STEAM_FRAME_ROOTFS_AUDIT.md`.

## Investigación web del 2026-09-29

Lo que la investigación web del 2026-09-29 encontró sobre
`holo-core-aarch64-preview` está en la sección 3 de
`STEAM_FRAME_SNAPSHOT_2026-09-29.md` (en inglés). Sus datos son investigación
web con etiquetas de evidencia, no mediciones hechas en el Mac. Donde el clon
y el índice de paquetes registrados aquí confirman un dato, esa página lo
indica y los cita.
