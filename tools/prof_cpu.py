#!/usr/bin/env python3
"""CPU profile of a ROM running in kestrel64, per function (local dev helper).

    prof_cpu.py rom.z64 rom.elf [--warmup 3] [--secs 5] [--top 30] [--pad cright] [--func name]

Samples the CPU PC with kestrel64's profiler (`prof.cpu`, physical PC in 16-byte buckets)
and attributes each bucket to the ELF function containing it (llvm-nm, with sizes). A PC
outside every function (code generated at run time on the heap, e.g. a translator's buffer)
counts as '?', and --unknown lists those buckets. The emulator is stopped by PID.
"""
import argparse, bisect, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prof_rsp import Tel, EXE


def symbols(elf):
    out = subprocess.run(['llvm-nm', '-n', '-S', '--defined-only', elf], capture_output=True, text=True).stdout
    syms = []
    for l in out.splitlines():
        p = l.split()
        if len(p) == 4 and p[2] in 'tTwW':
            syms.append((int(p[0], 16) & 0x1FFFFFFF, int(p[1], 16), p[3]))
        elif len(p) == 3 and p[1] in 'tTwW':          # no size (assembly labels)
            syms.append((int(p[0], 16) & 0x1FFFFFFF, 0, p[2]))
    syms.sort()
    return syms


def owner(syms, starts, pc):
    """The function holding pc; an unsized symbol reaches up to the next one."""
    i = bisect.bisect_right(starts, pc) - 1
    if i < 0:
        return '?'
    a, size, name = syms[i]
    if size:
        return name if pc < a + size else '?'
    end = syms[i + 1][0] if i + 1 < len(syms) else a + 0x1000
    return name if pc < min(end, a + 0x1000) else '?'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('elf')
    ap.add_argument('--warmup', type=float, default=3)
    ap.add_argument('--secs', type=float, default=5)
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--port', type=int, default=9133)
    ap.add_argument('--pad', help='player-1 buttons held during the run, e.g. cright')
    ap.add_argument('--func', help='also list the hottest 16-byte buckets inside this function')
    ap.add_argument('--unknown', action='store_true', help="also list the hottest '?' buckets")
    a = ap.parse_args()
    # kestrel64 samples only what its interpreter runs, so the JIT is off unless asked for
    env = dict(os.environ, KESTREL_AUDIO='0')
    env.setdefault('KESTREL_JIT', '0')
    proc = subprocess.Popen([EXE, a.rom, '--port', str(a.port), '--run'], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        t = None
        for _ in range(100):
            try:
                t = Tel(a.port)
                t.q('status')
                break
            except OSError:
                time.sleep(0.1)
        if t is None:
            sys.exit('kestrel64 telemetry did not come up')
        if a.pad:      # held for the whole run
            t.q('pad.set', buttons=a.pad, stick_x=0, stick_y=0, polls=-1)
        time.sleep(a.warmup)
        t.q('prof.start')
        time.sleep(a.secs)
        t.q('pause')
        d = t.q('prof.cpu', top=65536)
        if d.get('warn'):
            print('warning: %s' % d['warn'], file=sys.stderr)
    finally:
        proc.kill()     # this PID only
        proc.wait(5)
    ents = d.get('top') or d.get('entries') or d.get('hot') or []
    if not ents:
        sys.exit('no samples: %s' % str(d)[:300])
    key = next((k for k in ('pc', 'addr', 'paddr', 'bucket', 'phys') if k in ents[0]), None)
    if key is None:
        sys.exit('unknown entry format: %s' % ents[0])
    syms = symbols(a.elf)
    starts = [x[0] for x in syms]
    total = sum(e['count'] for e in ents) or 1
    per = {}
    for e in ents:
        e['fn'] = owner(syms, starts, e[key] & 0x1FFFFFFF)
        per[e['fn']] = per.get(e['fn'], 0) + e['count']
    print('CPU samples: %d' % total)
    for n, c in sorted(per.items(), key=lambda kv: -kv[1])[:a.top]:
        print('%6.2f%%  %10d  %s' % (100.0 * c / total, c, n))
    for fn in ([a.func] if a.func else []) + (['?'] if a.unknown else []):
        base = next((x[0] for x in syms if x[2] == fn), 0)
        rows = [e for e in ents if e['fn'] == fn]
        print('%s buckets (%s):' % (fn, 'offset from start' if base else 'physical address'))
        for e in sorted(rows, key=lambda e: -e['count'])[:a.top]:
            print('  %s%-8x %6.2f%%' % ('+' if base else '', (e[key] & 0x1FFFFFFF) - base, 100.0 * e['count'] / total))


if __name__ == '__main__':
    main()
