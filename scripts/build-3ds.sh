#!/usr/bin/env bash
# scripts/build-3ds.sh — configure (first time) and build build-3ds/sms_3ds.3dsx.
#   scripts/build-3ds.sh            incremental build
#   scripts/build-3ds.sh --clean    delete build-3ds/ first
# CMake checks that refs/sms-pc-port and its decomp are at the recorded commits.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}
B="$ROOT/build-3ds"
[[ "${1:-}" == --clean ]] && rm -rf "$B"
if [[ ! -f "$B/build.ninja" ]]; then
  cmake -G Ninja -S "$ROOT" -B "$B" -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake"
fi
cmake --build "$B"
"$DEVKITARM/bin/arm-none-eabi-size" "$B/sms_3ds.elf"
echo "Built $B/sms_3ds.3dsx (never commit or share it: it is built from game code)"
