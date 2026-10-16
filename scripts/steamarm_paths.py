"""Where SteamARM keeps its state (roots, Steam, settings, logs) and its build trees.

$STEAMARM_STATE and $STEAMARM_BUILD win. Otherwise ~/Developer/SteamARM-roots
(and ~/Developer/SteamARM-build) when it exists, else ~/SteamARM-roots (older
installs), else ~/Developer/SteamARM-roots for a new install. The shell scripts
and the launcher apply the same rule.
"""
import os


def _resolve(var, name):
    v = os.environ.get(var)
    if v:
        return os.path.expanduser(v)
    newer = os.path.expanduser("~/Developer/" + name)
    older = os.path.expanduser("~/" + name)
    if os.path.isdir(newer) or not os.path.isdir(older):
        return newer
    return older


def state_dir():
    return _resolve("STEAMARM_STATE", "SteamARM-roots")


def build_dir():
    return _resolve("STEAMARM_BUILD", "SteamARM-build")
