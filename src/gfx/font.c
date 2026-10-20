// kestrel64-sdk — proportional fonts (see k64/font.h, tools/k64font.py).
#include <k64/font.h>
#include <k64/rdp.h>
#include <k64/debug.h>
#include <stdarg.h>

#define FONT_TILE 2
#define BATCH     128            // glyphs laid out before a flush

typedef struct { s16 x, y; u16 g; u8 color, sheet; } item_t;

static item_t batch[BATCH];
static u32 nbatch, sheet_mask;
static u32 palette[8];
static int shadow_dx, shadow_dy;
static u32 shadow_rgba;

typedef struct {
  char magic[4];
  u8 flags, nsheets;
  u16 nglyphs, line_height, ascent, nkern, fallback;
  u32 off_cp, off_glyph, off_kern, off_sheet;
} blob_hdr_t;

bool k64_font_from_blob(k64_font_t *f, const void *blob) {
  const blob_hdr_t *h = blob;
  const u8 *b = blob;
  if (__builtin_memcmp(h->magic, "K6F1", 4) || h->nsheets > K64_FONT_MAX_SHEETS) return false;
  f->codepoints = (const u16 *)(b + h->off_cp);
  f->glyphs = (const k64_glyph_t *)(b + h->off_glyph);
  f->kern = (const u32 *)(b + h->off_kern);
  f->kern_first = (const u16 *)(f->kern + h->nkern);
  f->nglyphs = h->nglyphs; f->nkern = h->nkern;
  f->line_height = h->line_height; f->ascent = h->ascent; f->fallback = h->fallback;
  f->nsheets = h->nsheets;
  f->mono = h->flags & 1;
  f->ascii = h->nglyphs >= 95;
  for (u32 i = 0; i < 95 && f->ascii; i++) f->ascii = f->codepoints[i] == 32 + i;
  for (u32 i = 0; i < h->nsheets; i++) {
    const u8 *s = b + h->off_sheet + 8 * i;
    u16 w = (u16)(s[0] << 8 | s[1]), ht = (u16)(s[2] << 8 | s[3]);
    u32 off = (u32)s[4] << 24 | (u32)s[5] << 16 | (u32)s[6] << 8 | s[7];
    f->sheets[i] = (k64_tex_t){ .width = w, .height = ht, .fmt = K64_FMT_I, .siz = K64_SIZ_4,
                                .line_bytes = (u16)(w / 2), .tmem_line = (u16)(w / 16), .pixels = b + off };
  }
  return true;
}

u32 k64_font_glyph(const k64_font_t *f, u32 cp) {
  if (f->ascii && cp - 32 < 95) return cp - 32;
  u32 lo = 0, hi = f->nglyphs;
  while (lo < hi) {
    u32 mid = (lo + hi) / 2;
    if (f->codepoints[mid] < cp) lo = mid + 1; else hi = mid;
  }
  return lo < f->nglyphs && f->codepoints[lo] == cp ? lo : f->fallback;
}

static int kern(const k64_font_t *f, u32 a, u32 b) {
  u32 key = a << 12 | b, lo = f->kern_first[a], hi = f->kern_first[a + 1];
  while (lo < hi) {
    u32 mid = (lo + hi) / 2;
    if (f->kern[mid] >> 8 < key) lo = mid + 1; else hi = mid;
  }
  return lo < f->kern_first[a + 1] && f->kern[lo] >> 8 == key ? (s8)f->kern[lo] : 0;
}

// Next codepoint; malformed sequences decode as U+FFFD (drawn as the fallback glyph).
static u32 utf8(const u8 **ps, const u8 *e) {
  const u8 *s = *ps;
  u32 c = *s++;
  *ps = s;
  if (c < 0x80) return c;
  u32 n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  if (!n || s + n > e) return 0xFFFD;
  c &= 0x3F >> n;
  for (; n; n--, s++) {
    if ((*s & 0xC0) != 0x80) { *ps = s; return 0xFFFD; }
    c = c << 6 | (*s & 0x3F);
  }
  *ps = s;
  return c;
}

static void set_modes(const k64_font_t *f) {
  u64 om = K64_OM_BASE | K64_OM_CYCLE_1 | K64_OM_ALPHA_COMPARE;
  if (f->mono) {
    om |= K64_OM_BLEND(K64_BL_IN, K64_BL_A_IN, K64_BL_IN, K64_BL_B_1MA, 0,0,0,0);
    k64_rdp_set_blend_color(0x00000080);
  } else {
    // Blend the glyph's coverage with the framebuffer; alpha compare still skips the
    // empty texels so they cost no framebuffer write.
    om |= K64_OM_FORCE_BLEND | K64_OM_IMAGE_READ
          | K64_OM_BLEND(K64_BL_IN, K64_BL_A_IN, K64_BL_MEM, K64_BL_B_1MA, 0, 0, 0, 0);
    k64_rdp_set_blend_color(0x00000001);
  }
  k64_rdp_set_other_modes(om);
  k64_rdp_set_combine(K64_CC_1CYC(CC_ZERO,CC_ZERO,CC_C_ZERO,CC_PRIM, CA_TEX0,CA_ZERO,CA_PRIM,CA_ZERO));
}

static void flush(const k64_font_t *f) {
  for (u32 sh = 0; sheet_mask >> sh; sh++) {
    if (!(sheet_mask >> sh & 1)) continue;
    k64_rdp_load_tex(FONT_TILE, 0, &f->sheets[sh], K64_TEX_CLAMP, K64_TEX_CLAMP);
    for (int pass = shadow_rgba & 0xFF ? 0 : 1; pass < 2; pass++) {
      int dx = pass ? 0 : shadow_dx, dy = pass ? 0 : shadow_dy;
      if (!pass) k64_rdp_set_prim_color(shadow_rgba);
      for (u32 i = 0; i < nbatch; i++) {
        const item_t *it = &batch[i];
        if (it->sheet != sh) continue;
        const k64_glyph_t *g = &f->glyphs[it->g];
        if (pass) k64_rdp_set_prim_color(palette[it->color]);
        k64_rdp_tex_rect(FONT_TILE, it->x + dx, it->y + dy, g->w, g->h, g->sx, g->sy, 1 << 10, 1 << 10);
      }
    }
  }
  nbatch = 0;
  sheet_mask = 0;
}

// Lays out s[0 .. e) from (x, y); draws when f_draw. *color carries the palette index across
// calls (box lines). Returns the pen x at the end.
static int run(const k64_font_t *f, int x, int y, const u8 *s, const u8 *e, u8 *color, bool draw) {
  int pen = 0;                   // quarter pixels from x
  int x0 = x;
  u32 prev = 0xFFFF;
  while (s < e) {
    u32 cp = utf8(&s, e);
    if (cp == '\n') { y += f->line_height; pen = 0; x = x0; prev = 0xFFFF; continue; }
    if (cp - 1 < 8) { *color = (u8)(cp - 1); continue; }
    u32 gi = k64_font_glyph(f, cp);
    const k64_glyph_t *g = &f->glyphs[gi];
    if (prev != 0xFFFF && f->nkern) pen += kern(f, prev, gi);
    prev = gi;
    if (draw && g->w) {
      if (nbatch == BATCH) flush(f);
      batch[nbatch++] = (item_t){(s16)(x + ((pen + 2) >> 2) + g->xoff), (s16)(y + g->yoff), (u16)gi, *color, g->sheet};
      sheet_mask |= 1u << g->sheet;
    }
    pen += g->advance;
  }
  return x + ((pen + 2) >> 2);
}

void k64_font_set_palette(u32 index, u32 rgba) {
  if (index - 1 < 7) palette[index] = rgba;
}

void k64_font_set_shadow(int dx, int dy, u32 rgba) {
  shadow_dx = dx; shadow_dy = dy; shadow_rgba = rgba;
}

int k64_font_width(const k64_font_t *f, const char *s) {
  const u8 *e = (const u8 *)s;
  while (*e && *e != '\n') e++;
  u8 c = 0;
  return run(f, 0, 0, (const u8 *)s, e, &c, false);
}

int k64_font_draw_n(const k64_font_t *f, int x, int y, u32 rgba, const char *s, u32 len) {
  set_modes(f);
  palette[0] = rgba;
  u8 c = 0;
  int end = run(f, x, y, (const u8 *)s, (const u8 *)s + len, &c, true);
  flush(f);
  return end;
}

int k64_font_draw(const k64_font_t *f, int x, int y, u32 rgba, const char *s) {
  return k64_font_draw_n(f, x, y, rgba, s, __builtin_strlen(s));
}

int k64_font_printf(const k64_font_t *f, int x, int y, u32 rgba, const char *fmt, ...) {
  char buf[256];
  va_list ap; va_start(ap, fmt); k64_vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  return k64_font_draw(f, x, y, rgba, buf);
}

int k64_font_box(const k64_font_t *f, int x, int y, int w, u32 align, u32 rgba, const char *str, bool draw) {
  const u8 *p = (const u8 *)str, *e = p + __builtin_strlen(str);
  if (draw) set_modes(f);
  palette[0] = rgba;
  u8 color = 0;
  int y0 = y;
  while (p < e) {
    // Longest prefix that fits: break after the last space, or inside a too-long word.
    const u8 *q = p, *brk = 0, *end, *next;
    int pen = 0, brk_pen = 0, width;
    u32 prev = 0xFFFF;
    for (;;) {
      if (q == e || *q == '\n') { end = q; next = q == e ? q : q + 1; width = pen; break; }
      const u8 *c0 = q;
      u32 cp = utf8(&q, e);
      if (cp - 1 < 8) continue;
      if (cp == ' ') { brk = c0; brk_pen = pen; }
      u32 gi = k64_font_glyph(f, cp);
      int adv = (prev != 0xFFFF && f->nkern ? kern(f, prev, gi) : 0) + f->glyphs[gi].advance;
      prev = gi;
      if (cp != ' ' && pen + adv > w * 4 && c0 != p) {
        if (brk) { end = brk; next = brk + 1; width = brk_pen; }
        else     { end = c0;  next = c0;      width = pen; }
        break;
      }
      pen += adv;
    }
    if (draw) {
      int px = (width + 2) >> 2;
      int lx = x + (align == K64_ALIGN_CENTER ? (w - px) / 2 : align == K64_ALIGN_RIGHT ? w - px : 0);
      run(f, lx, y, p, end, &color, true);
    } else {
      for (const u8 *c = p; c < end; c++) if (*c - 1u < 8) color = (u8)(*c - 1);
    }
    y += f->line_height;
    p = next;
  }
  if (draw) flush(f);
  return y - y0;
}
