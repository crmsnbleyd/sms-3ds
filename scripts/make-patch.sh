#!/usr/bin/env bash
# scripts/make-patch.sh — turn edited working copies into a patch under patches/<kind>/.
#
#   scripts/make-patch.sh edit   port|decomp <rel-path>...      copy the current base of each file to work/<kind>/<rel-path>
#   scripts/make-patch.sh diff   port|decomp <name> <rel-path>... write patches/<kind>/<name>.patch from work/<kind>/
#
# Bases (never edited): port files come from refs/sms-pc-port/<rel-path>; decomp files from the
# build tree's upstream-patched copy (build-3ds/patched/<rel-path>) when one exists, else
# refs/sms-pc-port/decomp/<rel-path> — our decomp patches apply on top of sms-pc-port's.
# A file already patched by one of our own patches must be edited from that patched copy: pass
# the existing patch's name to `diff` again to regenerate it rather than stacking a second patch.
# work/ is gitignored.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
REFS="$ROOT/refs/sms-pc-port"
BUILD=${SMS_BUILD_DIR:-$ROOT/build-3ds}
cmd=${1:?usage}; kind=${2:?usage}; shift 2

base_of() {
  local rel=$1
  case "$kind" in
    port) echo "$REFS/$rel" ;;
    decomp)
      # the decomp file with only sms-pc-port's patches applied (their hunks for this file, in order)
      local tmp="$ROOT/work/.base/decomp"
      rm -rf "$tmp"; mkdir -p "$tmp/$(dirname "$rel")"
      [[ -f "$REFS/decomp/$rel" ]] && cp "$REFS/decomp/$rel" "$tmp/$rel"
      for p in $(ls "$REFS"/decomp-patches/*.patch | sort); do
        python3 -I - "$p" "$rel" > "$tmp/one.patch" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="utf-8", errors="surrogateescape").read().splitlines(keepends=True)
want, out, keep = sys.argv[2], [], False
for i, line in enumerate(text):
    if line.startswith("--- ") and i + 1 < len(text) and text[i + 1].startswith("+++ "):
        keep = re.match(r"\+\+\+ b/(\S+)", text[i + 1]) is not None and re.match(r"\+\+\+ b/(\S+)", text[i + 1]).group(1) == want
    elif line.startswith("diff "):
        keep = False
        continue
    if keep:
        out.append(line)
sys.stdout.write("".join(out))
PY
        if [[ -s "$tmp/one.patch" ]]; then
          patch -p1 --quiet --force -d "$tmp" -i "$tmp/one.patch" >/dev/null
        fi
      done
      echo "$tmp/$rel" ;;
    *) echo "kind must be port or decomp" >&2; exit 2 ;;
  esac
}

case "$cmd" in
  edit)
    for rel in "$@"; do
      src=$(base_of "$rel")
      [[ -f "$src" ]] || { echo "no such file: $src" >&2; exit 1; }
      mkdir -p "$ROOT/work/$kind/$(dirname "$rel")"
      cp "$src" "$ROOT/work/$kind/$rel"
      echo "work/$kind/$rel  (base: ${src#$ROOT/})"
    done ;;
  diff)
    name=${1:?patch name}; shift
    out="$ROOT/patches/$kind/$name.patch"
    mkdir -p "$(dirname "$out")"
    : > "$out.tmp"
    for rel in "$@"; do
      diff -u --label "a/$rel" --label "b/$rel" "$(base_of "$rel")" "$ROOT/work/$kind/$rel" >> "$out.tmp" || true
    done
    [[ -s "$out.tmp" ]] || { rm -f "$out.tmp"; echo "no changes" >&2; exit 1; }
    mv "$out.tmp" "$out"
    echo "${out#$ROOT/}: $(grep -c '^@@' "$out") hunk(s)" ;;
  *) echo "usage: $0 edit|diff port|decomp ..." >&2; exit 2 ;;
esac
