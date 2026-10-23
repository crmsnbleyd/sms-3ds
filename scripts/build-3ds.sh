#!/usr/bin/env bash
# scripts/build-3ds.sh — configure (first time) and build sms_3ds.3dsx.
#   scripts/build-3ds.sh                      debug build in build-3ds/ (developer tools: GX frame dumps)
#   scripts/build-3ds.sh --release            release build in build-3ds-release/ (for GitHub releases)
#   scripts/build-3ds.sh [--release] --clean  delete that build directory first
# CMake checks that refs/sms-pc-port and its decomp are at the recorded commits.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}
B="$ROOT/build-3ds" DEBUG=ON CLEAN=
for arg in "$@"; do
  case "$arg" in
    --release) B="$ROOT/build-3ds-release" DEBUG=OFF ;;
    --clean) CLEAN=1 ;;
    *) echo "usage: $0 [--release] [--clean]" >&2; exit 2 ;;
  esac
done
[[ -n "$CLEAN" ]] && rm -rf "$B"
if [[ ! -f "$B/build.ninja" ]]; then
  cmake -G Ninja -S "$ROOT" -B "$B" -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" -DSMS_3DS_DEBUG=$DEBUG
fi
cmake --build "$B"
"$DEVKITARM/bin/arm-none-eabi-size" "$B/sms_3ds.elf"
if [[ $DEBUG == ON ]]; then
  echo "Built $B/sms_3ds.3dsx (debug build; never commit it)"
else
  echo "Built $B/sms_3ds.3dsx (release build; never commit it: it goes on GitHub releases)"
fi
