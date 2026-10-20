# Fonts

`k64/font.h` draws proportional, antialiased text with the RDP. Fonts are baked at build
time from TrueType/OpenType files into `.k64f` blobs and kept resident.

## Baking

```cmake
k64_add_rom(NAME game SOURCES main.c
  FONTS "body=${K64_SDK_DIR}/assets/fonts/Lato.ttf:--size 16 --chars 32-126,161-255"
        "pixel=${K64_SDK_DIR}/assets/fonts/m5x7.ttf:--size 16 --mono")
```

`extern const u8 body[];` then `k64_font_from_blob(&font, body)`. By hand:
`tools/k64bake.py font in.ttf out.k64f --size 16 [--chars 32-126,0x100-0x17F] [--mono]
[--line-gap N] [--preview sheets.png]`.

The baker (`tools/k64font.py`, needs only Pillow):

- rasterizes each glyph with Pillow, crops it to its ink, quantizes to 4-bit alpha (I4);
  a font whose pixels are all 0 or 15 is flagged mono automatically (pixel fonts);
- packs glyphs on 128x64 I4 sheets (4 KB = all of TMEM), ASCII first so plain text
  usually needs a single sheet;
- reads kerning itself from the GPOS `kern` feature (PairPos formats 1 and 2, extension
  lookups) or the legacy `kern` table, since Pillow's basic layout ignores it; pairs are
  stored in quarter pixels;
- uses `?` as the fallback glyph for missing codepoints.

## Format (big-endian)

| offset | field |
|--------|-------|
| 0 | `K6F1`, flags (bit 0 mono), sheet count, glyph count |
| 8 | line height, ascent, kern pair count, fallback glyph |
| 16 | offsets: codepoints (u16, ascending), glyphs, kerning, sheet table |

Glyph = 8 bytes: sheet x/y, w/h, x/y offset from the pen and the line top, advance in
quarter pixels, sheet. Kerning = u32 `left << 20 | right << 8 | s8 adjust`, ascending, then
a u16 per glyph (+1) with the first pair of each left glyph. Sheets are I4 rows of 64 bytes.

## Drawing

- `k64_font_draw / draw_n / printf(font, x, y, rgba, ...)`: (x, y) is the top-left of the
  first line; returns the pen x.
- `k64_font_box(font, x, y, w, align, rgba, s, draw)`: word wrap at spaces (a word wider
  than the box is split), `\n`, left/center/right; returns the height. `draw = false`
  measures only.
- `k64_font_width` measures the first line.
- In strings: UTF-8, `\n`, bytes `\x01..\x08` select palette entries 0..7 (0 = the call's
  color, 1..7 set with `k64_font_set_palette`).
- `k64_font_set_shadow(dx, dy, rgba)` adds a drop shadow (alpha 0 = off).

A call lays the whole string out first (pen in quarter pixels, kerning by binary search in
the left glyph's pair range, ASCII glyphs indexed directly), then per sheet it issues one
TMEM load (skipped by `k64_rdp_load_tex` when that sheet is already resident) and one
textured rectangle per glyph, shadow pass first. Glyphs land on whole pixels.

RDP modes, 1-cycle: color = PRIM, alpha = TEX0 × PRIM alpha.

- Antialiased fonts blend `IN·A + MEM·(1−A)` (framebuffer read). Alpha compare with
  threshold 1 drops the empty texels, so they write nothing.
- Mono fonts use alpha compare only. They do no framebuffer read, which makes them the
  cheap choice for HUD counters.

## Cost (demo 20, kestrel64)

The scene has 377 glyphs: two 16 px and 12 px paragraphs, a title with a shadow, a
centered box and a pixel-font line.

| | |
|---|---|
| CPU to lay out + emit | 1.9 ms (~5 µs/glyph) |
| RDP | 186k GCLK ≈ 3.0 ms |

Kerning was a binary search over all pairs. Indexing it per left glyph cut the CPU time
from 3.2 ms to 1.9 ms.

## Fonts shipped

`assets/fonts/`: Lato (SIL OFL 1.1) and m5x7 (Daniel Linssen, free with attribution).
See its README.
