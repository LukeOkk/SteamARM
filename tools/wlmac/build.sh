#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build
/usr/bin/clang -fobjc-arc -O2 -Wall -Wextra -framework AppKit -framework QuartzCore \
    tools/wlmac/wlmac.m -o build/steamarm-wlmac
echo "build/steamarm-wlmac"
