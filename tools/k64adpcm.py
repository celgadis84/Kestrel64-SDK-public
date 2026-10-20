# kestrel64-sdk — VADPCM encoder (k64bake.py adpcm): WAV -> .k64a for the kgfx ADPCM overlay.
#
# Format (docs/AUDIO.md): 16-sample frames of 9 bytes, control byte scale << 4 | predictor,
# then 16 4-bit residuals (even sample in the high nibble); order-2 predictors stored as their
# responses in Q11 (p0: response to s[-2], p1: response to s[-1], 8 lanes each). The decoder is
# simulated exactly here (same integer arithmetic as the RSP and k64_mixer_run_cpu), so the
# encoder chooses each residual against what the console will actually decode.
#
# .k64a, big-endian: 'K6A1', rate, len, loop_start (0xFFFFFFFF = one-shot), nframes, npred,
# codebook offset, frames offset, loop_state[8] (s16); codebook (npred x 16 s16) at 48,
# frames after it (8-aligned).
import struct
import sys
import wave

NO_LOOP = 0xFFFFFFFF
MAX_SCALE = 12                      # (-8 << 12) still fits a s16 (RSP multiplies by 1 << scale)


def clamp16(x):
    return 32767 if x > 32767 else -32768 if x < -32768 else x


def read_wav(path):
    with wave.open(path, 'rb') as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 2:
        vals = struct.unpack('<%dh' % (len(raw) // 2), raw)
    elif width == 1:
        vals = [(b - 128) << 8 for b in raw]
    else:
        sys.exit('k64bake adpcm: %s: only 8/16-bit PCM WAV' % path)
    if ch > 1:
        vals = [sum(vals[i:i + ch]) // ch for i in range(0, len(vals), ch)]
    return list(vals), rate


def book_entry(a1, a2):
    """Q11 responses (p0, p1) of y[n] = a1 y[n-1] + a2 y[n-2] to s[-2] = 1 and to s[-1] = 1."""
    def resp(ym2, ym1):
        out = []
        for _ in range(8):
            y = a1 * ym1 + a2 * ym2
            out.append(y)
            ym2, ym1 = ym1, y
        return out
    q = lambda v: [clamp16(int(round(x * 2048))) for x in v]
    return q(resp(1.0, 0.0)), q(resp(0.0, 1.0))


def lpc2(frames_xy):
    """Least-squares (a1, a2) over a list of (x[n-2], x[n-1], x[n]) triples, kept stable."""
    r11 = r12 = r22 = c1 = c2 = 0.0
    for m2, m1, x in frames_xy:
        r11 += m1 * m1; r12 += m1 * m2; r22 += m2 * m2
        c1 += m1 * x; c2 += m2 * x
    det = r11 * r22 - r12 * r12
    if det <= 1e-9 * (r11 * r22 + 1e-9):
        a1, a2 = (c1 / r11 if r11 > 0 else 0.0), 0.0
    else:
        a1 = (c1 * r22 - c2 * r12) / det
        a2 = (r11 * c2 - r12 * c1) / det
    a2 = max(-0.97, min(0.97, a2))                       # stable: |a2| < 1, |a1| < 1 - a2
    lim = (1 - a2) * 0.995
    a1 = max(-lim, min(lim, a1))
    return a1, a2


def triples(x, f):
    base = f * 16
    return [(x[base + j - 2] if base + j >= 2 else 0, x[base + j - 1] if base + j >= 1 else 0,
             x[base + j]) for j in range(16)]


def design_book(x, nframes, npred, iters=12):
    """npred predictors by k-means over frames (assignment by open-loop prediction error,
    centroid = least-squares predictor of the cluster)."""
    trip = [triples(x, f) for f in range(nframes)]
    energy = [sum(t[2] * t[2] for t in tr) for tr in trip]
    order = sorted(range(nframes), key=lambda f: -energy[f])
    per = [lpc2(trip[f]) for f in range(nframes)]
    preds = []
    for f in order:                                      # seed: distinct loud frames
        if len(preds) == npred:
            break
        if all(abs(per[f][0] - p[0]) + abs(per[f][1] - p[1]) > 0.05 for p in preds):
            preds.append(per[f])
    while len(preds) < npred:
        preds.append((0.0, 0.0))

    def err(p, tr):
        return sum((x0 - p[0] * m1 - p[1] * m2) ** 2 for m2, m1, x0 in tr)
    for _ in range(iters):
        groups = [[] for _ in preds]
        for f in range(nframes):
            k = min(range(len(preds)), key=lambda i: err(preds[i], trip[f]))
            groups[k] += trip[f]
        preds = [lpc2(g) if g else p for g, p in zip(groups, preds)]
    return [book_entry(a1, a2) for a1, a2 in preds]


def decode_half(p0, p1, s6, s7, r):
    out = []
    for j in range(8):
        acc = p0[j] * s6 + p1[j] * s7 + r[j] * 2048
        for k in range(j):
            acc += r[k] * p1[j - k - 1]
        out.append(clamp16(acc >> 11))
    return out


def encode_frame(x16, state, book):
    """Best (error, control byte, nibbles, decoded 16) over predictors and nearby scales."""
    best = None
    for pi, (p0, p1) in enumerate(book):
        # open-loop residual size picks the scale range
        s6, s7 = state[6], state[7]
        peak = 0
        for h in range(2):
            pred = decode_half(p0, p1, s6, s7, [0] * 8)
            for j in range(8):
                peak = max(peak, abs(x16[h * 8 + j] - pred[j]))
            s6, s7 = x16[h * 8 + 6], x16[h * 8 + 7]
        sc0 = 0
        while sc0 < MAX_SCALE and (7 << sc0) < peak:
            sc0 += 1
        for sc in range(max(0, sc0 - 1), min(MAX_SCALE, sc0 + 1) + 1):
            s6, s7 = state[6], state[7]
            nib, dec, e = [], [], 0
            for h in range(2):
                r = []
                for j in range(8):
                    acc = p0[j] * s6 + p1[j] * s7
                    for k in range(j):
                        acc += r[k] * p1[j - k - 1]
                    want = x16[h * 8 + j]
                    # residual n with clamp16((acc + (n << sc) * 2048) >> 11) closest to want
                    n = int(round((want * 2048 - acc) / (2048.0 * (1 << sc))))
                    cand = []
                    for c in (n - 1, n, n + 1):
                        c = max(-8, min(7, c))
                        y = clamp16((acc + clamp16(c << sc) * 2048) >> 11)
                        cand.append(((y - want) ** 2, c, y))
                    d, c, y = min(cand)
                    e += d
                    nib.append(c)
                    r.append(clamp16(c << sc))
                    dec.append(y)
                s6, s7 = dec[h * 8 + 6], dec[h * 8 + 7]
            if best is None or e < best[0]:
                best = (e, sc << 4 | pi, nib, dec)
    return best


def bake(src, dst, loop=None, npred=4):
    x, rate = read_wav(src)
    if not x:
        sys.exit('k64bake adpcm: %s: empty' % src)
    if not 1 <= npred <= 8:
        sys.exit('k64bake adpcm: --npred 1..8')
    if loop is not None:
        if not 0 <= loop < len(x):
            sys.exit('k64bake adpcm: loop start %d outside 0..%d' % (loop, len(x) - 1))
        # loop start on a frame: silence before the attack; loop length a multiple of 16:
        # repeat the loop (exact, at most 16 copies)
        pad = (-loop) % 16
        x = [0] * pad + x
        loop += pad
        body = x[loop:]
        reps = 1
        while (len(body) * reps) % 16:
            reps += 1
        x = x[:loop] + body * reps
        nframes = len(x) // 16
        length = len(x)
    else:
        length = len(x)
        nframes = length // 16 + 1                       # the guard sample (len) is decoded
        x = x + [0] * (nframes * 16 - length)
    book = design_book(x, nframes, npred)

    state = [0] * 8
    frames = bytearray()
    loop_state = [0] * 8
    for f in range(nframes):
        if loop is not None and f * 16 == loop:
            loop_state = list(state)
        e, ctl, nib, dec = encode_frame(x[f * 16:f * 16 + 16], state, book)
        frames.append(ctl)
        for j in range(0, 16, 2):
            frames.append((nib[j] & 15) << 4 | (nib[j + 1] & 15))
        state = dec[8:]

    cb = b''.join(struct.pack('>16h', *(p0 + p1)) for p0, p1 in book)
    hdr = struct.pack('>4s7I8h', b'K6A1', rate, length, NO_LOOP if loop is None else loop,
                      nframes, npred, 48, 48 + len(cb), *loop_state)
    assert len(hdr) == 48
    with open(dst, 'wb') as f:
        f.write(hdr + cb + bytes(frames) + bytes((-len(frames)) & 7))
    return nframes
