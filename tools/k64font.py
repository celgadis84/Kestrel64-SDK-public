#!/usr/bin/env python3
"""kestrel64-sdk font baker: TrueType/OpenType -> .k64f (I4 glyph sheets + metrics + kerning).

    k64bake.py font in.ttf out.k64f --size 16 [--chars 32-126,160-255] [--mono] [--preview p]

.k64f layout (big-endian, 8-byte aligned sections):
  0  'K6F1'
  4  u8 flags (bit0: mono, 1-bit alpha)   5 u8 sheets   6 u16 glyphs
  8  u16 line_height   10 u16 ascent   12 u16 kern pairs   14 u16 fallback glyph
  16 u32 off codepoints (u16 x glyphs, ascending)
  20 u32 off glyphs     (8 bytes each: sx sy w h  s8 xoff s8 yoff  u8 advance/4px  u8 sheet)
  24 u32 off kerning    (u32 each: left glyph << 20 | right glyph << 8 | s8 adjust/4px,
                         ascending; then u16 x (glyphs + 1): first pair of each left glyph)
  28 u32 off sheets     (8 bytes each: u16 width, u16 height, u32 offset of the I4 texels)
  .. data; each sheet's texels 8-byte aligned, rows of width/2 bytes.

Sheets are at most 128x64 I4 (4 KB, all of TMEM): the runtime loads one and draws every
glyph of the string that lives on it, then the next one. Glyphs are shelf-packed, ASCII
first and tallest first, so the common case (ASCII at text sizes) needs one sheet.
Advances and kerning (GPOS 'kern' feature or the legacy kern table) are kept in quarter
pixels; the pen is rounded to a whole pixel per glyph, so spacing stays true to the font
without filtering the glyph bitmaps.
"""
import struct

SHEET_W, SHEET_H = 128, 64


def parse_chars(spec):
    out = []
    for part in spec.split(','):
        if '-' in part:
            a, b = part.split('-')
            out += range(int(a, 0), int(b, 0) + 1)
        else:
            out.append(int(part, 0))
    return sorted(set(out))


def render(path, size, chars, mono):
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    glyphs = []
    for cp in chars:
        ch = chr(cp)
        if font.getmask(ch).size == (0, 0) and not ch.isspace() and cp != 0xA0:
            continue                    # not in the font
        l, t, r, b = font.getbbox(ch, anchor='ls')
        adv = font.getlength(ch)
        w, h = max(r - l, 0), max(b - t, 0)
        img = None
        if not (w and h):
            w = h = 0
        else:
            img = Image.new('L', (w, h), 0)
            d = ImageDraw.Draw(img)
            if mono:
                d.fontmode = '1'
            d.text((-l, -t), ch, font=font, fill=255, anchor='ls')
            bb = img.getbbox()
            if bb is None:
                img, w, h = None, 0, 0
            else:                       # trim empty margins the bbox estimate left
                img = img.crop(bb)
                l, t = l + bb[0], t + bb[1]
                w, h = img.size
        glyphs.append(dict(cp=cp, img=img, w=w, h=h, xoff=l, yoff=ascent + t, adv=adv))
    return font, ascent, descent, glyphs


class _Sfnt:
    """Just enough of the TrueType/OpenType container to read pair kerning."""

    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        n = struct.unpack_from('>H', d, 4)[0]
        self.tables = {d[12 + 16 * i:16 + 16 * i].decode('latin-1'): struct.unpack_from('>I', d, 20 + 16 * i)[0]
                       for i in range(n)}

    def u16(self, o): return struct.unpack_from('>H', self.d, o)[0]
    def s16(self, o): return struct.unpack_from('>h', self.d, o)[0]
    def u32(self, o): return struct.unpack_from('>I', self.d, o)[0]

    def cmap(self):
        """codepoint -> glyph id (Unicode subtable, format 4 or 12)."""
        base = self.tables['cmap']
        best = None
        for i in range(self.u16(base + 2)):
            pid, eid, off = self.u16(base + 4 + 8 * i), self.u16(base + 6 + 8 * i), self.u32(base + 8 + 8 * i)
            fmt = self.u16(base + off)
            if (pid == 0 or (pid == 3 and eid in (1, 10))) and fmt in (4, 12):
                if best is None or fmt == 12:
                    best = base + off
        m = {}
        if best is None:
            return m
        if self.u16(best) == 12:
            for i in range(self.u32(best + 12)):
                a, b, g = struct.unpack_from('>III', self.d, best + 16 + 12 * i)
                for c in range(a, min(b, 0x10FFFF) + 1):
                    m[c] = g + c - a
            return m
        seg = self.u16(best + 6) // 2
        ends, starts = best + 14, best + 16 + 2 * seg
        deltas, ranges = starts + 2 * seg, starts + 4 * seg
        for i in range(seg):
            e, st = self.u16(ends + 2 * i), self.u16(starts + 2 * i)
            dl, ro = self.s16(deltas + 2 * i), self.u16(ranges + 2 * i)
            for c in range(st, e + 1):
                if c == 0xFFFF:
                    continue
                if ro:
                    g = self.u16(ranges + 2 * i + ro + 2 * (c - st))
                    g = (g + dl) & 0xFFFF if g else 0
                else:
                    g = (c + dl) & 0xFFFF
                if g:
                    m[c] = g
        return m

    def coverage(self, o):
        if self.u16(o) == 1:
            return {self.u16(o + 4 + 2 * i): i for i in range(self.u16(o + 2))}
        cov = {}
        for i in range(self.u16(o + 2)):
            a, b, idx = struct.unpack_from('>HHH', self.d, o + 4 + 6 * i)
            for g in range(a, b + 1):
                cov[g] = idx + g - a
        return cov

    def classdef(self, o):
        cls = {}
        if self.u16(o) == 1:
            first = self.u16(o + 2)
            for i in range(self.u16(o + 4)):
                cls[first + i] = self.u16(o + 6 + 2 * i)
        else:
            for i in range(self.u16(o + 2)):
                a, b, c = struct.unpack_from('>HHH', self.d, o + 4 + 6 * i)
                for g in range(a, b + 1):
                    cls[g] = c
        return cls

    def gpos_pairs(self, wanted):
        """{(left gid, right gid): x advance adjust in font units} from the GPOS 'kern'
        feature, for gids in `wanted`. The first subtable covering a pair decides it
        (OpenType lookup semantics)."""
        if 'GPOS' not in self.tables:
            return {}
        g = self.tables['GPOS']
        feats, lookups = g + self.u16(g + 6), g + self.u16(g + 8)
        idx = []
        for i in range(self.u16(feats)):
            if self.d[feats + 2 + 6 * i:feats + 6 + 6 * i] == b'kern':
                f = feats + self.u16(feats + 6 + 6 * i)
                idx += [self.u16(f + 4 + 2 * k) for k in range(self.u16(f + 2))]
        subs = []
        for li in sorted(set(idx)):
            lk = lookups + self.u16(lookups + 2 + 2 * li)
            typ = self.u16(lk)
            for k in range(self.u16(lk + 4)):
                st = lk + self.u16(lk + 6 + 2 * k)
                if typ == 9:                        # extension lookup wrapping a pair one
                    if self.u16(st + 2) != 2:
                        continue
                    st += self.u32(st + 4)
                elif typ != 2:
                    continue
                subs.append(st)
        out, done = {}, set()
        for st in subs:
            fmt, vf1, vf2 = self.u16(st), self.u16(st + 4), self.u16(st + 6)
            cov = self.coverage(st + self.u16(st + 2))
            n1, n2 = bin(vf1).count('1') * 2, bin(vf2).count('1') * 2
            # XAdvance of the first value record, after XPlacement/YPlacement if present
            xa = 2 * bin(vf1 & 3).count('1') if vf1 & 4 else None
            if fmt == 1:
                for a in wanted:
                    if a not in cov:
                        continue
                    ps = st + self.u16(st + 10 + 2 * cov[a])
                    for r in range(self.u16(ps)):
                        rec = ps + 2 + r * (2 + n1 + n2)
                        b = self.u16(rec)
                        if b in wanted and (a, b) not in done:
                            done.add((a, b))
                            v = self.s16(rec + 2 + xa) if xa is not None else 0
                            if v:
                                out[(a, b)] = v
            elif fmt == 2:
                c1, c2 = self.classdef(st + self.u16(st + 8)), self.classdef(st + self.u16(st + 10))
                n2c = self.u16(st + 14)
                for a in wanted:
                    if a not in cov:
                        continue
                    for b in wanted:
                        if (a, b) in done:
                            continue
                        done.add((a, b))
                        if xa is None:
                            continue
                        v = self.s16(st + 16 + (c1.get(a, 0) * n2c + c2.get(b, 0)) * (n1 + n2) + xa)
                        if v:
                            out[(a, b)] = v
        return out

    def kern_pairs(self, wanted):
        """Legacy 'kern' table, format 0 horizontal subtables."""
        out = {}
        if 'kern' not in self.tables:
            return out
        o = self.tables['kern'] + 4
        for _ in range(self.u16(o - 2)):
            length, cov = self.u16(o + 2), self.u16(o + 4)
            if cov >> 8 == 0 and cov & 1:
                for i in range(self.u16(o + 6)):
                    a, b, v = struct.unpack_from('>HHh', self.d, o + 14 + 6 * i)
                    if a in wanted and b in wanted and v:
                        out[(a, b)] = v
            o += length
        return out


def kerning(path, size, glyphs):
    """[(left index, right index, adjust in quarter pixels)] for the baked glyphs."""
    f = _Sfnt(path)
    upem = f.u16(f.tables['head'] + 18)
    cmap = f.cmap()
    gid = {i: cmap[g['cp']] for i, g in enumerate(glyphs) if g['cp'] in cmap}
    wanted = set(gid.values())
    units = f.gpos_pairs(wanted) or f.kern_pairs(wanted)
    pairs = []
    for i, a in gid.items():
        for j, b in gid.items():
            v = units.get((a, b))
            if v:
                k = round(v * size * 4 / upem)
                if k:
                    pairs.append((i, j, max(-128, min(127, k))))
    return pairs


def pack_sheets(glyphs):
    """Shelf packing, tallest first. Sets g['sheet'], g['sx'], g['sy']; returns sheet heights."""
    order = sorted((g for g in glyphs if g['w']), key=lambda g: (g['cp'] > 126, -g['h'], -g['w']))  # ASCII first
    sheets = []                         # [used height]
    x = y = shelf = 0
    for g in order:
        if g['w'] > SHEET_W or g['h'] > SHEET_H:
            raise SystemExit('k64font: glyph U+%04X is %dx%d, over %dx%d: smaller --size'
                             % (g['cp'], g['w'], g['h'], SHEET_W, SHEET_H))
        if not sheets:
            sheets.append(0)
        if x + g['w'] > SHEET_W:
            x, y, shelf = 0, y + shelf, 0
        if y + g['h'] > SHEET_H:
            sheets.append(0)
            x = y = shelf = 0
        g['sheet'], g['sx'], g['sy'] = len(sheets) - 1, x, y
        x += g['w']
        shelf = max(shelf, g['h'])
        sheets[-1] = max(sheets[-1], y + g['h'])
    for g in glyphs:
        if not g['w']:
            g['sheet'] = g['sx'] = g['sy'] = 0
    return sheets


def bake(src, dst, size, chars='32-126', mono=False, preview=None, line_gap=0):
    font, ascent, descent, glyphs = render(src, size, parse_chars(chars), mono)
    if not glyphs:
        raise SystemExit('k64font: no glyphs')
    pairs = kerning(src, size, glyphs)
    heights = pack_sheets(glyphs)
    from PIL import Image
    sheet_imgs = [Image.new('L', (SHEET_W, max(h, 1)), 0) for h in heights]
    for g in glyphs:
        if g['img']:
            sheet_imgs[g['sheet']].paste(g['img'], (g['sx'], g['sy']))
    q = lambda v: (v * 15 + 127) // 255
    flags = 1 if mono else 0
    if not mono and all(q(p) in (0, 15) for im in sheet_imgs for p in im.tobytes()):
        flags = 1                       # a pixel font rendered at its native size
    fallback = next((i for i, g in enumerate(glyphs) if g['cp'] == ord('?')), 0)

    def align8(b):
        b += bytes(-len(b) % 8)

    body = bytearray(32)
    off_cp = len(body)
    for g in glyphs:
        body += struct.pack('>H', g['cp'])
    align8(body)
    off_gl = len(body)
    for g in glyphs:
        adv = round(g['adv'] * 4)
        if adv > 255 or not -128 <= g['xoff'] < 128 or not -128 <= g['yoff'] < 128:
            raise SystemExit('k64font: U+%04X metrics out of range: smaller --size' % g['cp'])
        body += struct.pack('>BBBBbbBB', g['sx'], g['sy'], g['w'], g['h'], g['xoff'], g['yoff'], adv, g['sheet'])
    align8(body)
    off_kn = len(body)
    pairs.sort()
    for i, j, k in pairs:
        body += struct.pack('>I', i << 20 | j << 8 | (k & 0xFF))
    first = 0
    for g in range(len(glyphs) + 1):        # pairs of left glyph g: [start[g], start[g + 1])
        while first < len(pairs) and pairs[first][0] < g:
            first += 1
        body += struct.pack('>H', first)
    align8(body)
    off_sh = len(body)
    body += bytes(8 * len(heights))
    for n, im in enumerate(sheet_imgs):
        align8(body)
        struct.pack_into('>HHI', body, off_sh + 8 * n, SHEET_W, im.size[1], len(body))
        px = [q(p) for p in im.tobytes()]
        for y in range(im.size[1]):
            row = px[y * SHEET_W:(y + 1) * SHEET_W]
            body += bytes(row[x] << 4 | row[x + 1] for x in range(0, SHEET_W, 2))
    align8(body)
    struct.pack_into('>4sBBHHHHHIIII', body, 0, b'K6F1', flags, len(heights), len(glyphs),
                     ascent + descent + line_gap, ascent, len(pairs), fallback, off_cp, off_gl, off_kn, off_sh)
    with open(dst, 'wb') as f:
        f.write(body)
    if preview:
        for n, im in enumerate(sheet_imgs):
            im.point(lambda v: q(v) * 17).save('%s_sheet%d.png' % (preview, n))
    print('k64font: %s %dpx  %d glyphs, %d kern pairs, %d sheet(s), line %d%s -> %d bytes'
          % (src, size, len(glyphs), len(pairs), len(heights), ascent + descent + line_gap,
             ', mono' if flags & 1 else '', len(body)))
