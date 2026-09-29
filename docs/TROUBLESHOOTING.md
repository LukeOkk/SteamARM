# Troubleshooting

Logs are in `~/SteamARM-roots/logs` (**Configuración → Registros → Abrir
carpeta de registros**). Steam's log is `steam-<date>-<time>.log` (one per start); `safeguard.log`,
`pulseaudio.log` and `inputd.log` belong to the memory guard, sound and
controllers. Setup output stays in the
Terminal window.

## Installing

**"SteamARM can't be opened" / "Apple could not verify…"**
The app is signed ad hoc. Use **System Settings → Privacy & Security → Open
Anyway**, or run
`xattr -dr com.apple.quarantine /Applications/SteamARM.app`.

**Setup: "Xcode Command Line Tools missing"**
Run `xcode-select --install`, wait for it to finish, then click **Instalar**
again.

**Setup: "Homebrew missing"**
Install it from <https://brew.sh> (the Terminal window shows the command),
then click **Instalar** again.

**Setup: "less than 25 GB free"**
Free space in your home folder. The Linux roots, build trees and Steam need
about 25 GB before any game.

**Setup stopped halfway**
Click **Instalar** again, or run `scripts/setup.sh` in the source folder.
Finished steps are skipped. To redo one step: `scripts/setup.sh <step>`
(`--list` shows them).

## Steam

**Steam's window doesn't appear**
The first start updates the client and can take several minutes. Watch
the newest `~/SteamARM-roots/logs/steam-*.log`. If Steam exits right away, stop it from
the launcher (**Detener**) and start it again.

**Steam uses "Proton ARM64" / games don't start**
This applies to the x86 Steam client, the route the launcher uses today.
That client must see an x86-64 machine: if it notices FEX, it switches to
ARM64 tools that cannot run here. SteamARM hides FEX's identifying CPUID
leaves from the `steam` executable for that. If you changed FEX options by
hand, go back to the defaults. (Valve's native arm64 client, still
experimental, sees an arm64 machine by design; this setting does not apply
to it. See `docs/STEAM_ARM64_BRINGUP.md`.)

**Why aren't all the ARM64 tools selectable?**
The ARM64 Proton and FEX downloads can be installed without being runnable
by this macOS runtime. Valve's ARM64 Proton requires native ARM64 Steam;
Valve explicitly excludes an x86 Steam client running through FEX. Native
Wine ARM64EC also needs low-address mappings and Windows' x18 TEB convention
that this runtime does not yet implement. The current working route uses
the macOS-patched FEX and x86 Proton, with no VM. Letting the x86 client see
FEX's CPUID leaves only makes it pick those ARM64 tools; it does not make
them run. There is no virtual-machine option to fall back on.

**Procesador → Compatibilidad instalada** lists the installed tools and their
status. `python3 scripts/compat-status.py` provides the same inventory and
scans both `steamapps/common` and Steam's `compatibilitytools.d` folders. A
community tool such as Bannerlator appears after it is installed there; ARM64
tools remain listed as incompatible with this macOS runtime. This is an
installation check, not a promise that every game will run.

**Steam says "MoltenVK 0.2.2210"**
That is Steam decoding MoltenVK's decimal driver version `10402` as a Vulkan
bit-packed version. It corresponds to **MoltenVK 1.4.2**, not an older driver.
**Gráficos → MoltenVK instalado** queries the actual library version, using
the same library search order as the Vulkan shim. The raw driver version is
left intact. `tests/elf/run_vk_device.sh` verifies GPU submission and readback
through both x86-64 and i386 FEX thunks and logs the driver's own information.

**Launching after a reboot fails to find /tmp/lxrt-root**
The launch scripts now recreate their volatile root links before starting
guests, including direct `scripts/run-fex.sh` use.

**A Windows game doesn't start**
Games are not verified yet, so some won't run. In **Configuración →
Registros**, enable the Proton log, start the game again, and look at
`steam-<appid>.log` in `~/SteamARM-roots/steamroot/tmp/fexhome/` (Proton's
home folder). Set **Procesador → Orden de memoria (TSO)** back to
the full mode if you changed it, and **Sistema → esync/fsync** off.

## Performance

- **Procesador → Caché de traducción en disco** keeps FEX's translations
  between runs. It is experimental on macOS: turn it off if a game misbehaves.
- **Gráficos → Límite de FPS** and **Sistema → Sincronización vertical**
  reduce heat and stutter.
- **Gráficos → Mostrar Metal HUD** and the DXVK HUD show frame rate and GPU
  load.

## Memory

**"Steam terminó con la señal 9" / "El guardián de memoria detuvo Steam"**
The memory guard (`scripts/safeguard.sh`) stopped the Linux programs before
the Mac ran out of memory. The launcher now says so and gives the reason;
`~/SteamARM-roots/logs/safeguard.log` has a `STOP` or `KILL` line with it.
It checks every second and stops programs for two kinds of reason:

- **Memory pressure.** macOS reports critical memory pressure for 2 s, or
  free memory stays under 12 % for 3 s. Then it stops the largest program
  first (usually the game), and the rest only if the pressure lasts 5 s
  more (a `STOP largest guest` line). Up to 0.3.4 it stopped everything as
  soon as free memory read under 35 %, which on a 16 GB Mac happened with
  Steam alone and other apps open.
- **Hard limits**, whatever the free memory: all the Linux programs
  together use more than **Sistema → DRAM** (8 GB if the launcher has not
  written that limit yet), fseventsd uses more than 1500 MB, more than 80
  Linux processes run, or the kernel's VM objects or map entries pass
  1,500,000. Then it stops every Linux program at once, Steam included (a
  `KILL:` line with the limit).

Close other apps, or raise **Sistema → DRAM** (at most the maximum the list
offers). A signal 9 without such a line in `safeguard.log` came from
elsewhere.

## Sound

**No sound**
Check **Sonido** isn't muted, then run `scripts/audio.sh status`
in the source folder. It should say `running`. Sound follows the current
macOS output device; if you change the device while a game runs, restart
the game.

## Controllers

**The game doesn't see the controller**
1. The controller works on the Mac itself (**System Settings → Game
   Controllers**).
2. In **Configuración → Entrada**, the player has the controller selected
   (or none, to take the first free one), and the buttons light up on the
   drawing when pressed.
3. `scripts/input.sh status` shows `running` and an `event0` device.
4. Restart Steam after changing controller settings.

**A and B are swapped**
The player's type is **Nintendo Switch Pro**, which uses Nintendo's labels.
Choose an Xbox type for Xbox-style A/B.

**No vibration**
**Entrada → Vibración** must be enabled, and not every controller supports
rumble on macOS.

## Reporting a problem

Open an issue with:

- the macOS version and Mac model;
- the SteamARM version (**About**, or the .dmg name);
- the game;
- the relevant log.

Logs can contain your Steam user name and file paths. Remove anything
personal before posting.
