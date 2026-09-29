# Auditoría rootfs Steam Frame

Fecha de inicio: 2026-09-28. Fuente: recovery oficial Steam Frame
`20260922.5153644-0.3.0`. Base binaria abierta read-only; no arrancar kernel.

## Estado de extracción

La imagen raw y el archivo comprimido tienen hashes y permisos read-only
registrados en `STEAM_FRAME_REFERENCE.md`. GPT identifica `rootfs-A` como
Btrfs de 5 GiB, etiqueta `rootfs-A`, 4,475,207,680 bytes usados. FAT32 y ext4
fueron identificados por sus firmas. El disco se adjuntó con `hdiutil` en modo
`-readonly -nomount`; macOS reportó la partición read-only.

El host no monta Btrfs. `btrfs check --readonly --check-data-csum` bajo lxrun
leyó el filesystem completo y devolvió `found 4475207680 bytes used, no error
found`; no detectó errores de metadatos ni checksums de datos. No se arrancó
kernel, VM, `chroot` ni código del rootfs.

La extracción de lectura con `btrfs restore` sigue incompleta: btrfs-progs
6.17.1 ARM64, 6.6.3 x86-64 y 7.1 x86-64 informan `ZSTD frame incomplete`
para unos 115,000 archivos. El árbol resultante conserva nombres y directorios,
pero muchos archivos de datos quedaron vacíos y no sirve como fuente para
versiones, package DB, manifests ni inventario ELF. APFS además rechaza parte
de los propietarios y xattrs Linux. La comprobación Btrfs pasó, por lo que la
causa del fallo de descompresión no se atribuye a corrupción de la imagen; el
contenido interno permanece sin verificar. Los logs y herramientas de prueba
están fuera del repo en `~/SteamARM-roots/logs/` y
`~/SteamARM-roots/reference/tools/btrfs-progs/`.

## Inventario pendiente

Al extraer, registrar version/package metadata y `file`/ELF machine para:

- `/etc/os-release`, package DB, loader, glibc y librerías ARM64;
- Steam ARM64 y x86/i386 payloads;
- FEX, Proton ARM64/x86, Wine ARM64EC, tool-manifests y Steam Runtime ARM64;
- Gamescope, Vulkan loader/ICDs, Mesa/KosmicKrisp y piezas hardware-specific;
- packages reutilizables de Arch ARM64 frente a archivos ligados al dispositivo.

Cada archivo potencialmente reutilizable deberá tener package/licencia,
arquitectura, dependencia Linux, destino ZERO-VM y clasificación de inclusión.
La mera presencia en recovery no basta para copiarlo. La extracción actual no
permite afirmar que esos payloads o manifests estén disponibles.
