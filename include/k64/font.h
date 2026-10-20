// kestrel64-sdk — proportional fonts (.k64f, baked by `k64bake.py font` / k64_add_rom FONTS).
//
// Glyphs are I4 (16 alpha levels) on sheets of up to 128x64 = all of TMEM. A draw call lays
// the string out once, then per sheet it uses: one TMEM load (skipped when already resident)
// and one textured rectangle per glyph. Antialiased fonts blend with the framebuffer;
// 1-bit fonts use alpha compare (no framebuffer read). Advances and kerning are in quarter
// pixels, glyphs land on whole pixels.
//
// Strings are UTF-8. '\n' starts a new line at the starting x. Bytes 0x01..0x08 switch the
// color to palette entry 0..7 (entry 0 = the color passed to the call), e.g.
// "Press \x02A\x01 to start" with k64_font_set_palette(2 -> green).
#pragma once
#include "hw.h"
#include "tex.h"

#define K64_FONT_MAX_SHEETS 8

typedef struct {
  u8 sx, sy, w, h;              // on the sheet
  s8 xoff, yoff;                // from the pen and from the line top
  u8 advance;                   // quarter pixels
  u8 sheet;
} k64_glyph_t;

typedef struct {
  const u16 *codepoints;        // ascending
  const k64_glyph_t *glyphs;
  const u32 *kern;              // left << 20 | right << 8 | (s8 adjust, quarter px), ascending
  const u16 *kern_first;        // [glyph]: first pair with that left glyph (nglyphs + 1)
  u16 nglyphs, nkern, line_height, ascent, fallback;
  u8 nsheets;
  bool mono, ascii;             // ascii: codepoints[i] == 32 + i for 32..126
  k64_tex_t sheets[K64_FONT_MAX_SHEETS];
} k64_font_t;

enum { K64_ALIGN_LEFT = 0, K64_ALIGN_CENTER = 1, K64_ALIGN_RIGHT = 2 };

// Parse a resident .k64f blob (8-byte aligned). Returns false on a bad header.
bool k64_font_from_blob(k64_font_t *f, const void *blob);

// Glyph index for a codepoint (the font's '?' when missing).
u32 k64_font_glyph(const k64_font_t *f, u32 cp);

// Width in pixels of the first line of `s` (up to '\n' or the end).
int k64_font_width(const k64_font_t *f, const char *s);

// Palette for the 0x02..0x08 color codes (entries 1..7). Colors are RGBA8888.
void k64_font_set_palette(u32 index, u32 rgba);

// Draws at (x, y) = top-left of the first line, 1-cycle mode, color = prim * glyph alpha.
// Changes other-modes/combine/prim. Returns the pen x after the last glyph.
int k64_font_draw(const k64_font_t *f, int x, int y, u32 rgba, const char *s);
// Same with up to `len` bytes of s.
int k64_font_draw_n(const k64_font_t *f, int x, int y, u32 rgba, const char *s, u32 len);
int k64_font_printf(const k64_font_t *f, int x, int y, u32 rgba, const char *fmt, ...) __attribute__((format(printf, 5, 6)));

// Word-wrapped paragraph in a box `w` pixels wide, each line aligned. Breaks at spaces
// (a word wider than the box is split), honours '\n'. Returns the height used.
// With draw = false only measures.
int k64_font_box(const k64_font_t *f, int x, int y, int w, u32 align, u32 rgba, const char *s, bool draw);

// Drop shadow for the following draws: offset in pixels and color (alpha 0 = off).
void k64_font_set_shadow(int dx, int dy, u32 rgba);
