#!/usr/bin/env python3
# Drive the 3DS build in Azahar through its RPC server (Settings > Debug > Enable RPC server):
# launch it, read the HUD's counters from memory, take screenshots, press buttons.
#
#   uv run scripts/azahar.py launch [file.3dsx]   quit Azahar, start it on the build, wait for the game
#   uv run scripts/azahar.py quit
#   uv run scripts/azahar.py hud                  the bottom screen's counters, read from memory
#   uv run scripts/azahar.py shot [name]          screenshot of both screens -> local/shots/<name>.png
#   uv run scripts/azahar.py pad KEYS [SECONDS] [STICKX STICKY]
#                                                 hold buttons (A,B,X,Y,L,R,Z,START,UP,DOWN,LEFT,RIGHT,
#                                                 joined with +; "-" for none) and the circle pad
#                                                 (3DS units, rim ~156) for SECONDS (default 0.2)
#   uv run scripts/azahar.py wait-area AREA [TIMEOUT]   until the game is in that area (state 5)
#   uv run scripts/azahar.py log [N]              the last N lines of the game's log.txt
#
# Uses the AzaharRPC.py that ships with Azahar (AZAHAR_SCRIPTING, default ~/Downloads/azahar/scripting)
# and the symbols of build-3ds/sms_3ds.elf. Screenshots go to the gitignored local/shots/.
import os
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "build-3ds", "sms_3ds.elf")
DEFAULT_3DSX = os.path.join(ROOT, "build-3ds", "sms_3ds.3dsx")
APP = os.path.expanduser(os.environ.get("AZAHAR_APP", "~/Downloads/azahar/Azahar.app"))
SCRIPTING = os.path.expanduser(os.environ.get("AZAHAR_SCRIPTING", "~/Downloads/azahar/scripting"))
SDMC = os.path.expanduser("~/Library/Application Support/Azahar/sdmc/3ds/sms-3ds")
NM = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm"

KEYS = {"A": 1 << 0, "B": 1 << 1, "X": 1 << 2, "Y": 1 << 3, "L": 1 << 4, "R": 1 << 5, "Z": 1 << 6,
        "START": 1 << 7, "UP": 1 << 8, "DOWN": 1 << 9, "LEFT": 1 << 10, "RIGHT": 1 << 11}


def rpc():
    sys.path.insert(0, SCRIPTING)
    from AzaharRPC import AzaharRPC  # noqa: E402 (Azahar's own client)
    return AzaharRPC(timeout=0.5, retries=2)


_syms = None


def sym(name):
    global _syms
    if _syms is None:
        out = subprocess.run([NM, ELF], capture_output=True, text=True, check=True).stdout
        _syms = {}
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3:
                _syms[parts[2]] = int(parts[0], 16)
    if name not in _syms:
        raise SystemExit("no symbol %s in %s" % (name, ELF))
    return _syms[name]


def read(c, name, fmt):
    data = c.read_memory(sym(name), struct.calcsize(fmt))
    if data is None:
        raise SystemExit("cannot read %s (is the game running?)" % name)
    return struct.unpack("<" + fmt, data)


def alive(c):
    try:
        return c.perf_stats() is not None
    except (TimeoutError, OSError):
        return False


def cmd_quit():
    subprocess.run(["osascript", "-e", 'quit app "Azahar"'], capture_output=True)
    for _ in range(50):
        if subprocess.run(["pgrep", "-f", "Azahar.app/Contents/MacOS"], capture_output=True).returncode:
            return
        time.sleep(0.2)
    subprocess.run(["pkill", "-f", "Azahar.app/Contents/MacOS"])


def cmd_launch(path=DEFAULT_3DSX):
    cmd_quit()
    subprocess.run(["open", "-a", APP, "--args", os.path.abspath(path)], check=True)
    c = rpc()
    for _ in range(150):
        if alive(c):
            try:
                if read(c, "sms3ds_gx_frames", "I")[0] > 0:
                    print("running")
                    return
            except SystemExit:
                pass
        time.sleep(0.2)
    raise SystemExit("the game did not start drawing within 30 s")


def cmd_hud():
    c = rpc()
    f = lambda n: read(c, n, "f")[0]
    u = lambda n: read(c, n, "I")[0]
    i = lambda n: read(c, n, "i")[0]
    ph = read(c, "sms3ds_prof_phase_ms", "5f")
    rows = [
        ("state/area", "%d / %d" % (i("sms3ds_state"), i("sms3ds_area"))),
        ("frames", u("sms3ds_gx_frames")),
        ("frame ms", "%.1f" % f("sms3ds_prof_frame_ms")),
        ("cpu / gx wait / gx busy", "%.1f / %.1f / %.1f" % (f("sms3ds_prof_cpu_ms"), f("sms3ds_prof_gxwait_ms"),
                                                           f("sms3ds_prof_gxbusy_ms"))),
        ("render / xform / tex", "%.1f / %.1f / %.1f" % (f("sms3ds_prof_flush_ms"), f("sms3ds_prof_xform_ms"),
                                                        f("sms3ds_prof_tex_ms"))),
        ("beg / state / idx / dc / repl", "%.1f / %.1f / %.1f / %.1f / %.1f" % (
            f("sms3ds_prof_begin_ms"), f("sms3ds_prof_state_ms"), f("sms3ds_prof_idx_ms"),
            f("sms3ds_prof_draw_ms"), f("sms3ds_prof_replay_ms"))),
        ("dec / copy / wait", "%.1f / %.1f / %.1f" % (f("sms3ds_prof_decode_ms"), f("sms3ds_prof_copy_ms"),
                                                     f("sms3ds_prof_wait_ms"))),
        ("dl calls / dl ms / stream KiB", "%d / %.1f / %d" % (u("sms3ds_gx_dl_calls"), f("sms3ds_prof_dl_ms"),
                                                             u("sms3ds_gx_stream_kb"))),
        ("batches / verts", "%d / %d" % (u("sms3ds_gx_batches_per_frame"), u("sms3ds_gx_vertices_per_frame"))),
        ("fresh / replay verts", "%d / %d" % (u("sms3ds_gx_fresh_verts"), u("sms3ds_gx_replay_verts"))),
        ("hit / move / anim / draw / mix", " / ".join("%.1f" % x for x in ph)),
        ("approx why", "%05x" % u("sms3ds_gx_approx_why")),
    ]
    st = rpc().perf_stats()
    if st:
        rows.append(("azahar fps / speed", "%.1f / %.0f%%" % (st["game_fps"], st["emulation_speed"] * 100)))
    for k, v in rows:
        print("%-32s %s" % (k, v))


def cmd_shot(name=None):
    c = rpc()
    png = c.screenshot()
    d = os.path.join(ROOT, "local", "shots")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, (name or time.strftime("%H%M%S")) + ".png")
    with open(path, "wb") as fh:
        fh.write(png)
    print(path)


def cmd_pad(keys, seconds="0.2", sx="0", sy="0"):
    bits = 0
    for k in keys.split("+"):
        if k and k != "-":
            bits |= KEYS[k.upper()]
    c = rpc()
    addr = sym("sms3ds_remote_pad")
    c.write_memory(addr, struct.pack("<Ihhhh", bits, int(sx), int(sy), 0, 0))
    time.sleep(float(seconds))
    c.write_memory(addr, struct.pack("<Ihhhh", 0, 0, 0, 0, 0))


def cmd_wait_area(area, timeout="60"):
    c = rpc()
    end = time.time() + float(timeout)
    while time.time() < end:
        st, ar = read(c, "sms3ds_state", "i")[0], read(c, "sms3ds_area", "i")[0]
        if st == 5 and ar == int(area):
            print("in area", area)
            return
        time.sleep(0.5)
    raise SystemExit("not in area %s after %s s" % (area, timeout))


def cmd_log(n="30"):
    with open(os.path.join(SDMC, "log.txt"), errors="replace") as fh:
        print("".join(fh.readlines()[-int(n):]), end="")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__ or "see the comment at the top of scripts/azahar.py")
    cmd, args = sys.argv[1].replace("-", "_"), sys.argv[2:]
    fn = globals().get("cmd_" + cmd)
    if not fn:
        raise SystemExit("unknown command " + sys.argv[1])
    fn(*args)


if __name__ == "__main__":
    main()
