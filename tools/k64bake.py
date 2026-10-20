#!/usr/bin/env python3
"""kestrel64-sdk asset baker.

  k64bake.py tex  in.png out.k64t --format rgba16|rgba32|yuv16|ci4|ci8|ci4ia|ci8ia|i4|i8|ia4|ia8|ia16
  k64bake.py embed out.S --section rodata|rom name=file [name=file ...]
  k64bake.py model in.glb out.k64m [--tris N] [--tex 64x64:ci4] [--fps 30]   (tools/k64model.py)
  k64bake.py font in.ttf out.k64f --size 16 [--chars 32-126] [--mono]          (tools/k64font.py)

.k64t layout (big-endian, 32-byte header, data 8-byte aligned, TMEM-ready):
  0  'K6T1'    4 u16 width   6 u16 height
  8  u8 fmt    9 u8 siz      10 u8 tlut_count/16  11 u8 flags (bit 0: TLUT entries are IA16)
  12 u16 line_bytes (row stride, multiple of 8 = one TMEM line unit; 16 for rgba32 and
     yuv16, which the RDP splits across the two TMEM halves)
  16 u32 pixel_bytes         20..31 reserved
  32 pixels  [pad to 8]  tlut (u16 RGBA5551 x tlut_count)
"""
import argparse, os, struct, sys

FMT = {'rgba': 0, 'yuv': 1, 'ci': 2, 'ia': 3, 'i': 4}
SIZ = {4: 0, 8: 1, 16: 2, 32: 3}


def rgba5551(r, g, b, a):
    return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (1 if a >= 128 else 0)


def pack_rows(rows, bits, width, align=8):
    """rows: list of lists of ints (texel values); returns bytes padded per row to align."""
    out = bytearray()
    row_bytes = (width * bits + 7) // 8
    line = (row_bytes + align - 1) & ~(align - 1)
    for row in rows:
        b = bytearray()
        if bits == 4:
            for x in range(0, width, 2):
                hi = row[x] & 15
                lo = (row[x + 1] & 15) if x + 1 < width else 0
                b.append((hi << 4) | lo)
        elif bits == 8:
            b.extend(v & 0xFF for v in row)
        elif bits == 16:
            for v in row:
                b += struct.pack('>H', v & 0xFFFF)
        elif bits == 32:
            for v in row:
                b += struct.pack('>I', v & 0xFFFFFFFF)
        b += bytes(line - len(b))
        out += b
    return bytes(out), line


def bake_tex(src, dst, fmt):
    from PIL import Image
    blob = tex_blob(Image.open(src), fmt, src)
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, 'wb') as fo:
        fo.write(blob)


def tex_blob(im, fmt, src='texture'):
    """PIL image -> .k64t bytes."""
    from PIL import Image
    im = im.convert('RGBA')
    w, h = im.size
    px = im.load()
    tlut = []
    flags, align = 0, 8
    if fmt == 'rgba16':
        rows = [[rgba5551(*px[x, y]) for x in range(w)] for y in range(h)]
        f, bits = 'rgba', 16
    elif fmt == 'rgba32':
        rows = [[(px[x, y][0] << 24) | (px[x, y][1] << 16) | (px[x, y][2] << 8) | px[x, y][3] for x in range(w)] for y in range(h)]
        f, bits, align = 'rgba', 32, 16
    elif fmt == 'yuv16':
        # Full-range BT.601, chroma averaged over each horizontal pixel pair; RDRAM words are
        # U Y0 V Y1 (n64brew "Texture formats"). Alpha is dropped. The RDP converts back with
        # the SET_CONVERT factors of K64_YUV_CONVERT (k64/tex.h).
        if w & 1:
            sys.exit('k64bake: yuv16 needs an even width (%s)' % src)
        def yuv(p):
            r, g, b = p[0], p[1], p[2]
            return (0.299 * r + 0.587 * g + 0.114 * b,
                    -0.168736 * r - 0.331264 * g + 0.5 * b + 128,
                    0.5 * r - 0.418688 * g - 0.081312 * b + 128)
        c8 = lambda v: max(0, min(255, int(round(v))))
        rows = []
        for y in range(h):
            row = []
            for x in range(0, w, 2):
                a, b = yuv(px[x, y]), yuv(px[x + 1, y])
                u, v = c8((a[1] + b[1]) / 2), c8((a[2] + b[2]) / 2)
                row += [(u << 8) | c8(a[0]), (v << 8) | c8(b[0])]
            rows.append(row)
        f, bits, align = 'yuv', 16, 16
    elif fmt in ('ci4', 'ci8', 'ci4ia', 'ci8ia'):
        bits = 4 if fmt.startswith('ci4') else 8
        ia_tlut = fmt.endswith('ia')
        ncol = 16 if bits == 4 else 256
        has_alpha = not ia_tlut and any(px[x, y][3] < 128 for y in range(h) for x in range(w))
        if ia_tlut:
            # IA16 palette (other mode TLUT_IA): entries are intensity << 8 | alpha, so the
            # indices quantise (intensity, alpha) pairs: carried as an RGB image (i, a, 0).
            im = Image.merge('RGBA', (im.convert('L'), im.getchannel('A'), Image.new('L', (w, h)), Image.new('L', (w, h), 255)))
            px = im.load()
            flags = 1
        rgb = Image.new('RGB', (w, h))
        rgb.paste(im.convert('RGB'))
        if rgb.getcolors(ncol - (1 if has_alpha else 0)) is not None:
            # few enough colours already: exact palette (median cut could merge some)
            cols = sorted(c for _, c in rgb.getcolors(ncol))
            q = Image.new('P', (w, h))
            q.putpalette([v for c in cols for v in c])
            idx = {c: i for i, c in enumerate(cols)}
            q.putdata([idx[c] for c in rgb.getdata()])
        else:
            q = rgb.quantize(colors=ncol - (1 if has_alpha else 0), method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        pal = q.getpalette()[: 3 * (ncol - (1 if has_alpha else 0))]
        qi = q.load()
        base = 1 if has_alpha else 0
        if ia_tlut:
            tlut = [(pal[3 * i] << 8) | pal[3 * i + 1] for i in range(len(pal) // 3)]
        else:
            tlut = ([0] if has_alpha else []) + [rgba5551(pal[3 * i], pal[3 * i + 1], pal[3 * i + 2], 255) for i in range(len(pal) // 3)]
        tlut += [0] * (ncol - len(tlut))
        rows = [[0 if has_alpha and px[x, y][3] < 128 else qi[x, y] + base for x in range(w)] for y in range(h)]
        f = 'ci'
    elif fmt in ('i4', 'i8'):
        bits = 4 if fmt == 'i4' else 8
        sh = 8 - bits
        rows = [[((px[x, y][0] * 299 + px[x, y][1] * 587 + px[x, y][2] * 114) // 1000) >> sh for x in range(w)] for y in range(h)]
        f = 'i'
    elif fmt in ('ia4', 'ia8', 'ia16'):
        bits = int(fmt[2:])
        def ia(p):
            i = (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000
            a = p[3]
            if bits == 4: return ((i >> 5) << 1) | (1 if a >= 128 else 0)
            if bits == 8: return ((i >> 4) << 4) | (a >> 4)
            return (i << 8) | a
        rows = [[ia(px[x, y]) for x in range(w)] for y in range(h)]
        f = 'ia'
    else:
        sys.exit('k64bake: unknown format ' + fmt)

    data, line = pack_rows(rows, bits, w, align)
    tmem_bytes = line * h      # RGBA32, YUV16: line / 2 in each TMEM half
    limit = 2048 if f == 'ci' else 4096
    if tmem_bytes > limit:
        print('k64bake: warning: %s needs %d bytes of TMEM (limit %d) — split it' % (src, tmem_bytes, limit), file=sys.stderr)
    hdr = struct.pack('>4sHHBBBBHHI12x', b'K6T1', w, h, FMT[f], SIZ[bits], len(tlut) // 16, flags, line, 0, len(data))
    body = data + bytes((-len(data)) & 7)
    for c in tlut:
        body += struct.pack('>H', c)
    return hdr + body


def embed(out, section, pairs):
    sec = '.rodata.k64asset' if section == 'rodata' else '.rom'
    flags = '"a"'
    lines = ['# generated by k64bake.py embed — do not edit']
    for p in pairs:
        name, path = p.split('=', 1)
        path = os.path.abspath(path).replace('\\', '/')
        lines += ['    .section %s.%s, %s' % (sec, name, flags) if sec != '.rom' else '    .section .rom.%s, "a"' % name,
                  '    .balign 16', '    .global %s' % name, '    .global %s_end' % name,
                  '%s:' % name, '    .incbin "%s"' % path, '%s_end:' % name, '    .balign 8']
    with open(out, 'w') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    t = sp.add_parser('tex'); t.add_argument('src'); t.add_argument('dst'); t.add_argument('--format', default='rgba16')
    e = sp.add_parser('embed'); e.add_argument('out'); e.add_argument('--section', default='rodata'); e.add_argument('pairs', nargs='+')
    m = sp.add_parser('model'); m.add_argument('src'); m.add_argument('dst')
    m.add_argument('--tris', type=int, default=0, help='decimate to about this many triangles (0 = keep)')
    m.add_argument('--lods', type=lambda s: [int(x) for x in s.split(',')], default=[],
                   help='extra levels of detail: triangle counts, decreasing (e.g. 1200,500)')
    m.add_argument('--tex', default='64x64:ci4', help='atlas size and format, WxH:fmt')
    m.add_argument('--fps', type=int, default=30, help='animation sample rate')
    m.add_argument('--preview', help='write preview PNGs (atlas, posed frames) with this path prefix')
    m.add_argument('--flip', action='store_true', help='reverse the winding (glTF and kgfx: counter-clockwise = front)')
    s = sp.add_parser('adpcm'); s.add_argument('src'); s.add_argument('dst')
    s.add_argument('--loop', type=int, help='loop start sample (default: one-shot)')
    s.add_argument('--npred', type=int, default=4, help='predictors in the codebook (1..8)')
    f = sp.add_parser('font'); f.add_argument('src'); f.add_argument('dst')
    f.add_argument('--size', type=int, default=16, help='pixel size (TrueType em)')
    f.add_argument('--chars', default='32-126', help='codepoints: ranges, e.g. 32-126,160-255')
    f.add_argument('--mono', action='store_true', help='1-bit glyphs (no antialiasing)')
    f.add_argument('--line-gap', type=int, default=0, help='extra pixels between lines')
    f.add_argument('--preview', help='write the glyph sheets as PNGs with this path prefix')
    q = sp.add_parser('music'); q.add_argument('src'); q.add_argument('dst')
    q.add_argument('--loop', action='store_true', help='loop the whole song (without loop markers)')
    q.add_argument('--loop-end', type=int, help='with --loop: loop end tick (default: end of the last bar)')
    i = sp.add_parser('impostor'); i.add_argument('src'); i.add_argument('dst')
    import k64impostor
    k64impostor.add_args(i)
    a = ap.parse_args()
    if a.cmd == 'impostor':
        k64impostor.bake(a)
    elif a.cmd == 'adpcm':
        import k64adpcm
        k64adpcm.bake(a.src, a.dst, a.loop, a.npred)
    elif a.cmd == 'font':
        import k64font
        k64font.bake(a.src, a.dst, a.size, a.chars, a.mono, a.preview, a.line_gap)
    elif a.cmd == 'music':
        import k64music
        k64music.bake(a.src, a.dst, a.loop, a.loop_end)
    elif a.cmd == 'tex':
        bake_tex(a.src, a.dst, a.format)
    elif a.cmd == 'model':
        import k64model
        k64model.bake(a)
    else:
        embed(a.out, a.section, a.pairs)


if __name__ == '__main__':
    main()
