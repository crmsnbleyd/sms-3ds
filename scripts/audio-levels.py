#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy==2.1.3"]
# ///
# Levels of a mix dumped by a debug build (SMS_AUDIO_DUMP=sdmc:/3ds/sms-3ds/mix.wav in settings.txt):
# float32 stereo, the sum of the voices before the DSP saturates it, so 1.0 is full scale.
#
#   uv run scripts/audio-levels.py [mix.wav]      default: Azahar's sdmc/3ds/sms-3ds/mix.wav
#
# Prints a line per second (peak dBFS, rms dBFS, samples past full scale) and the totals.
import os
import struct
import sys

import numpy as np

path = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
    "~/Library/Application Support/Azahar/sdmc/3ds/sms-3ds/mix.wav")
with open(path, "rb") as fh:
    head = fh.read(44)
    rate, = struct.unpack_from("<I", head, 24)
    data = np.frombuffer(fh.read(), dtype="<f4")
data = data[: len(data) // 2 * 2].reshape(-1, 2)
db = lambda x: 20 * np.log10(max(float(x), 1e-9))
print("%6s %8s %8s %8s" % ("s", "peak dB", "rms dB", "clipped"))
for i in range(0, len(data), rate):
    w = data[i:i + rate]
    a = np.abs(w)
    print("%6d %8.1f %8.1f %8d" % (i // rate, db(a.max()), db(np.sqrt((w ** 2).mean())), int((a > 1.0).sum())))
a = np.abs(data)
print("%.1f s: peak %.1f dBFS, rms %.1f dBFS, %d of %d samples past full scale (%.3f%%)" % (
    len(data) / rate, db(a.max()), db(np.sqrt((data ** 2).mean())), int((a > 1.0).sum()), a.size,
    100.0 * (a > 1.0).sum() / max(a.size, 1)))
