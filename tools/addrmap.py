"""Find, in another version of a game's program, the addresses that match ones of the version already mapped.

usage: python -I tools/addrmap.py ref_ram.bin other_ram.bin code|data addr [addr...]
  ref_ram / other_ram: 2 MB RAM dumps (--ramdump) of the two versions at a comparable point (program loaded)
  code: addresses of instructions (hooks); data: addresses of variables

Code: the instructions around the address are compared with every place of the other program, ignoring what moves
between versions (call targets, upper halves and offsets of data addresses); the best place wins (score = differing
instructions out of the window). Data: each instruction of the reference program that reaches the variable
(lui + load / store / addiu) is found in the other program as above, and the address it reaches there is read back;
the most frequent answer wins.
"""
import struct
import sys
from collections import Counter

ref = open(sys.argv[1], 'rb').read()
oth = open(sys.argv[2], 'rb').read()
kind = sys.argv[3]
CODE_LO, CODE_HI = 0x10000, 0x100000
BEFORE, AFTER = 12, 20  # instructions in the window


def words(mem, lo, hi):
    return list(struct.unpack_from('<%dI' % ((hi - lo) // 4), mem, lo))


def norm(w):
    op = w >> 26
    if op in (2, 3):
        return op << 26
    if op == 0x0f:
        return w & 0xffff0000
    if op in (0x08, 0x09, 0x0a, 0x0b, 0x0d) or 0x20 <= op <= 0x2e or op in (0x32, 0x3a):
        return w & 0xffff0000
    return w


R = words(ref, CODE_LO, CODE_HI)
O = words(oth, CODE_LO, CODE_HI)
RN = [norm(w) for w in R]
ON = [norm(w) for w in O]
# index of the other program by 3 normalized instructions, to find candidates fast
idx = {}
for i in range(len(ON) - 3):
    idx.setdefault((ON[i], ON[i + 1], ON[i + 2]), []).append(i)


def map_code(addr):
    i0 = ((addr & 0x1fffff) - CODE_LO) // 4
    win = RN[i0 - BEFORE:i0 + AFTER]
    cands = Counter()
    for k in range(0, len(win) - 3, 2):
        for j in idx.get((win[k], win[k + 1], win[k + 2]), ()):
            cands[j - k] += 1
    best = None
    for s, _ in cands.most_common(40):
        if s < 0:
            continue
        diff = sum(1 for k in range(len(win)) if s + k >= len(ON) or ON[s + k] != win[k])
        # equal code in more than one place: the one nearest the reference address (versions move code a little)
        far = abs(s - (i0 - BEFORE))
        if best is None or (diff, far) < (best[1], best[2]):
            best = (s, diff, far)
    if best is None:
        return None, len(win)
    return 0x80000000 + CODE_LO + 4 * (best[0] + BEFORE), best[1]


def refs_to(mem_words, target):
    """instructions reaching target: lui rX, hi + (memory op / addiu) rX, lo within 8 instructions"""
    hi = ((target + 0x8000) >> 16) & 0xffff
    lo = target & 0xffff
    out = []
    for i, w in enumerate(mem_words):
        if w >> 26 == 0x0f and (w & 0xffff) == hi:
            rt = (w >> 16) & 31
            for j in range(i + 1, min(i + 9, len(mem_words))):
                x = mem_words[j]
                op = x >> 26
                if ((x >> 21) & 31) == rt and (op in (0x09,) or 0x20 <= op <= 0x2e) and (x & 0xffff) == lo:
                    out.append((i, j))
    return out


def reached(mem_words, i, j):
    hi = mem_words[i] & 0xffff
    lo = mem_words[j] & 0xffff
    return ((hi << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xffffffff


for a in sys.argv[4:]:
    addr = int(a, 16)
    if kind == 'code':
        m, d = map_code(addr)
        print('%08x -> %s (%d of %d instructions differ)' % (addr, '%08x' % m if m else '?', d, BEFORE + AFTER))
    else:
        votes = Counter()
        rs = refs_to(R, addr)
        for i, j in rs[:40]:
            m, d = map_code(0x80000000 + CODE_LO + 4 * j)
            if m is None or d > 8:
                continue
            jj = ((m & 0x1fffff) - CODE_LO) // 4
            ii = jj - (j - i)
            if 0 <= ii < len(O) and O[ii] >> 26 == 0x0f:
                votes[reached(O, ii, jj)] += 1
        best = votes.most_common(3)
        print('%08x -> %s  (%d references, votes %s)' % (addr, '%08x' % best[0][0] if best else '?', len(rs),
                                                         ', '.join('%08x:%d' % kv for kv in best)))
