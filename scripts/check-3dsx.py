"""scripts/check-3dsx.py <file.3dsx> — replay the 3DSX loader's relocation checks (Azahar's
src/core/loader/3dsx.cpp, as the Homebrew Launcher's): an absolute relocation's stored word must have
a zero top nibble, a relative one 0 or 1. Exits 1, naming the addresses, if any would be rejected
("Failed to load ROM (Error 1)"); map them back with arm-none-eabi-addr2line -f -e sms_3ds.elf <addr>."""
import struct, sys

data = open(sys.argv[1], "rb").read()
magic, hsize, rhsize, ver, flags, code, rodata, dsz, bss = struct.unpack_from("<4sHHIIIIII", data, 0)
assert magic == b"3DSX", magic
if "-v" in sys.argv: print(f"header {hsize} reloc_hdr {rhsize} code {code:#x} rodata {rodata:#x} data {dsz:#x} bss {bss:#x}")
pos = hsize
ntab = rhsize // 4
relhdr = []
for seg in range(3):
    relhdr.append(struct.unpack_from(f"<{ntab}I", data, pos))
    pos += rhsize
segsz = [code, rodata, dsz - bss]
segs = []
for s in segsz:
    segs.append(data[pos:pos + s]); pos += s
aligned = [(code + 0xFFF) & ~0xFFF, (rodata + 0xFFF) & ~0xFFF, (dsz + 0xFFF) & ~0xFFF]
base = 0x00100000
seg_base = [base, base + aligned[0], base + aligned[0] + aligned[1]]
bad = 0
for seg in range(3):
    words = segs[seg]
    nwords = len(words) // 4
    for t in range(ntab):
        n = relhdr[seg][t]
        if t >= 2:
            pos += n * 4; continue
        w = 0
        for i in range(n):
            skip, patch = struct.unpack_from("<HH", data, pos); pos += 4
            w += skip
            while patch > 0 and w < nwords:
                (orig,) = struct.unpack_from("<I", words, w * 4)
                sub = orig >> 28
                if (t == 0 and sub != 0) or (t == 1 and sub not in (0, 1)):
                    bad += 1
                    if bad <= 40:
                        print(f"reject: seg {seg} table {t} at {seg_base[seg] + w*4:#010x} word {orig:#010x}")
                w += 1; patch -= 1
print("rejections:", bad)
sys.exit(1 if bad else 0)
