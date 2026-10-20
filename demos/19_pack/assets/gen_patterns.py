#!/usr/bin/env python3
"""Writes patterns.bin: runs with periods 1..9 between random literals, to exercise the
decoder's short-offset match copies. main.c regenerates the same bytes to check them."""
import os

def gen():
    x = 1
    def r():
        nonlocal x
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        return x
    out = bytearray()
    for seg in range(300):
        v = r()
        if seg % 3 == 2:
            out += bytes(r() >> 24 for _ in range(1 + (v >> 12) % 40))
        else:
            p, n = 1 + (v >> 8) % 9, 1 + (v >> 12) % 200
            pat = [r() >> 24 for _ in range(p)]
            out += bytes(pat[i % p] for i in range(n))
    return bytes(out)

if __name__ == '__main__':
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'patterns.bin'), 'wb').write(gen())
