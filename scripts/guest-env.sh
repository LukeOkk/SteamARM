# Sourced by scripts/run-native.sh and scripts/run-steam-arm64.sh.
# guest_env_from_root ROOT: export the KEY=VALUE lines of ROOT/.lxrt-guest-env,
# the environment a root says its guests need (scripts/mkframeroot.sh writes
# one for the Steam Frame root: indirect GLX, as its Mesa has no software
# driver). A variable that is already set wins; '#' lines and anything that
# is not NAME=VALUE are skipped. No file, nothing done.
guest_env_from_root() {
    local f="$1/.lxrt-guest-env" kv k
    [ -f "$f" ] || return 0
    while IFS= read -r kv || [ -n "$kv" ]; do
        k=${kv%%=*}
        [[ "$kv" == *=* && "$k" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue
        [ -n "${!k+x}" ] || export "$kv"
    done < "$f"
}
