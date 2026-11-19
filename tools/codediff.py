"""List the places where two versions of a game's program differ (beyond addresses that moved).

usage: python -I tools/codediff.py ref_ram.bin other_ram.bin [lo_hex hi_hex]
Walks the reference program in steps of 32 instructions, finds each step in the other one (as tools/addrmap.py) and
prints the instructions that differ after normalisation (call targets, upper halves and offsets ignored).
"""
import struct
import sys
sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0])
from mips import Ins, disasm  # noqa: E402

ref = open(sys.argv[1], 'rb').read()
oth = open(sys.argv[2], 'rb').read()
lo = int(sys.argv[3], 16) & 0x1fffff if len(sys.argv) > 3 else 0x10000
hi = int(sys.argv[4], 16) & 0x1fffff if len(sys.argv) > 4 else 0x90000
CODE_LO, CODE_HI = 0x10000, 0x100000


def norm(w):
    op = w >> 26
    if op in (2, 3):
        return op << 26
    if op == 0x0f or op in (0x08, 0x09, 0x0a, 0x0b, 0x0d) or 0x20 <= op <= 0x2e or op in (0x32, 0x3a):
        return w & 0xffff0000
    return w


R = list(struct.unpack_from('<%dI' % ((CODE_HI - CODE_LO) // 4), ref, CODE_LO))
O = list(struct.unpack_from('<%dI' % ((CODE_HI - CODE_LO) // 4), oth, CODE_LO))
RN = [norm(w) for w in R]
ON = [norm(w) for w in O]
shift = 0
for i in range((lo - CODE_LO) // 4, (hi - CODE_LO) // 4, 16):
    win = RN[i:i + 32]
    if not any(win):
        continue
    best = None
    for d in [shift] + [shift + k for k in range(-256, 257) if k]:
        j = i + d
        if j < 0 or j + 32 > len(ON):
            continue
        diff = sum(1 for k in range(32) if ON[j + k] != win[k])
        if best is None or diff < best[1]:
            best = (d, diff)
        if diff == 0:
            break
    d, diff = best
    if diff > 12:
        continue  # not found (different code): skipped
    shift = d
    for k in range(16):
        if RN[i + k] != ON[i + d + k]:
            ra = 0x80000000 + CODE_LO + 4 * (i + k)
            oa = 0x80000000 + CODE_LO + 4 * (i + d + k)
            print('%08x %-32s | %08x %s' % (ra, disasm(Ins(ra, R[i + k])), oa, disasm(Ins(oa, O[i + d + k]))))
