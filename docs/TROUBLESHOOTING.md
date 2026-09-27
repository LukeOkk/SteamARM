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

**Proton (ARM64) is listed / chosen and the game doesn't start**
Valve's ARM64 Protons run an ARM64 Wine that macOS cannot host: it needs the
low 4 GiB of memory and the x18 register, both reserved by macOS. Pick
**Proton Experimental** (x86) for the game in its Properties →
Compatibility. The ARM64 entries only appear when **Configuración →
Procesador → Mostrar Proton ARM64 en Steam** is on.

**Steam uses "Proton ARM64" / games don't start (old note)**
Steam must see an x86-64 machine. SteamARM hides FEX's hypervisor CPUID bit
for that. If you changed FEX options by hand, go back to the defaults.

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

**Programs stopped with "memory limit"**
The memory guard stopped them before the Mac ran out of memory. Close other
apps, or raise **Sistema → DRAM** (at most the maximum the list offers).

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
