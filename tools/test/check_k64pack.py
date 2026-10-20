#!/usr/bin/env python3
"""k64pack round trip + in-place decode simulation (the buffer layout k64_unpack_rom uses).

    python tools/test/check_k64pack.py [files...]
"""
import os, random, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import k64pack


def inplace(z):
    """Decode the way k64_unpack_rom does, in one buffer, checking the D$-line margin."""
    raw, packed, bufsz = struct.unpack('>III', z[4:16])
    padded = (packed + 7) & ~7
    buf = bytearray(b'\xAA' * bufsz)
    d = bufsz - padded
    assert d % 8 == 0 and bufsz % 16 == 0 and bufsz >= raw
    buf[d:d + padded] = z[16:16 + padded]
    i, o, end = d, 0, d + packed

    def rd():
        nonlocal i
        b = buf[i]; i += 1
        return b

    def ext(n):
        while True:
            b = rd(); n += b
            if b != 255:
                return n

    while True:
        t = rd()
        lit = t >> 4
        if lit == 15:
            lit = ext(lit)
        assert o + 32 <= i or lit == 0, 'literal copy too close'
        for _ in range(lit):
            buf[o] = buf[i]; o += 1; i += 1
        if i == end:
            break
        off = buf[i] | buf[i + 1] << 8; i += 2
        ml = t & 15
        if ml == 15:
            ml = ext(ml)
        ml += 4
        for _ in range(ml):
            buf[o] = buf[o - off]; o += 1
        assert o + 32 <= i, 'match output reached unread input'
    assert o == raw
    return bytes(buf[:raw])


def check(name, data):
    z = k64pack.pack(data)
    assert z[:4] == b'K64Z' and len(z) % 8 == 0
    assert inplace(z) == data, name
    raw, packed, bufsz = struct.unpack('>III', z[4:16])
    print('ok  %-28s %7d -> %7d  margin %d' % (name, raw, packed + 16, bufsz - raw))


def main():
    rnd = random.Random(1)
    check('empty', b'')
    check('one byte', b'x')
    check('short', b'abcabcabcabc')
    check('zeros 100k', bytes(100000))
    check('random 20k', bytes(rnd.getrandbits(8) for _ in range(20000)))
    check('runs', b''.join(bytes([rnd.randrange(4)]) * rnd.randrange(1, 300) for _ in range(500)))
    words = [bytes(rnd.getrandbits(8) for _ in range(rnd.randrange(3, 12))) for _ in range(64)]
    check('words', b''.join(rnd.choice(words) for _ in range(4000)))
    check('long literal + match', bytes(rnd.getrandbits(8) for _ in range(5000)) * 3)
    for f in sys.argv[1:]:
        check(os.path.basename(f), open(f, 'rb').read())


if __name__ == '__main__':
    main()
