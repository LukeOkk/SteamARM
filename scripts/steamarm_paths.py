"""Compatibility bridge to the Rust path resolver in tools/settings-env."""
import os
from pathlib import Path
import subprocess

_ROOT = Path(os.environ.get("STEAMARM_PROJECT_ROOT", Path(__file__).resolve().parent.parent))
_BIN = None


def _settings_env():
    global _BIN
    if _BIN and os.access(_BIN, os.X_OK):
        return _BIN
    override = os.environ.get("STEAMARM_SETTINGS_ENV_BIN")
    candidates = [Path(override)] if override else []
    candidates.extend(
        [
            _ROOT / "build/steamarm-settings-env",
            _ROOT / "build/SteamARM.app/Contents/Resources/steamarm-settings-env",
        ]
    )
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            _BIN = candidate
            return _BIN
    try:
        subprocess.run(
            [
                "cargo",
                "build",
                "--offline",
                "--locked",
                "--release",
                "--manifest-path",
                str(_ROOT / "tools/settings-env/Cargo.toml"),
                "--target-dir",
                str(_ROOT / "build/rust-settings-env"),
            ],
            cwd=_ROOT,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise RuntimeError("Rust settings-env is unavailable; run make settings-env") from error
    _BIN = _ROOT / "build/rust-settings-env/release/steamarm-settings-env"
    if not os.access(_BIN, os.X_OK):
        raise RuntimeError("Rust settings-env build produced no executable")
    return _BIN


def _resolve(option):
    return subprocess.check_output([str(_settings_env()), option], text=True).rstrip("\n")


def state_dir():
    return _resolve("--state-dir")


def build_dir():
    return _resolve("--build-dir")
