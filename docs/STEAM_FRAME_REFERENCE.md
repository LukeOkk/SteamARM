# Referencia oficial Steam Frame

Snapshot verificado el 2026-09-28 para auditar el stack ARM64 de Valve sin
arrancar el kernel de la imagen.

## Versión y fuente

- Steamworks describe Steam Frame como Snapdragon 8 Gen 3 ARM64 con SteamOS
  basado en Arch, y documenta Windows x86 por Proton+FEX y Android por Lepton:
  <https://partner.steamgames.com/doc/steamhardware/steamframe/compatibility>
- Valve anunció SteamOS 0.4.1 Beta para Steam Frame el 2026-09-25:
  <https://steamcommunity.com/ogg/4165890/announcements/detail/674006995886409326>
- El índice oficial de recovery consultado el 2026-09-28 aún muestra como
  recovery completa más reciente
  `steamframe-oobe-repair-20260922.5153644-0.3.0.img.bz2` (3.8 GiB,
  2026-09-22 22:57 UTC), junto con `.zip` y QDL:
  <https://steamdeck-images.steamos.cloud/recovery/>
- No había recovery 0.4.1 o superior en ese índice. El identificador
  `20260925.6191901` queda como community-observed, no como snapshot oficial
  confirmado por este sondeo.

## Artefacto local y hashes

La copia recibida del usuario coincide en nombre con el índice oficial.
`bzip2 -tv` terminó con `ok`. Valve no publica hash en el índice consultado;
los SHA-256 siguientes son mediciones locales, no checksums del proveedor.

| Artefacto | Tamaño | SHA-256 | Permisos |
|---|---:|---|---|
| Archivo comprimido en `~/Downloads/` | 3.8 GiB | `3a4a077f1b1f40688ab3279affcb56776bd97c54db1573e7c65fc52a97106676` | read-only |
| Imagen raw en `~/SteamARM-roots/reference/` | 7,516,192,768 bytes | `081a38c051e99c09db6ae91be994b67ef3347ea71f330d803ab861f0ba8cc654` | read-only |

## GPT y filesystems

Imagen raw: GUID Partition Table, sectores de 512 bytes. La partición de
datos principal es Btrfs; no es un rootfs que macOS monte de forma nativa.
Inspección con `gpt -r show`, `hdiutil imageinfo` y attach `-readonly -nomount`;
`diskutil info` confirmó `Media Read-Only: Yes`.

| Partición | Inicio (sector) | Sectores | Tamaño | Tipo observado |
|---|---:|---:|---:|---|
| `esp` | 34 | 524,288 | 256 MiB | FAT32 |
| `efi-A` | 524,322 | 131,072 | 64 MiB | FAT32 |
| `rootfs-A` | 655,394 | 10,485,760 | 5 GiB | Btrfs, etiqueta `rootfs-A`; 4,475,207,680/5,368,709,120 bytes usados |
| `var-A` | 11,141,154 | 524,288 | 256 MiB | ext4 |
| `home` | 11,665,442 | 204,800 | 100 MiB | ext4 |

No se arrancó la imagen, no se cargó su kernel, no se montó ningún filesystem
con escritura y no se ejecutó código del rootfs. `btrfs check` validó el
filesystem y sus checksums; `btrfs restore` no pudo descomprimir la mayoría de
los datos ZSTD, así que el inventario de paquetes y binarios sigue pendiente.
El detalle está en `STEAM_FRAME_ROOTFS_AUDIT.md`.

## Qué sirve de referencia

Steam Frame es la referencia primaria para el layout Linux ARM64 real y las
decisiones de arquitectura. Separar packages genéricos Arch ARM64 de firmware,
kernel, drivers, servicios del dispositivo y capas propietarias específicas
de Snapdragon/Frame. Un archivo presente en recovery no pasa automáticamente
a ser redistribuible o necesario en ZERO-VM.

## Investigación web del 2026-09-29

La investigación web del 2026-09-29 sobre canales, imágenes de recovery,
bundles de actualización y árboles de paquetes está en
`STEAM_FRAME_SNAPSHOT_2026-09-29.md` (en inglés). Sus datos son investigación
web con etiquetas de evidencia, no mediciones hechas en el Mac. Donde coinciden
con las mediciones de este archivo, esa página lo indica y las cita.
