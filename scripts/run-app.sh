#!/bin/sh
set -u
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR/.." || exit 1
APP_RUNNER="${STEAMARM_APP_RUNNER_BIN:-$PWD/build/steamarm-app-runner}"
if [ ! -x "$APP_RUNNER" ]; then
    echo "run-app: Rust app runner missing at $APP_RUNNER (run make app-runner)" >&2
    exit 127
fi
exec "$APP_RUNNER" "$@"
