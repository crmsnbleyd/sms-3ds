#!/usr/bin/env bash
# scripts/check-env.sh — verify the sms-3ds Phase 0 environment. Read-only and idempotent.
#
#   scripts/check-env.sh            all checks
#   scripts/check-env.sh --no-rom   skip the disc-image header check
#
# Exit status: 0 if no FAIL (WARNs allowed), 1 otherwise.
# Expected versions/commits are the ones recorded in local/ENVIRONMENT.md; update both together.
# Optional machine-specific settings (gitignored) are sourced from local/check-env.local:
#   DKP_EXTRA_CA_PATTERN=<text>   a string that must appear in pacman's CA bundle (e.g. an extra trusted CA's name)
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CHECK_ROM=1
DKP_EXTRA_CA_PATTERN=
# shellcheck source=/dev/null
[[ -f "$ROOT/local/check-env.local" ]] && source "$ROOT/local/check-env.local"
[[ "${1:-}" == --no-rom ]] && CHECK_ROM=0

# --- recorded state ----------------------------------------------------------
PORT_URL=https://github.com/chasem-dev/sms-pc-port
PORT_COMMIT=e3cdcca1d3a78f657d2d195c0db084bbe28c5a45
DECOMP_URL=https://github.com/chasem-dev/sms-english.git
DECOMP_COMMIT=967436b8c623834d38a57d34923775f45eca8861
DKP_EXPECTED=(
  "devkitARM r68-1"
  "devkitarm-gcc 16.1.0-1"
  "devkitarm-binutils 2.46.0-1"
  "devkitarm-newlib 4.6.0.20260123-5"
  "libctru 2.7.0-1"
  "citro3d 1.7.1-2"
  "citro2d 1.7.0-1"
  "picasso 2.7.2-3"
  "tex3ds 2.3.0-4"
  "3dstools 1.3.1-3"
  "general-tools 1.4.4-1"
  "devkitarm-rules 1.6.0-4"
  "3ds-examples 20240917-1"
)

# --- output helpers ----------------------------------------------------------
fails=0 warns=0
ok()   { printf '  \033[32mOK\033[0m    %s\n' "$*"; }
warn() { printf '  \033[33mWARN\033[0m  %s\n' "$*"; warns=$((warns + 1)); }
fail() { printf '  \033[31mFAIL\033[0m  %s\n' "$*"; fails=$((fails + 1)); }
section() { printf '\n%s\n' "$*"; }

# --- host tools --------------------------------------------------------------
section "Host tools"
for t in git python3 cmake make patch xxd; do
  if command -v "$t" >/dev/null 2>&1; then ok "$t: $("$t" --version 2>&1 | head -1)"; else fail "$t not found"; fi
done
LLVM_BIN=$(brew --prefix llvm 2>/dev/null)/bin
if [[ -x "$LLVM_BIN/clang++" ]]; then ok "Homebrew llvm (PC baseline): $("$LLVM_BIN/clang++" --version | head -1)"
else warn "Homebrew llvm not found (only needed for the PC baseline build: brew install llvm)"; fi

# --- devkitPro ---------------------------------------------------------------
section "devkitPro"
DKP=${DEVKITPRO:-}
if [[ "$DKP" == /opt/devkitpro ]]; then ok "DEVKITPRO=$DKP"
elif [[ -z "$DKP" ]]; then warn "DEVKITPRO unset in this shell (expected /opt/devkitpro; see ~/.config/fish/conf.d/devkitpro.fish)"; DKP=/opt/devkitpro
else warn "DEVKITPRO=$DKP (expected /opt/devkitpro)"; fi
if [[ "${DEVKITARM:-}" == "$DKP/devkitARM" ]]; then ok "DEVKITARM=$DEVKITARM"
else warn "DEVKITARM='${DEVKITARM:-}' (expected $DKP/devkitARM)"; fi
case ":$PATH:" in *":$DKP/tools/bin:"*) ok "\$DEVKITPRO/tools/bin on PATH" ;; *) warn "$DKP/tools/bin not on PATH" ;; esac

GCC="$DKP/devkitARM/bin/arm-none-eabi-gcc"
if [[ -x "$GCC" ]]; then ok "$("$GCC" --version | head -1)"; else fail "arm-none-eabi-gcc missing at $GCC"; fi
for t in picasso tex3ds 3dsxtool smdhtool; do
  [[ -x "$DKP/tools/bin/$t" ]] && ok "tools/bin/$t" || fail "tools/bin/$t missing"
done
if [[ -x "$DKP/tools/bin/3dslink" ]]; then ok "tools/bin/3dslink"
else warn "tools/bin/3dslink missing (see local/ENVIRONMENT.md)"; fi

if command -v dkp-pacman >/dev/null 2>&1; then
  for e in "${DKP_EXPECTED[@]}"; do
    pkg=${e%% *} want=${e#* }
    have=$(dkp-pacman -Q "$pkg" 2>/dev/null | awk '{print $2}')
    if [[ -z "$have" ]]; then fail "$pkg not installed (expected $want)"
    elif [[ "$have" == "$want" ]]; then ok "$pkg $have"
    else warn "$pkg $have (recorded $want; update local/ENVIRONMENT.md if intentional)"; fi
  done
else
  fail "dkp-pacman not found (/usr/local/bin/dkp-pacman)"
fi
CA="$DKP/pacman/etc/ssl/certs/cacert.pem"
if [[ -n "$DKP_EXTRA_CA_PATTERN" ]]; then
  if grep -q "$DKP_EXTRA_CA_PATTERN" "$CA" 2>/dev/null; then ok "pacman CA bundle includes the extra CA from local/check-env.local"
  else warn "pacman CA bundle lacks the extra CA from local/check-env.local: dkp-pacman downloads may fail TLS (see local/ENVIRONMENT.md)"; fi
fi

# --- reference repos ---------------------------------------------------------
section "Reference repos (refs/, read-only)"
P="$ROOT/refs/sms-pc-port"
if [[ -d "$P/.git" ]]; then
  head=$(git -C "$P" rev-parse HEAD)
  [[ "$head" == "$PORT_COMMIT" ]] && ok "sms-pc-port at ${head:0:7}" || warn "sms-pc-port at ${head:0:7} (recorded ${PORT_COMMIT:0:7})"
  url=$(git -C "$P" remote get-url origin)
  [[ "$url" == "$PORT_URL" || "$url" == "$PORT_URL.git" ]] && ok "origin $url" || warn "origin $url (expected $PORT_URL)"
  [[ -z "$(git -C "$P" status --porcelain)" ]] && ok "sms-pc-port worktree clean" || fail "sms-pc-port has local modifications (refs/ must stay read-only)"
  if [[ -d "$P/decomp/.git" || -f "$P/decomp/.git" ]]; then
    d=$(git -C "$P/decomp" rev-parse HEAD)
    [[ "$d" == "$DECOMP_COMMIT" ]] && ok "decomp at ${d:0:7}" || warn "decomp at ${d:0:7} (recorded ${DECOMP_COMMIT:0:7})"
    du=$(git -C "$P" config -f .gitmodules submodule.decomp.url)
    [[ "$du" == "$DECOMP_URL" ]] && ok "decomp url $du" || warn "decomp url $du (expected $DECOMP_URL)"
    [[ -z "$(git -C "$P/decomp" status --porcelain)" ]] && ok "decomp worktree clean" || fail "decomp has local modifications"
  else
    fail "decomp submodule not checked out (git -C refs/sms-pc-port submodule update --init)"
  fi
else
  fail "refs/sms-pc-port missing (git clone --recurse-submodules $PORT_URL refs/sms-pc-port)"
fi

# --- distribution hygiene ----------------------------------------------------
section "Git hygiene (no disc images or Nintendo-derived files tracked)"
cd "$ROOT"
bad_ext='\.(iso|gcm|ciso|gcz|rvz|wia|wbfs|nkit\.[a-z]+|cia|3dsx|3ds|cci|elf|smdh|szs|arc|rarc|bmd|bdl|bti|bck|bca|blk|btp|brk|btk|bpk|bmt|col|thp|afc|ast|aw|bms|bnr|dol|rel|t3x|bcstm|bcwav)$'
tracked=$( { git ls-files; git diff --cached --name-only; } | sort -u)
hits=$(printf '%s\n' "$tracked" | grep -iE "$bad_ext|^(refs|rom|local|build[^/]*)/" || true)
if [[ -n "$hits" ]]; then fail "forbidden paths tracked or staged:"; sed 's/^/          /' <<<"$hits"
else ok "no forbidden extensions or refs/ rom/ local/ build*/ paths tracked"; fi
# Content sniff: disc/CISO/Yaz0/RARC/THP/BMD magics in any tracked or staged file.
magic_hits=()
while IFS= read -r f; do
  [[ -f "$f" ]] || continue
  m=$(xxd -p -l 8 "$f" 2>/dev/null)
  case "$m" in
    4349534f*|59617a30*|52415243*|54485000*|4a334432*) magic_hits+=("$f") ;;
  esac
  if [[ $(wc -c <"$f") -ge 32 && "$(xxd -p -s 28 -l 4 "$f")" == c2339f3d ]]; then magic_hits+=("$f"); fi
done <<<"$tracked"
if ((${#magic_hits[@]})); then fail "files with Nintendo/disc magic tracked:"; printf '          %s\n' "${magic_hits[@]}"
else ok "no tracked file has CISO/disc/Yaz0/RARC/THP/J3D magic"; fi
for p in refs/x rom/x.iso build-pc/x local/x x.ciso x.3dsx x.cia; do
  git check-ignore -q "$p" || fail ".gitignore does not cover $p"
done
ok ".gitignore covers refs/ rom/ build*/ local/ *.ciso *.3dsx *.cia (spot check)"

# --- disc image (optional) ---------------------------------------------------
if ((CHECK_ROM)); then
  section "Disc image in rom/ (optional)"
  shopt -s nullglob
  imgs=("$ROOT"/rom/*.iso "$ROOT"/rom/*.gcm "$ROOT"/rom/*.ciso)
  if ((${#imgs[@]} == 0)); then warn "no .iso/.gcm/.ciso in rom/ (only needed for running the PC baseline)"
  else
    for img in "${imgs[@]}"; do
      off=0
      [[ "$(xxd -p -l 4 "$img")" == 4349534f ]] && off=$((0x8000))  # CISO: disc block 0 follows the 32 KiB map
      hdr=$(xxd -p -s $off -l 8 "$img")
      magic=$(xxd -p -s $((off + 0x1c)) -l 4 "$img")
      if [[ "$hdr" == 474d534530310000 && "$magic" == c2339f3d ]]; then ok "$(basename "$img"): GMSE01 disc 00 rev 00"
      else fail "$(basename "$img"): header $hdr magic $magic (expected GMSE01 disc 00 rev 00)"; fi
    done
  fi
fi

printf '\n%d FAIL, %d WARN\n' "$fails" "$warns"
((fails == 0))
