#!/usr/bin/env python3
"""kestrel64-sdk video encoder: K64V, a transform codec laid out for the RSP.

    k64vid.py out.k64v --frames DIR [--fps 24] [--q 8] [--gop 48] [--rd 0.85] [--search 12] [--audio clip.wav]
    k64vid.py --decode in.k64v [outdir]     reference decode: checks every frame CRC,
                                            optionally writes the frames as PNG

DIR holds the frames as PNG files, played in name order; width and height must be
multiples of 16. Audio: a 16-bit mono WAV at any rate (the player sets the AI to it),
coded as VADPCM (tools/k64adpcm.py: one codebook of --npred predictors for the clip) and
decoded on the RSP by the kgfx ADPCM overlay, the same decoder as the mixer's.

Codec (docs/VIDEO.md): YUV 4:2:0, 16x16 macroblocks of four 8x8 Y blocks + one U + one V.
I frames code every macroblock intra; P frames choose per macroblock between skip (copy
the previous frame), inter (previous frame + coded residual), motion (previous frame
displaced by a whole-pixel vector + coded residual) and intra. Blocks are an 8x8
integer IDCT of dequantised coefficients, defined as the RSP computes it: two passes of a
Q15 matrix product, each output rounded and saturated like vmulf + 7 x vmacf, so the RSP
microcode and the CPU reference decoder (src/video/video_ref.c) are bit-exact with the
reconstruction this encoder predicts from.

Bitstream, MSB first: frame header {1 bit P, 5 bits qscale}; per macroblock (P frames
only) ue(mode) 0 skip / 1 inter / 2 intra / 3 motion; mode 3 then se(dx - px), se(dy - py)
with (px, py) the vector of the macroblock to the left if that one is mode 3, else (0, 0)
(se(v): ue(2v - 1) for v > 0, else ue(-2v)). The vector moves the 16x16 luma source block,
chroma moves by (dx >> 1, dy >> 1) (arithmetic shift); the luma source lies inside the
frame. Skip, inter and motion predict from the previous frame. Unless skip, then 6 bits
coded block pattern (Y0..Y3, U, V,
bit 5 = Y0); per coded block ue(ntok - 1) then ntok tokens {ue(run), ue(|level| - 1),
sign} in zigzag order. Coefficient = clamp16(level * qscale * (8 + u + v)).

File (big-endian): 64-byte header {'K64V', u16 version 3, u16 flags, u16 width,
u16 height, u32 fps 16.16, u32 frames, u32 audio rate (0 none), u16 channels, u16 npred
(audio predictors), u32 max packet, u32 first packet size, u32 max audio frames per
packet}, the audio codebook (npred x 16 s16, Q11, as in .k64a), then one packet per frame,
each 16-aligned: {u32 next packet size (0 last), u32 bitstream bytes, u32 audio frames, u32
FNV-1a of the decoded Y, U, V planes, u32 FNV-1a of the decoded audio samples (s16
big-endian bytes), 12 bytes 0}, VADPCM frames (9 bytes per 16 samples, padded to 16),
bitstream (padded to 16). The packet of video frame f carries the audio frames that end by
sample round((f + 1) * rate / fps); the decoder state carries over from packet to packet.
"""
import argparse, os, struct, sys, wave
import numpy as np
import k64adpcm

# ---- transform, defined by the RSP arithmetic ---------------------------------------------
def _basis():
    m = np.zeros((8, 8))
    for i in range(8):
        for k in range(8):
            c = np.sqrt(0.5) if k == 0 else 1.0
            m[i, k] = 0.5 * c * np.cos((2 * i + 1) * k * np.pi / 16)
    return m

BASIS = _basis()                                   # pixel[i] = sum_k BASIS[i,k] coef[k]
M_Q15 = np.round(BASIS * 32768).astype(np.int64)   # IDCT matrix as the microcode holds it

ZIGZAG = sorted(((v, u) for v in range(8) for u in range(8)),
                key=lambda p: (p[0] + p[1], p[1] if (p[0] + p[1]) & 1 == 0 else p[0]))
ZZ_INDEX = np.array([v * 8 + u for v, u in ZIGZAG])
WEIGHT = np.array([[8 + u + v for u in range(8)] for v in range(8)], dtype=np.int64)

def _mac_pass(x):
    """vmulf + 7 x vmacf over (n, 8, 8): out[i][j] = sat16((sum_k 2 M[i,k] x[k,j] + 0x8000) >> 16)."""
    s = np.einsum('ik,nkj->nij', M_Q15, x)
    return np.clip((2 * s + 0x8000) >> 16, -32768, 32767)

def idct(coef):
    """(n, 8, 8) s16 coefficients [v][u] -> (n, 8, 8) pixel deltas [row][col] (integers)."""
    t = _mac_pass(coef)                             # vertical pass, lanes = u
    p = _mac_pass(t.transpose(0, 2, 1))             # horizontal pass, lanes = row
    p = p.transpose(0, 2, 1)
    return (p * 0x2000 + 0x8000) >> 16              # vmulf by 0x1000: round(p / 8)

def fdct(blk):
    """(n, 8, 8) float pixel deltas -> coefficients in the decoder's x8 scale."""
    return np.einsum('ki,nij,jl->nkl', BASIS.T, blk * 8.0, BASIS)

# ---- bit writer ---------------------------------------------------------------------------
class Bits:
    def __init__(self):
        self.out = bytearray(); self.acc = 0; self.n = 0
    def put(self, v, n):
        self.acc = (self.acc << n) | v; self.n += n
        while self.n >= 8:
            self.n -= 8
            self.out.append((self.acc >> self.n) & 0xFF)
        self.acc &= (1 << self.n) - 1
    def ue(self, v):
        v += 1; n = v.bit_length()
        self.put(v, 2 * n - 1)
    def se(self, v):
        self.ue(2 * v - 1 if v > 0 else -2 * v)
    def bytes(self):
        b = bytearray(self.out)
        if self.n: b.append((self.acc << (8 - self.n)) & 0xFF)
        return bytes(b)

def ue_len(v):
    return 2 * (v + 1).bit_length() - 1

def se_len(v):
    return ue_len(2 * v - 1 if v > 0 else -2 * v)

# ---- block coding -------------------------------------------------------------------------
def quantize(coef, step, intra):
    """Dead-zone quantiser: rounding offset 1/3 intra, 1/6 inter."""
    off = 1 / 3 if intra else 1 / 6
    a = np.floor(np.abs(coef) / step + off)
    return (np.sign(coef) * a).astype(np.int64)

def dequant(level, step):
    return np.clip(level * step, -32768, 32767)

def tokens(level_zz):
    """Zigzag levels (64,) -> [(run, level)]."""
    out, run = [], 0
    for l in level_zz:
        if l == 0: run += 1
        else: out.append((run, int(l))); run = 0
    return out

def tok_bits(toks):
    if not toks: return 0
    return ue_len(len(toks) - 1) + sum(ue_len(r) + ue_len(abs(l) - 1) + 1 for r, l in toks)

def write_block(bw, toks):
    bw.ue(len(toks) - 1)
    for r, l in toks:
        bw.ue(r); bw.ue(abs(l) - 1); bw.put(1 if l < 0 else 0, 1)

# ---- frames -------------------------------------------------------------------------------
def rgb_to_yuv(rgb):
    """BT.601 studio-swing... full range: Y 0-255, U/V centred on 128, chroma averaged 2x2."""
    r, g, b = [rgb[..., i].astype(np.float64) for i in range(3)]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    u = (b - y) * 0.564 + 128
    v = (r - y) * 0.713 + 128
    sub = lambda c: c.reshape(c.shape[0] // 2, 2, c.shape[1] // 2, 2).mean(axis=(1, 3))
    return y, sub(u), sub(v)

def blocks(plane, bs=8):
    h, w = plane.shape
    return plane.reshape(h // bs, bs, w // bs, bs).transpose(0, 2, 1, 3)   # (by, bx, 8, 8)

def mb_blocks(y, u, v):
    """Planes -> (mbh, mbw, 6, 8, 8) in macroblock block order Y0 Y1 Y2 Y3 U V."""
    yb = blocks(y)
    mbh, mbw = yb.shape[0] // 2, yb.shape[1] // 2
    yb = yb.reshape(mbh, 2, mbw, 2, 8, 8).transpose(0, 2, 1, 3, 4, 5).reshape(mbh, mbw, 4, 8, 8)
    return np.concatenate([yb, blocks(u)[:, :, None], blocks(v)[:, :, None]], axis=2)

def mb_planes(b, mbh, mbw):
    """Inverse of mb_blocks."""
    yb = b[:, :, :4].reshape(mbh, mbw, 2, 2, 8, 8).transpose(0, 2, 4, 1, 3, 5).reshape(mbh * 16, mbw * 16)
    cb = lambda k: b[:, :, k].transpose(0, 2, 1, 3).reshape(mbh * 8, mbw * 8)
    return yb, cb(4), cb(5)

def fnv1a(*planes):
    h = 0x811C9DC5
    for p in planes:
        for byte in p.astype(np.uint8).tobytes():
            h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h

def motion_search(sy, ry, rng):
    """Whole-pixel full search of luma SAD (+ a small vector-length penalty) per macroblock,
    source blocks inside the frame. sy, ry: (h, w) planes. Returns (mbh, mbw, 2) of (dx, dy)."""
    h, w = sy.shape
    mbh, mbw = h // 16, w // 16
    pad = np.pad(ry.astype(np.float64), rng, constant_values=1e7)   # outside: never chosen
    best = np.full((mbh, mbw), np.inf)
    mv = np.zeros((mbh, mbw, 2), dtype=np.int64)
    for dy in range(-rng, rng + 1):
        for dx in range(-rng, rng + 1):
            sh = pad[rng + dy:rng + dy + h, rng + dx:rng + dx + w]
            sad = np.abs(sy - sh).reshape(mbh, 16, mbw, 16).sum(axis=(1, 3)) + 4 * (abs(dx) + abs(dy))
            better = sad < best
            best = np.where(better, sad, best)
            mv[better] = (dx, dy)
    return mv

def mc_block(planes, mx, my, dx, dy):
    """Prediction (6, 8, 8) of macroblock (mx, my) from the planes displaced by (dx, dy)."""
    y, u, v = planes
    out = np.empty((6, 8, 8), dtype=np.int64)
    ly = y[my * 16 + dy:my * 16 + dy + 16, mx * 16 + dx:mx * 16 + dx + 16]
    out[:4] = ly.reshape(2, 8, 2, 8).transpose(0, 2, 1, 3).reshape(4, 8, 8)
    cy, cx = my * 8 + (dy >> 1), mx * 8 + (dx >> 1)
    out[4] = u[cy:cy + 8, cx:cx + 8]
    out[5] = v[cy:cy + 8, cx:cx + 8]
    return out

def encode_frame(src, ref, q, is_p, lam, rng=12):
    """src: (mbh, mbw, 6, 8, 8) float; ref: same shape uint8 recon or None.
    Returns (bitstream bytes, recon (mbh, mbw, 6, 8, 8) int)."""
    mbh, mbw = src.shape[:2]
    step = q * WEIGHT
    n = mbh * mbw * 6
    flat = src.reshape(n, 8, 8)

    def code(delta, base, intra):
        lev = quantize(fdct(delta), step, intra)
        rec = np.clip(idct(dequant(lev, step)) + base, 0, 255)
        return lev, rec

    lev_i, rec_i = code(flat - 128, 128, True)
    cand = [('intra', lev_i, rec_i)]
    if is_p:
        rf = ref.reshape(n, 8, 8).astype(np.int64)
        lev_p, rec_p = code(flat - rf, rf, False)
        cand.append(('inter', lev_p, rec_p))
        if rng:
            planes = mb_planes(ref.astype(np.int64), mbh, mbw)
            mv = motion_search(mb_planes(src, mbh, mbw)[0], planes[0], rng)
            mp = np.stack([mc_block(planes, mx, my, *mv[my, mx])
                           for my in range(mbh) for mx in range(mbw)]).reshape(n, 8, 8)
            lev_m, rec_m = code(flat - mp, mp, False)
            cand.append(('motion', lev_m, rec_m))

    bw = Bits()
    bw.put(1 if is_p else 0, 1); bw.put(q, 5)
    recon = np.zeros((n, 8, 8), dtype=np.int64)
    pv = (0, 0)
    for m in range(mbh * mbw):
        if m % mbw == 0: pv = (0, 0)
        sl = slice(m * 6, m * 6 + 6)
        best = None
        if is_p:   # skip: no bits beyond the mode
            sse = float(((flat[sl] - rf[sl]) ** 2).sum())
            best = (sse + lam * ue_len(0), 'skip', None, rf[sl], None)
        for name, lev, rec in cand:
            d = None
            if name == 'motion':
                d = (int(mv[m // mbw, m % mbw, 0]), int(mv[m // mbw, m % mbw, 1]))
                if d == (0, 0): continue                # that is inter
                nbits = ue_len(3) + se_len(d[0] - pv[0]) + se_len(d[1] - pv[1])
                base = mp[sl]
            else:
                nbits = ue_len(1 if name == 'inter' else 2) if is_p else 0
                base = rf[sl] if name == 'inter' else None
            zz = lev[sl].reshape(6, 64)[:, ZZ_INDEX]
            toks = [tokens(z) for z in zz]
            nbits += 6 + sum(tok_bits(t) for t in toks)
            # an uncoded block reconstructs as its base (128 or the prediction)
            r = rec[sl].copy()
            for k in range(6):
                if not toks[k]: r[k] = 128 if name == 'intra' else base[k]
            sse = float(((flat[sl] - r) ** 2).sum())
            j = sse + lam * nbits
            if best is None or j < best[0]: best = (j, name, toks, r, d)
        _, name, toks, r, d = best
        if is_p: bw.ue({'skip': 0, 'inter': 1, 'intra': 2, 'motion': 3}[name])
        if d is not None:
            bw.se(d[0] - pv[0]); bw.se(d[1] - pv[1])
        pv = d if d is not None else (0, 0)
        if name != 'skip':
            cbp = 0
            for k in range(6):
                if toks[k]: cbp |= 0x20 >> k
            bw.put(cbp, 6)
            for k in range(6):
                if toks[k]: write_block(bw, toks[k])
        recon[sl] = r
    return bw.bytes(), recon.reshape(mbh, mbw, 6, 8, 8)

def encode_audio(pcm, nframes, npred):
    """VADPCM: (codebook bytes, frames bytes, decoded samples) for the whole clip."""
    x = [int(v) for v in pcm] + [0] * (nframes * 16 - len(pcm))
    book = k64adpcm.design_book(x, nframes, npred)
    state, frames, dec = [0] * 8, bytearray(), []
    for f in range(nframes):
        _, ctl, nib, d = k64adpcm.encode_frame(x[f * 16:f * 16 + 16], state, book)
        frames.append(ctl)
        for j in range(0, 16, 2):
            frames.append((nib[j] & 15) << 4 | (nib[j + 1] & 15))
        dec += d
        state = d[8:]
    cb = b''.join(struct.pack('>16h', *(p0 + p1)) for p0, p1 in book)
    return cb, bytes(frames), dec

def decode_audio(book, frames, state):
    """Decodes VADPCM frames (the RSP's arithmetic); returns (samples, final state)."""
    out = []
    for f in range(0, len(frames), 9):
        ctl = frames[f]
        p0, p1 = book[ctl & 15]
        res = []
        for b in frames[f + 1:f + 9]:
            for nib in (b >> 4, b & 15):
                res.append(k64adpcm.clamp16(((nib ^ 8) - 8) << (ctl >> 4)))
        for h in range(2):
            state = k64adpcm.decode_half(p0, p1, state[6], state[7], res[h * 8:h * 8 + 8])
            out += state
    return out, state

def fnv1a_s16(samples):
    h = 0x811C9DC5
    for byte in struct.pack('>%dh' % len(samples), *samples):
        h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h

def load_wav(path):
    with wave.open(path, 'rb') as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 1:
            sys.exit("k64vid: audio must be 16-bit mono WAV")
        return w.getframerate(), np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(np.int64)

# ---- reference decoder (independent of the encoder state: parses the file) ----------------
class BitReader:
    def __init__(self, b):
        self.v = int.from_bytes(b, 'big'); self.n = len(b) * 8; self.p = 0
    def get(self, n):
        self.p += n
        if self.p > self.n: raise ValueError("bitstream overrun")
        return (self.v >> (self.n - self.p)) & ((1 << n) - 1)
    def ue(self):
        z = 0
        while self.get(1) == 0: z += 1
        return ((1 << z) | self.get(z)) - 1
    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)

def yuv_to_rgb(y, u, v):
    up = lambda c: c.repeat(2, 0).repeat(2, 1).astype(np.float64) - 128
    y = y.astype(np.float64); u = up(u); v = up(v)
    r = y + 1.402 * v; g = y - 0.344 * u - 0.714 * v; b = y + 1.772 * u
    return np.clip(np.stack([r, g, b], -1) + 0.5, 0, 255).astype(np.uint8)

def decode_file(path, outdir=None):
    data = open(path, 'rb').read()
    magic, ver, _, w, h, fps, nframes, rate, ch, npred, maxp, size, maxaf = \
        struct.unpack('>4sHHHHIIIHHIII', data[:40])
    if magic != b'K64V' or ver != 3: sys.exit("k64vid: not a K64V v3 file")
    book = []
    for i in range(npred):
        e = struct.unpack('>16h', data[64 + 32 * i:96 + 32 * i])
        book.append((list(e[:8]), list(e[8:])))
    astate = [0] * 8
    mbh, mbw = h // 16, w // 16
    ref = np.zeros((mbh, mbw, 6, 8, 8), dtype=np.int64)
    pos, bad = 64 + 32 * npred, 0
    zz = np.array(ZIGZAG)
    for f in range(nframes):
        nxt, nbits, naf, crc, acrc = struct.unpack('>IIIII', data[pos:pos + 20])
        astart = pos + 32
        if naf > maxaf: raise ValueError("audio frames over the header's maximum")
        smp, astate = decode_audio(book, data[astart:astart + naf * 9], astate)
        if fnv1a_s16(smp) != acrc:
            bad += 1; print("frame %d audio crc mismatch" % f)
        bits = data[astart + (naf * 9 + 15) // 16 * 16:][:nbits]
        br = BitReader(bits)
        is_p = br.get(1); q = br.get(5)
        step = q * WEIGHT
        cur = np.empty_like(ref)
        planes = mb_planes(ref, mbh, mbw)
        for my in range(mbh):
            pv = (0, 0)
            for mx in range(mbw):
                mode = br.ue() if is_p else 2
                if mode > 3: raise ValueError("bad mode")
                prev = ref[my, mx]
                if mode == 3:
                    dx, dy = pv[0] + br.se(), pv[1] + br.se()
                    if not (0 <= mx * 16 + dx <= w - 16 and 0 <= my * 16 + dy <= h - 16):
                        raise ValueError("motion vector points outside the frame")
                    prev = mc_block(planes, mx, my, dx, dy)
                    pv = (dx, dy)
                else:
                    pv = (0, 0)
                if mode == 0:
                    cur[my, mx] = prev; continue
                cbp = br.get(6)
                for k in range(6):
                    base = 128 if mode == 2 else prev[k]
                    if not cbp & (0x20 >> k):
                        cur[my, mx, k] = base; continue
                    lev = np.zeros((8, 8), dtype=np.int64)
                    i = -1
                    for _ in range(br.ue() + 1):
                        i += br.ue() + 1
                        l = br.ue() + 1
                        if br.get(1): l = -l
                        if i > 63: raise ValueError("coefficient index past 63")
                        lev[zz[i][0], zz[i][1]] = l
                    cur[my, mx, k] = np.clip(idct(dequant(lev, step)[None])[0] + base, 0, 255)
        ref = cur
        planes = mb_planes(ref, mbh, mbw)
        got = fnv1a(*planes)
        if got != crc:
            bad += 1; print("frame %d crc %08x want %08x" % (f, got, crc))
        if outdir:
            from PIL import Image
            os.makedirs(outdir, exist_ok=True)
            Image.fromarray(yuv_to_rgb(*planes)).save(os.path.join(outdir, 'd%04d.png' % f))
        pos += size; size = nxt
    print("k64vid: decoded %d frames, %d crc mismatches" % (nframes, bad))
    return bad

def encode(frames, nframes, fps, rate=0, pcm=None, q=8, gop=48, rd=0.85, search=12, npred=4):
    """Encode nframes RGB frames (iterable of uint8 [h, w, 3] arrays, w and h multiples of 16)
    and optional mono s16 audio (pcm: int array at rate Hz) into K64V file bytes.
    Returns (bytes, stats dict)."""
    lam = rd * q * q                     # SSE per bit; the step is q in pixel units

    # Audio frames of video frame f: those that end by sample round((f + 1) * rate / fps); the
    # last packet takes the rest (the tail padded with silence to a whole frame).
    cb, aframes, adec, af_end = b'', b'', [], []
    if pcm is not None:
        pcm = np.asarray(pcm, dtype=np.int64)
        ends = [int(round((f + 1) * rate / fps)) // 16 for f in range(nframes)]
        ends[-1] = max(ends[-1], (len(pcm) + 15) // 16)
        af_end = ends
        cb, aframes, adec = encode_audio(pcm, ends[-1], npred)
    packets, ref, sse = [], None, 0.0
    w = h = None
    for f, rgb in enumerate(frames):
        if f >= nframes: break
        if w is None:
            h, w = rgb.shape[:2]
            if w % 16 or h % 16: raise ValueError("k64vid: frame size must be a multiple of 16")
        elif rgb.shape[:2] != (h, w): raise ValueError("k64vid: frame %d has a different size" % f)
        src = mb_blocks(*rgb_to_yuv(rgb))
        bits, ref = encode_frame(src, ref, q, f % gop != 0, lam, search)
        crc = fnv1a(*mb_planes(ref, h // 16, w // 16))
        sse += float(((src - ref) ** 2).sum())
        aud, naf, acrc = b'', 0, 0
        if pcm is not None:
            f0 = af_end[f - 1] if f else 0
            naf = af_end[f] - f0
            aud = aframes[f0 * 9:af_end[f] * 9]
            acrc = fnv1a_s16(adec[f0 * 16:af_end[f] * 16])
        aud += b'\0' * (-len(aud) % 16)
        body = aud + bits
        body += b'\0' * (-len(body) % 16)
        packets.append([len(bits), naf, crc, acrc, body])
        print("frame %3d %s %6d bytes" % (f, 'P' if f % gop else 'I', len(bits)), file=sys.stderr) if os.environ.get('K64VID_VERBOSE') else None
    if len(packets) != nframes: raise ValueError("k64vid: got %d frames, expected %d" % (len(packets), nframes))

    sizes = [32 + len(p[4]) for p in packets]
    npred = len(cb) // 32
    out = bytearray(struct.pack('>4sHHHHIIIHHIII', b'K64V', 3, 0, w, h, int(round(fps * 65536)),
                                len(packets), rate if pcm is not None else 0, 1 if pcm is not None else 0,
                                npred, max(sizes), sizes[0], max(p[1] for p in packets)))
    out += b'\0' * (64 - len(out)) + cb
    for i, (nbits, naf, crc, acrc, body) in enumerate(packets):
        nxt = sizes[i + 1] if i + 1 < len(packets) else 0
        out += struct.pack('>IIIII12x', nxt, nbits, naf, crc, acrc) + body
    tot = sum(p[0] for p in packets)
    st = dict(width=w, height=h, frames=len(packets), video_bytes=tot, audio_bytes=len(aframes),
              file_bytes=len(out), psnr=10 * np.log10(255 ** 2 * len(packets) * w * h * 1.5 / max(sse, 1e-9)))
    if pcm is not None:
        x = pcm.astype(np.float64)
        d = np.array(adec[:len(pcm)], dtype=np.float64)
        st['audio_snr'] = 10 * np.log10((x ** 2).sum() / max(((x - d) ** 2).sum(), 1e-9))
    return bytes(out), st

def main():
    if len(sys.argv) >= 3 and sys.argv[1] == '--decode':
        sys.exit(1 if decode_file(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None) else 0)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out')
    ap.add_argument('--frames', required=True)
    ap.add_argument('--fps', type=float, default=24.0)
    ap.add_argument('--rd', type=float, default=0.85, help='rate-distortion weight (x q^2 SSE per bit)')
    ap.add_argument('--q', type=int, default=8, help='quantiser scale 1..31 (higher = smaller, blurrier)')
    ap.add_argument('--gop', type=int, default=48, help='frames between I frames')
    ap.add_argument('--search', type=int, default=12, help='motion search range in pixels (0: no motion vectors)')
    ap.add_argument('--audio')
    ap.add_argument('--npred', type=int, default=4, help='VADPCM predictors 1..8')
    a = ap.parse_args()
    if not 1 <= a.q <= 31: sys.exit("k64vid: --q must be 1..31")
    if not 1 <= a.npred <= 8: sys.exit("k64vid: --npred must be 1..8")
    from PIL import Image
    names = sorted(f for f in os.listdir(a.frames) if f.lower().endswith('.png'))
    if not names: sys.exit("k64vid: no PNG frames in " + a.frames)
    rate, pcm = load_wav(a.audio) if a.audio else (0, None)
    frames = (np.asarray(Image.open(os.path.join(a.frames, n)).convert('RGB')) for n in names)
    try:
        out, st = encode(frames, len(names), a.fps, rate, pcm, a.q, a.gop, a.rd, a.search, a.npred)
    except ValueError as e:
        sys.exit(str(e))
    with open(a.out, 'wb') as fo: fo.write(out)
    print("k64vid: %s %dx%d %d frames, video %d KB (%.1f KB/s), audio %d KB, file %d KB, YUV PSNR %.2f dB" %
          (a.out, st['width'], st['height'], st['frames'], st['video_bytes'] >> 10,
           st['video_bytes'] / 1024 * a.fps / st['frames'], st['audio_bytes'] >> 10, st['file_bytes'] >> 10, st['psnr']))
    if pcm is not None:
        print("k64vid: audio %d Hz, %d VADPCM frames, SNR %.1f dB" % (rate, st['audio_bytes'] // 9, st['audio_snr']))

if __name__ == '__main__':
    main()
