# Contributing

## Building

You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev` package group, plus CMake,
Ninja and Python 3. The build uses [sms-pc-port](https://github.com/chasem-dev/sms-pc-port) (with its `decomp`
submodule) checked out in `refs/sms-pc-port` at the commit recorded in `CMakeLists.txt`:

```sh
git clone --recurse-submodules https://github.com/chasem-dev/sms-pc-port refs/sms-pc-port
git -C refs/sms-pc-port checkout --recurse-submodules e3cdcca1d3a78f657d2d195c0db084bbe28c5a45
scripts/build-3ds.sh
```

The result is `build-3ds/sms_3ds.3dsx`, a debug build: it includes developer tools such as GX frame dumps,
which write to the SD card. For a release build without them, run `scripts/build-3ds.sh --release`; the
result is `build-3ds-release/sms_3ds.3dsx`. Both are built with the same optimisation.

## Checking the audio

A debug build meters what the 3DS DSP is given: `uv run scripts/azahar.py audio [SECONDS]` prints, once a
second, the loudest output sample (above 0 dBFS the DSP saturates it), and counts of samples past full scale,
of voice buffers that found their channel already played out (gaps), and of subframes with an echo. With
`SMS_AUDIO_DUMP=sdmc:/3ds/sms-3ds/mix.wav` in the SD card's `settings.txt` it also writes that mix (float,
before saturation) to the SD card; `uv run scripts/audio-levels.py` summarises the file.

## Settings and debug switches

`/3ds/sms-3ds/settings.txt` on the SD card holds `NAME = value` lines (sms-pc-port's settings file). The build
sets some variables itself before the file is read, and the file never overrides a variable that is already set.

Settings the file can change:

| Setting | Builds | Effect |
| --- | --- | --- |
| `SMS_SKIP_INTRO=1` | all | Skip the Nintendo and Dolby logos and the opening movie. |
| `SMS_WARP=STAGE,EPISODE` | all | Loading file A goes to that stage instead. Stages with no scene on the disc are ignored. |
| `SMS_AUDIO_SOFTMIX=0` | all | Play each voice on its own DSP channel instead of the CPU's stereo mix. The DSP's own mixing crinkles; this is for comparison. |
| `SMS_AUDIO_FX=0` | all | No echo (the FX lines), as on sms-pc-port. |
| `SMS_AUDIO_TRACE=1` | all | Log voice and channel counts every 2000 audio subframes. |
| `SMS_VI_HZ=RATE` | all | Override the retrace rate (benchmarking). |
| `SMS_AUDIO_DUMP=sdmc:/3ds/sms-3ds/mix.wav` | debug | Write the meter's mix (float, before saturation) to the SD card (see above). |
| `SMS_AUDIO_PREROLL_MS=MS` | debug | Silence before each new sound starts (default 40), so its queue outlasts the game's audio bursts. |

Set by the build, not changeable from the file: `SMS_NO_AUDIO` (1 without `/3ds/dspfirm.cdc`), `SMS_AUDIO=0`,
`SMS_SKIP_MOVIES=0`, `SMS_FRAME_RATE=30`, `SMS_SAVE_DIR`, `SMS_SETTINGS` and `SMS_DISC_ROOT` (`game.ciso`, else
`game.iso`).

Debug switches are variables written over Azahar's RPC server (`scripts/azahar.py`, which reads the build's symbols):

| Variable | Effect |
| --- | --- |
| `sms3ds_gx_dump_request` | 1: dump the next frame's GX state to `frame-dump-areaN.txt` (debug builds also dump 10 s into each area). |
| `sms3ds_gx_skip_image` | A texture address: draws sampling it are skipped (debug builds). |
| `sms3ds_grass_nocull` | 1: turn the grass's view culling off. |
| `sms3ds_talk_bottom_all` | 1: every talk window on the bottom screen, not only questions. |
| `sms3ds_remote_pad`, `sms3ds_remote_touch` | Buttons, sticks and touches from outside (`azahar.py pad`, `touch`). |

Controls on the console: L + R + X swaps the lower screen between the HUD and the debug display; START + SELECT quits.
