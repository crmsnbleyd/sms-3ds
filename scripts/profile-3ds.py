#!/usr/bin/env python3
# Sampling profiler over Azahar's GDB stub (GDB remote protocol, no gdb needed): interrupts the
# game every few milliseconds of wall time, reads the stopped thread's registers and stack, and
# prints where the time goes.
#
#   uv run scripts/profile-3ds.py [--port 24689] [--samples 400] [--interval-ms 40] [--trigger FILE]
#
# Self time is by PC. Callers come from the link register and a scan of the top of the stack for
# return addresses (words that point just after a BL/BLX in the ELF's code), so "inclusive" can
# over-count functions whose stale return addresses linger on the stack; read it as a guide.
# With --trigger FILE, sampling starts once FILE exists (bring the game to the scene first).
import argparse
import bisect
import os
import collections
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = ROOT + "/build-3ds/sms_3ds.elf"
BIN = "/opt/devkitpro/devkitARM/bin/"


class Stub:
    def __init__(self, port):
        while True:  # Azahar listens once a game starts with the stub enabled
            try:
                self.s = socket.create_connection(("127.0.0.1", port))
                break
            except OSError:
                time.sleep(0.5)
        self.buf = b""

    def _read(self):
        while True:
            i = self.buf.find(b"$")
            if i >= 0:
                j = self.buf.find(b"#", i)
                if j >= 0 and len(self.buf) >= j + 3:
                    body = self.buf[i + 1 : j]
                    self.buf = self.buf[j + 3 :]
                    self.s.sendall(b"+")
                    return body.decode("latin-1")
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError("stub closed the connection")
            self.buf += chunk

    def cmd(self, data, reply=True):
        pkt = data.encode("latin-1")
        self.s.sendall(b"$" + pkt + b"#%02x" % (sum(pkt) & 0xFF))
        return self._read() if reply else None

    def interrupt(self):
        self.s.sendall(b"\x03")
        return self._read()

    def regs(self):
        h = self.cmd("g")
        return list(struct.unpack("<16I", bytes.fromhex(h[: 16 * 8])))

    def mem(self, addr, n):
        h = self.cmd("m%x,%x" % (addr, n))
        if h.startswith("E"):
            return b""
        return bytes.fromhex(h)


def load_symbols():
    out = subprocess.run([BIN + "arm-none-eabi-nm", "-C", "-n", "--defined-only", ELF], capture_output=True,
                         text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def load_text():
    out = subprocess.run([BIN + "arm-none-eabi-objdump", "-h", ELF], capture_output=True, text=True,
                         check=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 6 and f[1] == ".text":
            size, vma, off = int(f[2], 16), int(f[3], 16), int(f[5], 16)
            with open(ELF, "rb") as e:
                e.seek(off)
                return vma, e.read(size)
    raise SystemExit("no .text in " + ELF)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=24689)
    ap.add_argument("--samples", type=int, default=400)
    ap.add_argument("--interval-ms", type=float, default=40)
    ap.add_argument("--stack-words", type=int, default=256)
    ap.add_argument("--trigger", default="")
    ap.add_argument("--out", default="")
    a = ap.parse_args()

    addrs, names = load_symbols()
    tvma, text = load_text()

    def sym(pc):
        i = bisect.bisect_right(addrs, pc) - 1
        return names[i] if i >= 0 else "0x%08x" % pc

    def is_return(v):  # v - 4 holds a BL or BLX (ARM state)
        o = v - 4 - tvma
        if v & 3 or o < 0 or o + 4 > len(text):
            return False
        ins = struct.unpack_from("<I", text, o)[0]
        return (ins & 0x0F000000) == 0x0B000000 or (ins & 0xFE000000) == 0xFA000000 or \
               (ins & 0x0FFFFFF0) == 0x012FFF30

    st = Stub(a.port)
    st.s.sendall(b"+")
    st.cmd("?")
    st.cmd("c", reply=False)
    if a.trigger:
        print("connected; sampling starts when %s exists" % a.trigger, file=sys.stderr)
        while not os.path.exists(a.trigger):
            time.sleep(0.2)
    selfc, incl, raw = collections.Counter(), collections.Counter(), []
    for k in range(a.samples):
        time.sleep(a.interval_ms / 1000.0)
        st.interrupt()
        r = st.regs()
        pc, lr, sp = r[15], r[14], r[13]
        chain = [sym(pc)]
        if is_return(lr):
            chain.append(sym(lr - 4))
        data = st.mem(sp, 4 * a.stack_words)
        for (v,) in struct.iter_unpack("<I", data[: len(data) & ~3]):
            if is_return(v):
                chain.append(sym(v - 4))
        st.cmd("c", reply=False)
        selfc[chain[0]] += 1
        incl.update(set(chain))
        raw.append(chain)
        if k % 50 == 49:
            print("%d samples" % (k + 1), file=sys.stderr)
    st.interrupt()
    st.cmd("D")

    n = max(1, len(raw))
    print("\n%d samples\n\nself" % len(raw))
    for name, c in selfc.most_common(45):
        print("%5.1f%%  %s" % (100.0 * c / n, name))
    print("\ninclusive (stack scan)")
    for name, c in incl.most_common(70):
        print("%5.1f%%  %s" % (100.0 * c / n, name))
    if a.out:
        with open(a.out, "w") as f:
            for c in raw:
                f.write(" < ".join(c) + "\n")


if __name__ == "__main__":
    main()
