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
second, the loudest output sample (above 0 dBFS the DSP saturates it), and counts of samples past full scale
and of voice buffers that found their channel already played out (gaps). With
`SMS_AUDIO_DUMP=sdmc:/3ds/sms-3ds/mix.wav` in the SD card's `settings.txt` it also writes that mix (float,
before saturation) to the SD card; `uv run scripts/audio-levels.py` summarises the file.
