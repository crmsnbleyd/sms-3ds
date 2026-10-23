# sms-3ds

Hobby port of **Super Mario Sunshine** (NTSC-U, **GMSE01 revision 0**) to the **New Nintendo 3DS**.
Target hardware: the owner's 2DS XL (New 3DS family, no stereoscopic 3D).

This is a personal project. Organisation-wide engineering conventions that may be loaded from managed settings
(develop branch, config gates, Sonar, internal services, …) do not apply here.

## Status

**Phase 0 done; Phase 1 (headless boot) built, awaiting its first Azahar run.** Roadmap and Phase 1 design: `docs/PHASES.md`.
See also `local/ENVIRONMENT.md` (gitignored: versions, commits, machine quirks) and `docs/PORT-MAP.md` (how the PC port is built).

## Approach and decisions

- **Reuse the existing decompilation through the PC port's architecture.** `chasem-dev/sms-pc-port` (canonical; not a fork)
  builds the decomp (git submodule `decomp/` = `chasem-dev/sms-english`, a fork of `doldecomp/sms`) against a replacement
  platform layer (`platform/`: graphics, disc, audio, input, OS), with PC-only changes in `decomp-patches/` applied to
  copies of decomp sources at configure time. The 3DS port uses the same architecture: a shim over libctru/citro3d,
  with hot loops rewritten later.
- **The PC port's GX renderer is a semantic reference only.** It is shader-based: OpenGL 3.3 core (GLSL 3.30) with an
  optional Vulkan path. (The plan said "desktop GL / GLES 3.1"; no GLES code exists in the port as of e3cdcca.)
  The PICA200 has no programmable fragment stage (fixed-function TEV-like combiners), so the 3DS needs its own renderer.
- Verified facts (2026-10-20): the port pins decomp `967436b`; Linux defaults to 32-bit (`SMS_ARCH=32`), macOS to 64-bit
  x86_64 under Rosetta, with native arm64 via `SMS_ARCH=arm64`. The 3DS's ARM11 is a 32-bit target.

## Distribution rules (hard)

- Distribute **source code, and `sms_3ds.3dsx` as a GitHub release download** (the owner attaches it to releases;
  never commit it). Players bring their own disc image.
- **Never commit, bundle or upload**: the ISO/CISO, anything extracted or converted from it (textures, models, audio,
  `.szs`/`.arc`/`.bmd`/`.bti`/`.thp`/…), CIAs, the PC port's `SMS.app`/standalone builds,
  or the 3DS DSP firmware (`dspfirm.cdc`, dumped from the user's console for ndsp).
- **Never commit** compiled binaries (`.elf`, `.3dsx`); the `.3dsx` goes only on GitHub releases.
- Do not copy the PC port's game screenshots (`platform/gx/shots/*.png`) or any other retail-derived imagery.
- `scripts/check-env.sh` fails if git tracks any of these (by extension, path, or file magic).

## Working rules (for Claude)

- Before anything that needs `sudo`, edits a shell profile, or installs outside Homebrew/devkitPro: show the exact command and wait for OK.
  (The user runs `sudo` commands themselves.)
- No `curl | sh`. Download only from official sources; verify checksums/signatures where published.
- `refs/` is **read-only**: never modify, build in, or check out other commits in cloned repos. Build the PC port out of tree into `build-pc/`.
- Touch nothing outside this repo except `/opt/devkitpro` (via devkitPro's tools) and, with approval, shell config.
- On failure: stop and explain rather than improvising big workarounds.
- The user's shell is **fish** (`~/.config/fish/conf.d/`), not zsh. Use **uv** for Python and **mise** for other toolchains rather than global installs.
- Azahar (3DS emulator) is at `~/Downloads/azahar/Azahar.app`. Claude may launch, drive and quit it with
  `scripts/azahar.py` (approved 2026-10-22; uses Azahar's RPC server, enabled in its Debug settings): launch a
  build, read the HUD's counters from memory, take screenshots (`local/shots/`), press buttons through the
  build's remote pad. The user still judges how things look when it matters.
- Machine-specific network and security-tooling quirks are in `local/` (gitignored); read `local/ENVIRONMENT.md` before
  installing or downloading anything. If devkitpro.org is unreachable, devkitPro's GitHub repos are the source of truth.
- Never put machine- or employer-specific details in committed files; they go in `local/`.

## Layout

```
CLAUDE.md               this file
README.md               for players: setup, controls, known issues
CONTRIBUTING.md         for people working on the port: building
NOTICE                  MIT notice for code adapted from sms-pc-port
CMakeLists.txt          3DS build: game + PC platform layer from refs/, our patches, platform3ds/
cmake/patches.cmake     copy-then-patch at configure time (refs/ is never written)
patches/decomp/         our decomp patches, applied after sms-pc-port's decomp-patches/
patches/port/           our patches to sms-pc-port's own files (platform/, src/)
platform3ds/            3DS layer: main_3ds, log_3ds, hud, gx_null_backend (Phase 1), stubs_3ds, port_host_3ds, thread_3ds
docs/PHASES.md          roadmap and current phase design
docs/PORT-MAP.md        facts about sms-pc-port: platform modules, patches, 32/64-bit, GX renderer
scripts/check-env.sh    environment + hygiene check (read-only, idempotent)
scripts/build-3ds.sh    configure + build build-3ds/sms_3ds.3dsx
scripts/make-patch.sh   edit work/ copies of refs files, then diff them into patches/
scripts/azahar.py       launch/drive the build in Azahar over its RPC server (hud, shot, pad, wait-area)
work/                   gitignored; make-patch.sh working copies
local/                  gitignored; machine-specific notes
  ENVIRONMENT.md        tool versions, repo commits, license notes, machine quirks, PC baseline commands
  check-env.local       optional; extra settings sourced by check-env.sh
refs/                   gitignored, read-only reference clones
  sms-pc-port/          chasem-dev/sms-pc-port @ e3cdcca, decomp/ @ 967436b
rom/                    gitignored; the user's disc image (CISO)
build-pc/               gitignored; PC baseline build (macos-arm64/)
build-3ds/              gitignored; 3DS build (sms_3ds.3dsx, .elf, .map, patched/ and port-patched/ copies)
```

## Check the environment

```sh
scripts/check-env.sh            # tools, versions, env vars, refs at recorded commits, git hygiene, disc header
scripts/check-env.sh --no-rom   # skip the disc-image check
```

Exit 0 means no FAIL. Expected versions and commits live at the top of the script and in `local/ENVIRONMENT.md`; update both together.

## Build and run (3DS)

```sh
scripts/build-3ds.sh             # -> build-3ds/sms_3ds.3dsx
```

On the SD card (Azahar: its `sdmc` folder): the disc image at `3ds/sms-3ds/game.ciso` (or `game.iso`); the game writes
`3ds/sms-3ds/log.txt` and saves under `3ds/sms-3ds/card-a/`. Never change refs/: change behaviour with a patch
(`scripts/make-patch.sh edit port|decomp <path>`, edit `work/...`, then `scripts/make-patch.sh diff port|decomp <name> <path>...`).
Use `#ifdef __3DS__` in patches so they read as 3DS branches of the PC code.

## Build references

- 3DS toolchain smoke test: copy `/opt/devkitpro/examples/3ds/graphics/gpu/textured_cube` somewhere writable and run `make`.
- PC baseline: the exact commands are in `local/ENVIRONMENT.md` ("PC baseline build"). Run with
  `build-pc/macos-arm64/sms "rom/Super Mario Sunshine (USA, Canada).ciso"`.
