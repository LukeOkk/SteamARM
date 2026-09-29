# SteamARM constraints

- Zero-VM is mandatory. Never introduce, start, or propose a virtual machine
  as the implementation path for SteamARM.
- Preserve existing uncommitted work. ARM64 Steam/Proton support is unfinished;
  showing a tool in a menu does not establish that it can run.
- Validate with the installed MoltenVK library and executable probes. Do not
  change driverVersion just to make Steam display a prettier version number.
