#!/usr/bin/env python3
"""RSP microcode profile of a ROM running in kestrel64 (local dev helper).

    prof_rsp.py rom.z64 build/ucode/kgfx.lst [--warmup 3] [--secs 5 | --fields N] [--top 40] [--prdp] [--timing]

Launches kestrel64 with its telemetry server on a private port, lets the ROM warm up,
samples the RSP per IMEM instruction (kestrel64 `prof.*`), then prints the time per
routine (nearest preceding label in the k64rsp listing) and the hottest instructions with
their source lines. The emulator is stopped by PID.

--timing runs kestrel64 with KESTREL_RSP_TIMING=1 (pipeline model: stalls, SU/VU dual issue,
DMA wait) and reports modelled cycles, stalls and dual-issue pairs per routine instead of
instruction counts.

--fields N makes the window deterministic: the emulator starts paused, advances --warmup x 60
VI fields, then profiles exactly N fields (`frame.advance`), so demos whose scene is a function
of the frame number give the same numbers on every run (A/B of ucode changes at ~0.1%).
"""
import argparse, json, os, socket, struct, subprocess, sys, time

EXE = os.environ.get('KESTREL_EXE', 'kestrel64')
EXE_PRDP = os.environ.get('KESTREL_EXE_PRDP', EXE)


class Tel:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=30)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.id = 0

    def recv(self, n):
        b = bytearray()
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise OSError('telemetry closed')
            b += c
        return bytes(b)

    def q(self, cmd, **args):
        self.id += 1
        req = {'id': self.id, 'cmd': cmd}
        if args:
            req['args'] = args
        j = json.dumps(req).encode()
        payload = struct.pack('<I', len(j)) + j
        self.s.sendall(struct.pack('<I', len(payload)) + payload)
        (total,) = struct.unpack('<I', self.recv(4))
        p = self.recv(total)
        (jl,) = struct.unpack('<I', p[:4])
        r = json.loads(p[4:4 + jl].decode())
        if not r.get('ok'):
            raise RuntimeError(r.get('error'))
        return r.get('data', {})


def load_listing(path, overlay=None):
    lines, label, cur = {}, '?', None
    for l in open(path, encoding='utf-8'):
        l = l.rstrip('\n')
        if l.startswith('; overlay'):       # overlays share slot addresses: main image, then
            cur = l.split()[2]              # the one overlay asked for over its slot
            continue
        if cur is not None and cur != overlay:
            continue
        if l.endswith(':') and ' ' not in l:
            label = l[:-1]
            continue
        addr = int(l[:3], 16)
        lines[addr] = (label, l[14:])
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('listing')
    ap.add_argument('--warmup', type=float, default=3)
    ap.add_argument('--secs', type=float, default=5)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--port', type=int, default=9130)
    ap.add_argument('--prdp', action='store_true')
    ap.add_argument('--timing', action='store_true')
    ap.add_argument('--pad', help='player-1 buttons held during the run, e.g. cright')
    ap.add_argument('--save', help='write the raw profile (JSON) to this file')
    ap.add_argument('--load', help='report a profile saved with --save instead of running the ROM')
    ap.add_argument('--routine', action='append', default=[],
                    help='list every instruction of this routine in IMEM order (repeatable)')
    ap.add_argument('--overlay', help='name the slot addresses after this overlay (clip, audio, part, ...)')
    ap.add_argument('--fields', type=int, default=0,
                    help='deterministic window: start paused, advance --warmup*60 fields, profile N fields')
    a = ap.parse_args()

    if a.load:
        d = json.load(open(a.load))
        a.timing = 'timing' in d
        return report(a, d)
    env = dict(os.environ, KESTREL_AUDIO='0')
    exe = EXE
    if a.prdp:
        exe, env['KESTREL_PRDP'] = EXE_PRDP, '1'
    if a.timing:
        env['KESTREL_RSP_TIMING'] = '1'
    argv = [exe, a.rom, '--port', str(a.port)] + ([] if a.fields else ['--run'])
    proc = subprocess.Popen(argv, env=env,
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
        if a.fields:
            t.q('frame.advance', fields=int(a.warmup * 60), timeout_ms=120000)
            t.q('prof.start')
            t.q('frame.advance', fields=a.fields, timeout_ms=300000)
        else:
            time.sleep(a.warmup)
            t.q('prof.start')
            time.sleep(a.secs)
            t.q('pause')
        d = t.q('prof.rsp', top=4096, metric='cycles' if a.timing else 'count')
    finally:
        proc.kill()     # this PID only
        proc.wait(5)
    if a.save:
        json.dump(d, open(a.save, 'w'))
    report(a, d)


def report(a, d):
    lst = load_listing(a.listing, a.overlay)
    ents = d.get('top') or d.get('entries') or d.get('hot') or []
    if a.timing:
        report_timing(lst, ents, d.get('timing', {}), a.top)
        if a.routine:
            by = {e['imem'] & 0xFFF: e for e in ents}
            for addr in sorted(lst):
                lab, src = lst[addr]
                if lab in a.routine:
                    e = by.get(addr, {})
                    print('%9d st %8d  %03X  %-14s %s' % (e.get('cycles', 0), e.get('stalls', 0), addr, lab, src))
        return
    total = d.get('total') or sum(e['count'] for e in ents) or 1
    per = {}
    for e in ents:
        lab = lst.get(e['imem'] & 0xFFF, ('<unknown>', ''))[0]
        per[lab] = per.get(lab, 0) + e['count']
    print('RSP samples: %d' % total)
    print('\n-- per routine --')
    for lab, c in sorted(per.items(), key=lambda kv: -kv[1]):
        print('%6.2f%%  %10d  %s' % (100.0 * c / total, c, lab))
    print('\n-- hottest instructions --')
    for e in sorted(ents, key=lambda e: -e['count'])[:a.top]:
        lab, src = lst.get(e['imem'] & 0xFFF, ('?', '?'))
        print('%6.2f%%  %03X  %-14s %s' % (100.0 * e['count'] / total, e['imem'] & 0xFFF, lab, src))


def report_timing(lst, ents, tm, top):
    tot = sum(e.get('cycles', 0) for e in ents) or 1
    per = {}
    for e in ents:
        lab = lst.get(e['imem'] & 0xFFF, ('<unknown>', ''))[0]
        r = per.setdefault(lab, [0, 0, 0, 0])
        r[0] += e.get('cycles', 0); r[1] += e['count']; r[2] += e.get('stalls', 0); r[3] += e.get('pairs', 0)
    print('timing: %s' % json.dumps(tm, sort_keys=True))
    print('\n-- per routine (modelled cycles) --')
    print('%7s %11s %11s %10s %10s %5s  %s' % ('cyc%', 'cycles', 'insns', 'stalls', 'pairs', 'IPC', 'routine'))
    for lab, (c, n, st, pr) in sorted(per.items(), key=lambda kv: -kv[1][0]):
        print('%6.2f%% %11d %11d %10d %10d %5.2f  %s' % (100.0 * c / tot, c, n, st, pr, n / c if c else 0, lab))
    print('\n-- most expensive instructions (cycles, stalls) --')
    for e in sorted(ents, key=lambda e: -e.get('cycles', 0))[:top]:
        lab, src = lst.get(e['imem'] & 0xFFF, ('?', '?'))
        print('%6.2f%% %9d st %8d  %03X  %-14s %s' % (100.0 * e.get('cycles', 0) / tot, e.get('cycles', 0),
                                                    e.get('stalls', 0), e['imem'] & 0xFFF, lab, src))


if __name__ == '__main__':
    main()
