# FEX runner

`steamarm-fex-runner` replaces the shell implementation of `scripts/run-fex.sh`.
The shell file is only a source-checkout compatibility entrypoint. FEX and the
Steam Frame root remain external runtime inputs; SteamARM owns the root linking,
environment setup, FEXServer lifecycle, and guest handoff.

The canonical host-side FEX directory is `/tmp/lxrt-root/usr/bin`, linked from
the Steam Frame SteamOS/Arch root. The x86 Steam root remains
`/tmp/lxrt-steamroot`. `LXRT_ROOT`, `FEX_ROOTFS`, `FEXDIR`, `FEXBIN`,
`FEX_SERVER_ROOTFS`, and `FEX_SERVER_LXRT_ROOT` keep their existing overrides.
