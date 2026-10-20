#!/usr/bin/env python3
"""Per-scene RCP load of a ROM in kestrel64, for A/B comparisons (local dev helper).

    ab_stats.py rom.z64 [rom2.z64 ...] [--warmup 3] [--fields 120] [--prdp] [--timing]

For each ROM: start kestrel64 paused on a private telemetry port, advance --warmup x 60
VI fields, reset the `rdp.stats` window, advance exactly --fields fields and print the
window (RDP busy, RSP run, CPU idle, ms per flip). Deterministic for scenes driven by the
frame count. The emulator is stopped by PID.
"""
import argparse, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from prof_rsp import Tel, EXE, EXE_PRDP

KEYS = ('msPerFlip', 'rdpBusyPct', 'rspRunPct', 'rspPollPct', 'cpuIdlePct', 'tris', 'gclk')


def flat(d):
    """rdp.stats -> one row: the guest window plus triangles and RDP GCLK per flip."""
    r = dict(d.get('guest', {}))
    flips = max(d.get('flips', 0), 1)
    r['tris'] = sum(v for k, v in d.get('ops', {}).items() if k.startswith('TRI')) / flips
    r['gclk'] = d.get('perFlip', {}).get('gclk')
    return r


def measure(a, rom):
    env = dict(os.environ, KESTREL_AUDIO='0')
    exe = EXE
    if a.prdp:
        exe, env['KESTREL_PRDP'] = EXE_PRDP, '1'
    if a.timing:
        env['KESTREL_RSP_TIMING'] = '1'
    proc = subprocess.Popen([exe, rom, '--port', str(a.port)], env=env,
                            stdout=subprocess.DEVNULL, stderr=open(os.environ.get('AB_LOG', os.devnull), 'w'))
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
        if a.pad:      # held for the whole run (e.g. cright: more actors in tiny3d 03_objects)
            t.q('pad.set', buttons=a.pad, stick_x=0, stick_y=0, polls=-1)
        t.q('frame.advance', fields=int(a.warmup * 60), timeout_ms=300000)
        t.q('rdp.stats.reset')
        t.q('frame.advance', fields=a.fields, timeout_ms=600000)
        return t.q('rdp.stats')
    finally:
        proc.kill()
        proc.wait()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('roms', nargs='+')
    p.add_argument('--warmup', type=float, default=3)
    p.add_argument('--fields', type=int, default=120)
    p.add_argument('--port', type=int, default=9133)
    p.add_argument('--prdp', action='store_true')
    p.add_argument('--timing', action='store_true')
    p.add_argument('--pad', help='player-1 buttons held during the run, e.g. cright')
    p.add_argument('--raw', action='store_true', help='print every rdp.stats field')
    a = p.parse_args()
    print('%-34s' % 'rom' + ''.join('%12s' % k for k in KEYS))
    for rom in a.roms:
        # kestrel64 sometimes dies mid-run (host-side, intermittent): retry a few times.
        for attempt in range(3):
            try:
                d = measure(a, rom)
                break
            except ConnectionError as e:
                print(f'{os.path.basename(rom)}: kestrel64 connection lost ({e}), retrying', file=sys.stderr)
        else:
            sys.exit('kestrel64 kept dying')
        if a.raw:
            print(d)
        d = flat(d)
        print('%-34s' % os.path.basename(rom)
              + ''.join('%12s' % (('%.2f' % d[k] if d[k] < 1e5 else '%.0f' % d[k]) if isinstance(d.get(k), (int, float)) else '-')
                        for k in KEYS))


if __name__ == '__main__':
    main()
