# Phases

| Phase | Goal | Status |
| --- | --- | --- |
| 0 | macOS toolchain (devkitPro), reference repos, PC baseline build, recon (`docs/PORT-MAP.md`) | done 2026-10-21 |
| 1 | Headless boot: `sms_3ds.3dsx` runs the decomp + PC platform layer on 3DS with a null GX backend; the console shows the game advancing to a stage load | built 2026-10-21; awaiting first Azahar run |
| 2 | Renderer: citro3d backend behind `gx_internal.h`'s seam (replaces `platform3ds/gx_null_backend.cpp`); TEV → PICA200 combiners; texture conversion with a hash-keyed 3DS texture pack from the browser converter, runtime fallback | 2a geometry ✓, 2b textures ✓ (title + file select recognisable), 2c TEV built; 2d (EFB copies) next |
| 3 | Input (HID → PAD, C-stick on New 3DS) and audio (DSP mixer → ndsp; DSP-ADPCM is native) | — |
| 4 | Performance: 804 MHz, second core, hot loops (skinning, matrix maths, FIFO parsing) | — |
| 5 | Browser asset converter (reads the user's disc image locally, writes the 3DS texture pack) | — |

## Phase 1 design (see the approved plan for details)

- Builds from `refs/sms-pc-port` at the recorded commits; nothing from refs is committed.
- Patch sets, applied at configure time to copies in `build-3ds/`:
  sms-pc-port's `decomp-patches/` → `patches/decomp/` (ours, on top) → `patches/port/` (ours, on PC-port files).
  Make or edit one with `scripts/make-patch.sh`.
- Compiled from refs: all game units, the GX front end (`gx_api`, `gx_fifo`, `gx_vert`, GD), and the portable
  platform modules (os, vi, dvd, disc, ar, card, noaudio + DSP HLE, endian, misc, mtx, modhooks, sdk_stubs).
- `platform3ds/`: entry point, logging (Azahar debug log + `sdmc:/3ds/sms-3ds/log.txt` + on-screen tail), HUD
  thread on core 1, null GX backend, stubs for modules not ported yet (pad, savestate, HD movies).
- MEM1 is a 24 MiB heap block (3DS user space ends below 0x40000000), so the GX memory window is set to it and two
  decomp spots that assumed 0x80000000 are patched (`patches/decomp/3ds-01-mem1-anywhere.patch`).

## Phase 1 run log

1. Azahar refused the .3dsx: a relocation outside the image (`&jdsp + 0x80000000`). Fixed (patches/decomp/3ds-02);
   `scripts/check-3dsx.py` now runs after every build.
2. Booted to `SMS_main`, heaps, DSP HLE, memory card; 75 frames, then the GX parser lost sync and read far past its
   buffers (~920k unmapped reads; Azahar ran at 7%). Cause: ARM short enums. Fixed by building with `-fno-short-enums`.
   Also seen: `/AudioRes/Seqs/JaiArcS.hed` not found, followed by null reads in MSound/JAIBasic (check after the fix).

## Phase 2 run log

1. 2a: orientation right; untextured surfaces white (expected).
2. 2b: textured title and file select; textures upside down (PICA rows are bottom-up) -> fixed.
3. 2c: title correct; Airstrip reached by hand (Peach, Mario, HUD text). ~5 fps. Profile: frame 201 ms =
   CPU transform 141 + other flush 15 + game 38 (+ decode 3, tex 2, GPU wait 4). Fix: transform, lighting and
   texgen moved to the PICA vertex shader (gx.v.pica); the PICA reads the front end's packed vertices in place.
   Note: Azahar logs ConfigureNew3DSCPU as unimplemented, so it may run the CPU at 268 MHz, not 804 MHz.
   Open: noisy textures on some Airstrip surfaces; Mario grey; lines/points not drawn.

3. Enum fix confirmed: parser in sync, full speed, 468 frames. GX display lists and vertex arrays were then read
   ~128 MiB past MEM1 (the window base was added on top of OSCachedToPhysical's result). Fixed: identity window
   (patches/port/3ds-05). Minimal PADRead added.
4. With the GDB stub (Azahar, port 24689; `arm-none-eabi-gdb build-3ds/sms_3ds.elf`, `target remote :24689`):
   title passed with START; file select reached (area 15, TCardLoad progress 0x13). The file select is the 3D
   save blocks Mario jumps into, so the choice was made from GDB (`gpCardLoad->unkB0 = 0`, then `unk10 = 3` to
   confirm "start"). **The Delfino Airstrip (area 0) loaded and ran** (frames and primitive counts rising, main
   thread in per-frame lighting/TEV set-up). **M1 met.**
5. Open: ~920k unmapped reads per run from the seagull sound (TAnimalBase::perform -> MSRandPlay::startSeRandPlay):
   the static MSRandPlay list's links are corrupted by small u16-sized increments (e.g. 0x0838b294 -> 0x083bb294),
   so the walk reads through null and 0x8044xxxx pointers. Next: a GDB watchpoint on the first list link
   (0x838b294, deterministic) from boot to catch the writer. May also exist on the PC with SMS_NO_AUDIO, where
   such pointers land inside MEM1 at 0x80000000.

## Phase 1 open risks (checked on the first runs)

- FPSCR: **confirmed** — Horizon starts threads with FPSCR `0x03000000` (flush-to-zero and default-NaN on), unlike the
  Gekko. Not yet changed; revisit if float results differ (denormals become 0).
- Misaligned VFP loads from file data would raise data aborts on the ARM11.
- pthread behaviour on libctru: priorities, `pthread_cond_timedwait`, detached threads.
- Memory: MEM1 24 MiB + ARAM 16 MiB + ~6 MiB image + thread stacks need New 3DS application memory.
