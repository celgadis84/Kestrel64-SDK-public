# kestrel64-sdk — Standard MIDI File (type 0 / 1) -> .k64q song (docs/MUSIC.md).
#
# All tracks are merged into one event stream in tick order. A note is one event with its
# duration (note-on .. note-off), so the player keeps no note-off state. Kept: program,
# volume (CC 7), pan (CC 10), expression (CC 11), sustain pedal (CC 64), pitch bend, bend
# range (RPN 0), tempo. Dropped: other controllers, sysex, aftertouch, text.
#
# Loop: markers named "loopStart" / "loopEnd" (meta 0x06), or --loop (whole song, the loop
# end at the end of the last bar that has notes, or --loop-end ticks). Notes still sounding
# at the loop end are cut there; the tempo at the loop start is restored after the jump (other
# channel state is not).
#
# Layout (big-endian):
#   0  'K64Q'
#   4  u16 ppq, u16 version (1)
#   8  u32 tempo (µs per quarter at tick 0)
#   12 u32 events bytes
#   16 events: varint delta ticks, then an op byte:
#        0x0c note       key, velocity, varint duration (ticks, >= 1)
#        0x1c program    p
#        0x2c volume     v              (c = channel 0..15)
#        0x3c pan        p
#        0x4c bend       s16 (-8192..8191)
#        0x5c expression e
#        0x6c sustain    0 / 1
#        0x7c bend range semitones
#        0xF0 tempo      u24 µs per quarter
#        0xF1 loop start
#        0xF2 loop end   (jump back to the loop start)
#        0xFF end
import struct
import sys


def varint(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def read_smf(data):
    if data[:4] != b'MThd':
        raise SystemExit('not a Standard MIDI File')
    hlen, fmt, ntrk, div = struct.unpack('>IHHH', data[4:14])
    if div & 0x8000:
        raise SystemExit('SMPTE time division not supported')
    pos = 8 + hlen
    tracks = []
    for _ in range(ntrk):
        while data[pos:pos + 4] != b'MTrk':
            if pos + 8 > len(data):
                raise SystemExit('truncated MIDI file')
            pos += 8 + struct.unpack('>I', data[pos + 4:pos + 8])[0]
        n = struct.unpack('>I', data[pos + 4:pos + 8])[0]
        tracks.append(data[pos + 8:pos + 8 + n])
        pos += 8 + n
    return div, tracks


def track_events(trk, ti):
    """(tick, order, kind, args) for one track; order keeps the file order at equal ticks."""
    ev = []
    pos = tick = 0
    status = 0
    seq = 0

    def rd_var():
        nonlocal pos
        v = 0
        while True:
            b = trk[pos]; pos += 1
            v = (v << 7) | (b & 0x7F)
            if not b & 0x80:
                return v

    while pos < len(trk):
        tick += rd_var()
        b = trk[pos]
        if b & 0x80:
            pos += 1
            status = b if b < 0xF0 else status
        else:
            b = status
        seq += 1
        o = (ti, seq)
        if b == 0xFF:
            typ = trk[pos]; pos += 1
            n = rd_var()
            body = trk[pos:pos + n]; pos += n
            if typ == 0x51:
                ev.append((tick, o, 'tempo', (body[0] << 16 | body[1] << 8 | body[2],)))
            elif typ in (0x06, 0x01) and body.strip().lower() in (b'loopstart', b'loop_start'):
                ev.append((tick, o, 'loopstart', ()))
            elif typ in (0x06, 0x01) and body.strip().lower() in (b'loopend', b'loop_end'):
                ev.append((tick, o, 'loopend', ()))
            elif typ == 0x2F:
                break
            continue
        if b in (0xF0, 0xF7):
            n = rd_var(); pos += n
            continue
        hi, ch = b & 0xF0, b & 0x0F
        if hi in (0xC0, 0xD0):
            d = (trk[pos],); pos += 1
        else:
            d = (trk[pos], trk[pos + 1]); pos += 2
        if hi == 0x90 and d[1]:
            ev.append((tick, o, 'on', (ch, d[0], d[1])))
        elif hi == 0x80 or hi == 0x90:
            ev.append((tick, o, 'off', (ch, d[0])))
        elif hi == 0xC0:
            ev.append((tick, o, 'program', (ch, d[0])))
        elif hi == 0xE0:
            ev.append((tick, o, 'bend', (ch, (d[0] | d[1] << 7) - 8192)))
        elif hi == 0xB0:
            ev.append((tick, o, 'cc', (ch, d[0], d[1])))
    return ev


def convert(data, loop=False, loop_end=None):
    ppq, tracks = read_smf(data)
    ev = []
    for i, t in enumerate(tracks):
        ev += track_events(t, i)
    # offs before ons at equal ticks (a re-struck note ends first), else file order
    rank = {'off': 0}
    ev.sort(key=lambda e: (e[0], rank.get(e[2], 1), e[1]))

    # notes: pair each on with the next off of the same channel + key (FIFO)
    open_notes = {}
    notes = []
    last_tick = 0
    for tick, o, kind, a in ev:
        last_tick = max(last_tick, tick)
        if kind == 'on':
            open_notes.setdefault(a[:2], []).append(len(notes))
            notes.append([tick, o, a[0], a[1], a[2], None])
        elif kind == 'off':
            q = open_notes.get(a)
            if q:
                notes[q.pop(0)][5] = tick
    for n in notes:
        if n[5] is None:
            n[5] = last_tick
    ls = [e[0] for e in ev if e[2] == 'loopstart']
    le = [e[0] for e in ev if e[2] == 'loopend']
    lstart = ls[0] if ls else None
    lend = le[0] if le else None
    if loop and lstart is None:
        lstart = 0
        if loop_end is not None:
            lend = loop_end
        else:
            bar = ppq * 4
            end = max((n[5] for n in notes), default=0)
            lend = max(bar, (end + bar - 1) // bar * bar)
    if lstart is not None and lend is None:
        lend = max((n[5] for n in notes), default=lstart + 1)

    out = []   # (tick, order, bytes)
    rpn = {}
    for tick, o, kind, a in ev:
        if lend is not None and tick >= lend:
            continue
        if kind == 'tempo':
            out.append((tick, o, bytes([0xF0]) + a[0].to_bytes(3, 'big')))
        elif kind == 'program':
            out.append((tick, o, bytes([0x10 | a[0], a[1]])))
        elif kind == 'bend':
            out.append((tick, o, bytes([0x40 | a[0]]) + struct.pack('>h', a[1])))
        elif kind == 'cc':
            ch, c, v = a
            r = rpn.setdefault(ch, [127, 127])
            if c == 7:
                out.append((tick, o, bytes([0x20 | ch, v])))
            elif c == 10:
                out.append((tick, o, bytes([0x30 | ch, v])))
            elif c == 11:
                out.append((tick, o, bytes([0x50 | ch, v])))
            elif c == 64:
                out.append((tick, o, bytes([0x60 | ch, 1 if v >= 64 else 0])))
            elif c == 101:
                r[0] = v
            elif c == 100:
                r[1] = v
            elif c == 6 and r == [0, 0]:
                out.append((tick, o, bytes([0x70 | ch, v])))
    for t0, o, ch, key, vel, t1 in notes:
        if lend is not None:
            if t0 >= lend:
                continue
            t1 = min(t1, lend)
        out.append((t0, o, bytes([ch, key, vel]) + varint(max(1, t1 - t0))))
    if lstart is not None:
        # a tempo change inside the loop: restore the loop start's tempo after the jump
        tempos = [e for e in ev if e[2] == 'tempo']
        if any(lstart < e[0] < lend for e in tempos):
            at = [e[3][0] for e in tempos if e[0] <= lstart]
            out.append((lstart, (-1, 1), bytes([0xF0]) + (at[-1] if at else 500000).to_bytes(3, 'big')))
        out.append((lstart, (-1, 0), b'\xF1'))
        out.append((lend, (1 << 30, 0), b'\xF2'))

    # equal ticks: loop start, controls, notes, loop end (a control applies to its tick's notes)
    def rank(e):
        if e[1][0] < 0:
            return -1
        if e[1][0] >= 1 << 30:
            return 2
        return 1 if e[2][0] < 0x10 else 0
    out.sort(key=lambda e: (e[0], rank(e), e[1]))

    body = bytearray()
    t = 0
    for tick, _, b in out:
        body += varint(tick - t) + b
        t = tick
    body += varint(0) + b'\xFF'
    first_tempo = next((e[3][0] for e in ev if e[2] == 'tempo' and e[0] == 0), 500000)
    head = b'K64Q' + struct.pack('>HHII', ppq, 1, first_tempo, len(body))
    blob = head + bytes(body)
    blob += b'\0' * (-len(blob) % 16)
    return blob, len(notes)


def bake(src, dst, loop=False, loop_end=None):
    blob, n = convert(open(src, 'rb').read(), loop, loop_end)
    with open(dst, 'wb') as f:
        f.write(blob)
    print('k64music: %s -> %s, %d notes, %d bytes' % (src, dst, n, len(blob)))


def write_smf(path, ppq, tracks):
    """Writes a type 1 file; tracks = lists of (tick, bytes) with absolute ticks (tools, tests)."""
    out = b'MThd' + struct.pack('>IHHH', 6, 1, len(tracks), ppq)
    for tr in tracks:
        body = bytearray()
        t = 0
        for tick, b in sorted(tr, key=lambda e: e[0]):
            body += varint(tick - t) + b
            t = tick
        body += b'\x00\xFF\x2F\x00'
        out += b'MTrk' + struct.pack('>I', len(body)) + bytes(body)
    with open(path, 'wb') as f:
        f.write(out)


if __name__ == '__main__':
    bake(sys.argv[1], sys.argv[2], '--loop' in sys.argv)
