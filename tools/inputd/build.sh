#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
out=${STEAMARM_BUILD:-build}
mkdir -p "$out"
clang -std=c11 -Wall -Wextra -O2 -I/opt/homebrew/opt/sdl2/include/SDL2 tools/inputd/inputd.c -L/opt/homebrew/opt/sdl2/lib -lSDL2 -o "$out/steamarm-inputd"
