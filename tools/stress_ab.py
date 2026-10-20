#!/usr/bin/env python3
"""RCP load of the stress scenes (demos/16_stress vs bench/t3d_stress) at fixed copy counts.

    stress_ab.py rom.z64 [rom2.z64 ...] [--copies 8 16 32 64] [--fields 120]

Both ROMs start at 1 copy and double it on each D-Up press (which also stops their
automatic sweep). For each count: boot kestrel64 paused on a private port, let it boot,
press D-Up log2(copies) times, settle, then read `rdp.stats` over --fields VI fields.
"""
import argparse, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from prof_rsp import Tel, EXE
from ab_stats import flat


def measure(rom, copies, fields, port):
    proc = subprocess.Popen([EXE, rom, '--port', str(port)], env=dict(os.environ, KESTREL_AUDIO='0'),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                t = Tel(port); t.q('status'); break
            except OSError:
                time.sleep(0.1)
        t.q('frame.advance', fields=90, timeout_ms=300000)
        for _ in range(copies.bit_length() - 1):
            t.q('pad.set', buttons='dup', stick_x=0, stick_y=0, polls=3)
            t.q('frame.advance', fields=12, timeout_ms=300000)
        t.q('frame.advance', fields=60, timeout_ms=300000)
        t.q('rdp.stats.reset')
        t.q('frame.advance', fields=fields, timeout_ms=600000)
        return flat(t.q('rdp.stats'))
    finally:
        proc.kill(); proc.wait()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('roms', nargs='+')
    p.add_argument('--copies', type=int, nargs='+', default=[8, 16, 32, 64])
    p.add_argument('--fields', type=int, default=120)
    p.add_argument('--port', type=int, default=9133)
    a = p.parse_args()
    print('%-22s %6s %9s %8s %8s %8s %10s' % ('rom', 'copies', 'ms/flip', 'rsp%', 'rdp%', 'tris', 'rsp us/tri'))
    for c in a.copies:
        for rom in a.roms:
            r = measure(rom, c, a.fields, a.port)
            ms, rsp, tris = r.get('msPerFlip', 0), r.get('rspRunPct', 0), r.get('tris', 0)
            per = ms * 1000 * rsp / 100 / tris if tris else 0
            print('%-22s %6d %9.2f %8.1f %8.1f %8.0f %10.2f' % (os.path.basename(rom)[:22], c, ms, rsp,
                  r.get('rdpBusyPct', 0), tris, per), flush=True)


if __name__ == '__main__':
    main()
