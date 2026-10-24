#!/usr/bin/env python3
# Drive the 3DS build in Azahar through its RPC server (Settings > Debug > Enable RPC server):
# launch it, read the HUD's counters from memory, take screenshots, press buttons.
#
#   uv run scripts/azahar.py launch [file.3dsx]   quit Azahar, start it on the build, wait for the game
#   uv run scripts/azahar.py quit
#   uv run scripts/azahar.py hud                  the bottom screen's counters, read from memory
#   uv run scripts/azahar.py shot [name]          screenshot of both screens -> local/shots/<name>.png
#   uv run scripts/azahar.py pad KEYS [SECONDS] [STICKX STICKY [CX CY]]
#                                                 hold buttons (A,B,X,Y,L,R,Z,START,UP,DOWN,LEFT,RIGHT,
#                                                 joined with +; "-" for none) and the circle pad
#                                                 (3DS units, rim ~156) for SECONDS (default 0.2);
#                                                 CX CY after the stick: the C-stick (camera)
#   uv run scripts/azahar.py wait-area AREA [TIMEOUT]   until the game is in that area (state 5)
#   uv run scripts/azahar.py touch X Y [SECONDS]  touch the bottom screen at pixel (X, Y)
#   uv run scripts/azahar.py plaza [AREA]         from the title screen to Delfino Plaza (file A), or to
#                                                 AREA when settings.txt on the SD card sets SMS_WARP
#   uv run scripts/azahar.py log [N]              the last N lines of the game's log.txt
#
# Uses the AzaharRPC.py that ships with Azahar (AZAHAR_SCRIPTING, default ~/Downloads/azahar/scripting)
# and the symbols of build-3ds/sms_3ds.elf (SMS3DS_ELF: another build's, when launching that one). Screenshots go to the gitignored local/shots/.
import os
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.environ.get("SMS3DS_ELF") or os.path.join(ROOT, "build-3ds", "sms_3ds.elf")  # symbols of the running build
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
    # SIGTERM rather than a polite quit: that one asks for confirmation (Azahar's own dialog)
    pat = "Azahar.app/Contents/MacOS"
    subprocess.run(["pkill", "-TERM", "-f", pat], capture_output=True)
    for _ in range(50):
        if subprocess.run(["pgrep", "-f", pat], capture_output=True).returncode:
            return
        time.sleep(0.2)
    subprocess.run(["pkill", "-KILL", "-f", pat])


def cmd_launch(path=DEFAULT_3DSX):
    cmd_quit()
    for attempt in range(10):  # right after a kill, LaunchServices may still refuse (error -600)
        if not subprocess.run(["open", "-a", APP, "--args", os.path.abspath(path)]).returncode:
            break
        time.sleep(1)
    else:
        raise SystemExit("could not open Azahar")
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
        ("pixel / vtx miss / unif / texbind / arena KiB", " / ".join(
            str(x) for x in read(c, "sms3ds_gx_count", "5I"))),
        ("write-back us / begin wait us", "%d / %d" % read(c, "sms3ds_gx_count", "8I")[6:]),
        ("write-back tiles re-encoded", read(c, "sms3ds_gx_count", "8I")[5]),
        ("tev compiles / xf-only set-ups", "%d / %d" % read(c, "sms3ds_gx_count", "10I")[8:]),
        ("audio: voices / channels / bufs / overruns / no-chan / ndsp frames / dropped / soft-mixed",
         " / ".join(str(x) for x in read(c, "sms3ds_audio_stats", "8I"))),
        ("gx waits us: queue / fence / boundary; fences", "%d / %d / %d; %d" % read(c, "sms3ds_gx_waits", "4I")),
        ("game HUD: batches / verts / us", "%d / %d / %d" % read(c, "sms3ds_gx_hud", "3I")),
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


def cmd_touch(x, y, seconds="0.2"):
    # a touch on the bottom screen at pixel (x, y) for `seconds` (hid_3ds.cpp sms3ds_remote_touch)
    c = rpc()
    addr = sym("sms3ds_remote_touch")
    c.write_memory(addr, struct.pack("<iii", int(x), int(y), 1))
    time.sleep(float(seconds))
    c.write_memory(addr, struct.pack("<iii", 0, 0, 0))


def cmd_pad(keys, seconds="0.2", sx="0", sy="0", cx="0", cy="0"):
    bits = 0
    for k in keys.split("+"):
        if k and k != "-":
            bits |= KEYS[k.upper()]
    c = rpc()
    addr = sym("sms3ds_remote_pad")
    c.write_memory(addr, struct.pack("<Ihhhh", bits, int(sx), int(sy), int(cx), int(cy)))
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


BLOCK_A_X = 846.0  # Mario's x under save block A at the file select (x grows to the right on screen)


def mario_pos(c):
    p = read(c, "gpMarioPos", "I")[0]
    data = c.read_memory(p, 12) if p else None
    return struct.unpack("<3f", data) if data else None


def cmd_plaza(area="1"):
    # title -> file A -> Delfino Plaza, past the "explanation again?" question and the flyover
    c = rpc()
    state = lambda: (read(c, "sms3ds_state", "i")[0], read(c, "sms3ds_area", "i")[0])
    cmd_wait_area(15, "60")
    # The title and the file select are one scene with Mario at the same spot: alternate START with
    # stick pushes (which also wake him when he lies asleep) until he walks, i.e. the file select is
    # up and he is controllable.
    for _ in range(60):
        if mario_pos(c):
            break
        time.sleep(0.5)
    x0 = mario_pos(c)[0]
    for i in range(40):
        if i % 3 == 0:
            cmd_pad("START")
            time.sleep(1.0)
        cmd_pad("-", "0.3", "-156", "0")
        time.sleep(0.5)
        if abs(mario_pos(c)[0] - x0) > 1:
            break
    else:
        raise SystemExit("Mario never became controllable at the file select")
    # walk under block A in short pushes, steering by his position
    for _ in range(60):
        dx = BLOCK_A_X - mario_pos(c)[0]
        if abs(dx) < 25:
            break
        cmd_pad("-", "0.1" if abs(dx) < 80 else "0.3", "156" if dx > 0 else "-156", "0")
        time.sleep(0.4)
    else:
        raise SystemExit("could not walk under block A (x %.0f)" % mario_pos(c)[0])
    time.sleep(0.5)
    cmd_pad("A")  # jump into the block: its menu, START highlighted
    time.sleep(2.5)
    cmd_pad("A")
    cmd_wait_area(int(area), "60")  # another area with SMS_WARP in the SD card's settings.txt
    for _ in range(12):  # "No" to the explanation question, then skip/ride out the flyover
        time.sleep(1.5)
        cmd_pad("A")
    print("in the plaza" if area == "1" else "in area " + area)


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
