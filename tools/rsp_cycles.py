#!/usr/bin/env python3
"""Static RSP cycle estimate for k64rsp microcode (stalls + SU/VU dual issue).

    rsp_cycles.py build/ucode/kgfx.lst [--overlay NAME] [--from LABEL] [--to LABEL] [--detail] [--joined]

Walks each labelled straight-line segment of the listing (fall-through path) and models the
RSP pipeline the way RSPL's cost evaluator does (src/lib/optimizer/eval/evalCost.js,
Max Bebök, Apache-2.0; docs https://n64brew.dev/wiki/Reality_Signal_Processor/CPU_Pipeline):

  - a scalar instruction (incl. vector loads/stores, mtc2/mfc2) and a vector computational
    instruction can issue in the same cycle when they do not depend on each other, neither
    is a branch and the pair is not in a delay slot;
  - results are ready after a latency: vector ops / vector loads / mtc2 4 cycles, scalar
    loads 3, mfc0/mfc2/cfc2/ctc2 3; a consumer issued earlier stalls;
  - a store issued exactly two cycles after a load stalls one cycle;
  - a taken branch costs one bubble (every branch counts as taken here).

kestrel64 runs the RSP at one instruction per cycle (no stalls, no dual issue), so it only
shows instruction-count changes; this model is the yardstick for scheduling work.
"""
import argparse, re

# --- decode ---------------------------------------------------------------------------------
LOAD_S = {0x20: 'lb', 0x21: 'lh', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu'}
STORE_S = {0x28: 'sb', 0x29: 'sh', 0x2B: 'sw'}
VEC = lambda n: 32 + n


class Op:
    __slots__ = ('pc', 'word', 'src', 'dst', 'lat', 'vec', 'branch', 'mload', 'mstore', 'ctl', 'text', 'label')


def decode(pc, w):
    o = Op()
    o.pc, o.word = pc, w
    o.src, o.dst, o.lat, o.vec, o.branch, o.mload, o.mstore, o.ctl = [], [], 0, False, False, False, False, False
    op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    if w == 0:
        return o                                          # nop
    if op == 0:
        f = w & 63
        if f in (0, 2, 3):        o.src, o.dst = [rt], [rd]
        elif f in (4, 6, 7):      o.src, o.dst = [rt, rs], [rd]
        elif f == 8:              o.src, o.branch = [rs], True
        elif f == 9:              o.src, o.dst, o.branch = [rs], [rd], True
        elif f == 13:             pass                    # break
        else:                     o.src, o.dst = [rs, rt], [rd]
    elif op == 1:
        o.src, o.branch = [rs], True
        if rt & 0x10: o.dst = [31]
    elif op == 2:                 o.branch = True
    elif op == 3:                 o.branch, o.dst = True, [31]
    elif op in (4, 5):            o.src, o.branch = [rs, rt], True
    elif op in (6, 7):            o.src, o.branch = [rs], True
    elif 8 <= op <= 14:           o.src, o.dst = [rs], [rt]
    elif op == 15:                o.dst = [rt]
    elif op in LOAD_S:            o.src, o.dst, o.lat, o.mload = [rs], [rt], 3, True
    elif op in STORE_S:           o.src, o.mstore = [rs, rt], True
    elif op == 0x10:                                      # COP0
        o.mload = o.mstore = True
        if rs == 0: o.dst, o.lat = [rt], 3
        else:       o.src = [rt]
    elif op == 0x12:                                      # COP2
        if w & (1 << 25):
            vt, vs, vd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
            f = w & 63
            o.vec, o.lat, o.dst = True, 4, [VEC(vd)]
            if 0x30 <= f <= 0x37:   o.src = [VEC(vt)]                    # vrcp*/vrsq*/vmov: vs = element
            elif f == 0x1D:         o.src = []                           # vsar reads the accumulator
            else:                   o.src = [VEC(vs), VEC(vt)]
        else:
            o.mload = o.mstore = True
            if rs == 0:   o.src, o.dst, o.lat = [VEC(rd)], [rt], 3      # mfc2
            elif rs == 2: o.dst, o.lat, o.ctl = [rt], 3, True            # cfc2
            elif rs == 4: o.src, o.dst, o.lat = [rt], [VEC(rd)], 4       # mtc2
            elif rs == 6: o.src, o.lat, o.ctl = [rt], 3, True            # ctc2
    elif op == 0x32:              o.src, o.dst, o.lat, o.mload = [rs], [VEC(rt)], 4, True
    elif op == 0x3A:              o.src, o.mstore = [rs, VEC(rt)], True
    o.src = [r for r in o.src if r != 0]
    o.dst = [r for r in o.dst if r != 0]
    return o


def load_listing(path, overlay=None):
    ops, label, cur = [], '?', None
    for l in open(path, encoding='utf-8'):
        l = l.rstrip('\n')
        if l.startswith("; overlay"):     # overlays share slot addresses: the main image, or
            cur = l.split()[2]            # the one overlay asked for
            continue
        if cur != overlay:
            continue
        if l.endswith(':') and ' ' not in l:
            label = l[:-1]
            continue
        o = decode(int(l[:3], 16), int(l[4:12], 16))
        o.text, o.label = l[14:], label
        ops.append(o)
    return ops


# --- pipeline model (port of RSPL evalCost) --------------------------------------------------
def simulate(ops):
    """Returns (cycles, per-op issue cycle, per-op stall cycles, per-op paired flag)."""
    ready = [0] * 64          # remaining cycles until each register is readable
    load_mask = 0
    cycle = 0
    branch_step = 0
    issue, stall, paired = [0] * len(ops), [0] * len(ops), [False] * len(ops)
    pc, n = 0, 0

    def ticks(c):
        nonlocal cycle, load_mask
        for i in range(64):
            ready[i] -= c
        load_mask >>= c
        cycle += c

    while pc < len(ops):
        while True:
            before = cycle
            for k in range(n):
                o = ops[pc + k]
                for r in o.src:
                    if ready[r] > 0:
                        stall[pc + k] += ready[r]
                        ticks(ready[r])
                if (load_mask & 1) and o.mstore:
                    stall[pc + k] += 1
                    ticks(1)
            if before == cycle:
                break
        for k in range(n):
            o = ops[pc + k]
            if o.mload:
                load_mask |= 0b100
            branch_step >>= 1
            if not branch_step and o.branch:
                branch_step = 2
            if branch_step == 1:              # delay slot of a (taken) branch: one bubble
                ticks(1)
            issue[pc + k], paired[pc + k] = cycle, n == 2
            for r in o.dst:
                ready[r] = o.lat
        pc += n
        if pc >= len(ops):
            break
        a = ops[pc]
        b = ops[pc + 1] if pc + 1 < len(ops) else None
        dual = (b is not None and a.vec != b.vec and branch_step != 2 and not a.branch and not b.branch
                and not (set(a.dst) & set(b.src)) and not (set(a.dst) & set(b.dst))
                and not (b.ctl and a.dst))
        n = 2 if dual else 1
        ticks(1)
    return cycle, issue, stall, paired


def segments(ops, lo=None, hi=None):
    seg, cur = [], []
    for o in ops:
        if cur and o.label != cur[-1].label:
            seg.append(cur)
            cur = []
        cur.append(o)
    if cur:
        seg.append(cur)
    names = [s[0].label for s in seg]
    a = names.index(lo) if lo else 0
    b = names.index(hi) + 1 if hi else len(seg)
    return seg[a:b]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('listing')
    ap.add_argument('--from', dest='lo')
    ap.add_argument('--to', dest='hi')
    ap.add_argument('--detail', action='store_true', help='per-instruction issue cycle / stalls / pairing')
    ap.add_argument('--overlay', help='walk this overlay (clip, audio, part, ...) instead of the main image')
    ap.add_argument('--joined', action='store_true', help='simulate the selected segments as one fall-through run')
    a = ap.parse_args()
    ops = load_listing(a.listing, a.overlay)
    segs = segments(ops, a.lo, a.hi)
    if a.joined:
        segs = [[o for s in segs for o in s]]
    tot_i = tot_c = 0
    print('%-16s %6s %7s %6s %6s %6s' % ('segment', 'insns', 'cycles', 'stall', 'pairs', 'ipc'))
    for s in segs:
        cyc, issue, stall, paired = simulate(s)
        cyc += 1
        tot_i += len(s); tot_c += cyc
        print('%-16s %6d %7d %6d %6d %6.2f' % (s[0].label, len(s), cyc, sum(stall), sum(paired) // 2, len(s) / cyc))
        if a.detail:
            for o, c, st, p in zip(s, issue, stall, paired):
                print('    %4d %s %s %03X  %s' % (c, ('+%d' % st).rjust(3) if st else '   ', '=' if p else ' ', o.pc, o.text))
    print('%-16s %6d %7d %6s %6s %6.2f' % ('total', tot_i, tot_c, '', '', tot_i / max(tot_c, 1)))


if __name__ == '__main__':
    main()
