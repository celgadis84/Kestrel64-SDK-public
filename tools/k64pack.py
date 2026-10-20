#!/usr/bin/env python3
"""kestrel64-sdk asset packer: LZ4 block stream with an optimal parse, in-place decodable.

    k64pack.py in.bin out.k64z        (also: import k64pack; k64pack.pack(bytes) -> bytes)

.k64z layout (big-endian):
  0  'K64Z'
  4  u32 raw_size      bytes after decoding
  8  u32 packed_size   bytes of the LZ4 block stream (without padding)
  12 u32 buf_size      buffer needed to decode in place: raw_size + margin, multiple of 16
  16 LZ4 block stream, zero padded to a multiple of 8

The stream is a standard LZ4 block (any LZ4 decoder reads it). In-place decoding
(k64_unpack_rom): the stream is DMA'd to the END of the destination buffer, at
buf_size - align8(packed_size), and decoded towards the start. buf_size is chosen so the
output stays 32 bytes behind the stream bytes not yet read: the decoder's fast paths
write up to 24 bytes past a sequence, and it never dirties a D$ line that a DMA still in
flight will fill.
"""
import struct, sys

MIN_MATCH = 4
MAX_OFFSET = 65535
LAST_LITERALS = 5           # LZ4 end rules: the last 5 bytes are literals
MF_LIMIT = 12               # and the last match starts at least 12 bytes before the end
CHAIN = 256                 # match finder depth


def _ext(n):
    """Extra length bytes for a 4-bit length field holding n."""
    return 0 if n < 15 else (n - 15) // 255 + 1


def _extend(data, i, j, l, lim):
    """Length of the common run at i and j, knowing the first l bytes match, up to lim."""
    while l + 32 <= lim and data[j + l:j + l + 32] == data[i + l:i + l + 32]:
        l += 32
    while l < lim and data[j + l] == data[i + l]:
        l += 1
    return l


def _matches(data):
    """Longest match (length, offset) at each position, nearest offset among equals."""
    n = len(data)
    best = [(0, 0)] * n
    head = {}
    prev = [-1] * n
    end_match = n - LAST_LITERALS
    pl, po = 0, 0                       # match at the previous position
    for i in range(n - MIN_MATCH + 1):
        key = data[i:i + 4]
        j = head.get(key, -1)
        prev[i] = j
        head[key] = i
        if i > n - MF_LIMIT:
            continue
        lim = end_match - i
        if pl > 64:
            # inside a long match: its tail is (almost surely) the best here too
            pl, po = min(pl - 1, lim), po
            best[i] = (pl, po)
            continue
        bl, bo, depth = 0, 0, CHAIN
        while j >= 0 and i - j <= MAX_OFFSET and depth:
            depth -= 1
            if bl == 0 or (bl < lim and data[j + bl] == data[i + bl]):
                l = _extend(data, i, j, 4, lim)
                if l > bl:
                    bl, bo = l, i - j
                    if l >= lim:
                        break
            j = prev[j]
        if bl >= MIN_MATCH:
            best[i] = (bl, bo)
        pl, po = bl, bo
    return best


def _parse(data):
    """Optimal parse (backward DP on encoded size). Returns [(lit_start, lit_len, mlen, off)]."""
    n = len(data)
    best = _matches(data)
    INF = 1 << 60
    cost = [INF] * (n + 1)
    choice = [0] * (n + 1)      # 0 = literal, else match length
    cost[n] = 0
    for i in range(n - 1, -1, -1):
        c = cost[i + 1] + 1
        ch = 0
        ml, off = best[i]
        if ml:
            # every length up to the longest: a shorter match can land on a better spot
            lens = range(MIN_MATCH, ml + 1) if ml <= 40 else list(range(MIN_MATCH, 20)) + [ml]
            for l in lens:
                m = 3 + _ext(l - MIN_MATCH) + cost[i + l]
                if m < c:
                    c, ch = m, l
        cost[i] = c
        choice[i] = ch
    seqs, i, lit = [], 0, 0
    while i < n:
        l = choice[i]
        if l:
            seqs.append((i - lit, lit, l, best[i][1]))
            i += l
            lit = 0
        else:
            i += 1
            lit += 1
    seqs.append((n - lit, lit, 0, 0))
    return seqs


def _encode(data, seqs):
    out = bytearray()
    marks = []                  # (input pos after literals, output pos after literals, ...)
    o = 0
    for start, lit, ml, off in seqs:
        lit_start_in = len(out) + 1 + _ext(lit)
        lit_start_out = o
        tok_l = min(lit, 15)
        tok_m = min(ml - MIN_MATCH, 15) if ml else 0
        out.append(tok_l << 4 | tok_m)
        if lit >= 15:
            r = lit - 15
            while r >= 255:
                out.append(255)
                r -= 255
            out.append(r)
        out += data[start:start + lit]
        o += lit
        if ml:
            out += struct.pack('<H', off)
            if ml - MIN_MATCH >= 15:
                r = ml - MIN_MATCH - 15
                while r >= 255:
                    out.append(255)
                    r -= 255
                out.append(r)
            o += ml
        # literal copy: output at lit_start_out.. must not pass the input being read;
        # after the match: output end must stay below the next unread input byte
        marks.append((lit_start_out - lit_start_in, o - len(out)))
    return bytes(out), marks


def decode(stream, raw_size):
    """Reference LZ4 block decoder (tests)."""
    out = bytearray()
    i = 0
    while i < len(stream):
        t = stream[i]; i += 1
        lit = t >> 4
        if lit == 15:
            while True:
                b = stream[i]; i += 1
                lit += b
                if b != 255:
                    break
        out += stream[i:i + lit]; i += lit
        if i >= len(stream):
            break
        off = stream[i] | stream[i + 1] << 8; i += 2
        ml = t & 15
        if ml == 15:
            while True:
                b = stream[i]; i += 1
                ml += b
                if b != 255:
                    break
        ml += MIN_MATCH
        for _ in range(ml):
            out.append(out[-off])
    assert len(out) == raw_size
    return bytes(out)


def pack(data):
    data = bytes(data)
    n = len(data)
    if n < MF_LIMIT + 1:
        seqs = [(0, n, 0, 0)]
    else:
        seqs = _parse(data)
    stream, marks = _encode(data, seqs)
    assert decode(stream, n) == data
    packed = len(stream)
    padded = (packed + 7) & ~7
    # stream at D = buf - padded (8-aligned); need D + in >= out + 32 at every mark
    need = max([0] + [max(a, b) for a, b in marks]) + 32
    buf = (max(n, need + padded) + 15) & ~15
    hdr = b'K64Z' + struct.pack('>III', n, packed, buf)
    return hdr + stream + bytes(padded - packed)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    z = pack(data)
    open(sys.argv[2], 'wb').write(z)
    raw, packed, buf = struct.unpack('>III', z[4:16])
    print('k64pack: %s  %d -> %d bytes (%.1f%%), in-place buffer %d (+%d)'
          % (sys.argv[1], raw, packed + 16, 100.0 * (packed + 16) / max(raw, 1), buf, buf - raw))


if __name__ == '__main__':
    main()
