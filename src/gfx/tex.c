// kestrel64-sdk — texture blobs, TMEM loads, textured rectangles, HUD text.
#include <k64/tex.h>
#include <k64/cpu.h>
#include <k64/mem.h>
#include <k64/debug.h>
#include <stdarg.h>

#define LOAD_TILE 7

// TMEM residency: skip re-uploading a texture that is already there (sprites/text batches).
// One entry per tile slot (tile & 3): the source pointer and TMEM address last loaded there.
static struct { const void *pixels; u32 tmem; } tmem_cache[4];

// Big-endian readers for the blob header.
static u32 rd32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u16 rd16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }

bool k64_tex_from_blob(k64_tex_t *t, const void *blob) {
  const u8 *b = blob;
  if (rd32(b) != 0x4B365431) return false;              // 'K6T1'
  t->width = rd16(b + 4); t->height = rd16(b + 6);
  t->fmt = b[8]; t->siz = b[9]; t->tlut_count = (u16)(b[10] * 16); t->flags = b[11];
  t->line_bytes = rd16(b + 12);
  // RGBA32 and YUV16 are split across the two TMEM halves (red/green or UV low, blue/alpha
  // or Y high, n64brew "TMEM"): the tile line counts 64-bit words of one half.
  t->tmem_line = (u16)(t->line_bytes / (k64_tex_split(t) ? 16 : 8));
  u32 pix_size = rd32(b + 16);
  t->pixels = b + 32;
  // The TLUT follows the pixel data, 8-byte aligned.
  t->tlut = t->tlut_count ? (const u16*)(b + 32 + ((pix_size + 7) & ~7u)) : 0;
  return true;
}

u32 k64_tex_tmem_bytes(const k64_tex_t *t) {
  return (u32)t->tmem_line * 8 * t->height * (k64_tex_split(t) ? 2 : 1);
}

// Smallest n with 2^n >= v: the tile mask field for a power-of-two repeat size.
static u32 log2_ceil(u32 v) { u32 n = 0; while ((1u << n) < v) n++; return n; }

// Fence before a TMEM load: a pending load is closed with SYNC_LOAD first. No-op if none is pending.
static void sync_for_load(void) {
  k64_rdp_pipe_guard();
  if (k64_rdps.load_pending) {
    k64_rdp_put(k64_rdpc_sync_load());
    k64_rdps.load_pending = false;
    k64_rdps.stat_syncs_load++;
  }
}
// Fence before a tile descriptor changes under queued primitives (SYNC_TILE), when one is pending.
static void sync_for_tile(void) {
  if (k64_rdps.tile_pending) {
    k64_rdp_put(k64_rdpc_sync_tile());
    k64_rdps.tile_pending = false;
    k64_rdps.stat_syncs_tile++;
  }
}

// TMEM load of the whole texture (TLUT first for CI) through LOAD_TILE.
static u32 cmds_load(u64 *o, u32 tmem, const k64_tex_t *t) {
  u32 n = 0;
  // CI palette first: its TLUT goes through LOAD_TILE before the texels.
  if (t->tlut_count) {
    o[n++] = k64_rdpc_tex_image(K64_FMT_RGBA, K64_SIZ_16, 1, K64_PHYS(t->tlut));
    o[n++] = k64_rdpc_set_tile(0, 0, 0, 0x100, LOAD_TILE, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    o[n++] = k64_rdpc_load_tlut(LOAD_TILE, 0, t->tlut_count - 1);
  }
  // 4-bit textures load as 8-bit rows of half the width (the RDP can't load 4b directly).
  u32 ldsiz = t->siz == K64_SIZ_4 ? K64_SIZ_8 : t->siz;
  u32 bpt = ldsiz == K64_SIZ_8 ? 1 : ldsiz == K64_SIZ_16 ? 2 : 4;
  u32 ldw = t->line_bytes / bpt;                         // texels per RDRAM row (padded)
  o[n++] = k64_rdpc_tex_image(t->fmt == K64_FMT_CI ? K64_FMT_CI : t->fmt, ldsiz, ldw, K64_PHYS(t->pixels));
  o[n++] = k64_rdpc_set_tile(t->fmt, ldsiz, t->tmem_line, tmem / 8, LOAD_TILE, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  // Whole-texture load: the lower-right corner in 10.2 fixed (texel and row counts times 4).
  o[n++] = k64_rdpc_load_tile(LOAD_TILE, 0, 0, (ldw - 1) * 4, (t->height - 1) * 4);
  return n;
}

// Render tile description: the tile's format, TMEM address, wrap and size. No TMEM traffic.
static u32 cmds_tile(u64 *o, u32 tile, u32 tmem, const k64_tex_t *t, u32 wrap_s, u32 wrap_t) {
  u32 ms = wrap_s == K64_TEX_MIRROR, mt = wrap_t == K64_TEX_MIRROR;
  u32 cs = wrap_s == K64_TEX_CLAMP,  ct = wrap_t == K64_TEX_CLAMP;
  // Masks are log2 of the repeat size; a clamped axis uses mask 0 (no repeat).
  u32 masks = cs ? 0 : log2_ceil(t->width), maskt = ct ? 0 : log2_ceil(t->height);
  o[0] = k64_rdpc_set_tile(t->fmt, t->siz, t->tmem_line, tmem / 8, tile, 0, ct, mt, maskt, 0, cs, ms, masks, 0);
  o[1] = k64_rdpc_tile_size(tile, 0, 0, (t->width - 1) * 4, (t->height - 1) * 4);
  return 2;
}

void k64_rdp_load_tex(u32 tile, u32 tmem, const k64_tex_t *t, u32 wrap_s, u32 wrap_t) {
  u64 c[K64_TEX_LOAD_CMDS];
  u32 n = 0;
  u32 slot = tile & 3;
  // Resident: this slot already holds the same source at the same TMEM address.
  bool resident = tmem_cache[slot].pixels == t->pixels && tmem_cache[slot].tmem == tmem;
  if (!resident) {
    // Any cached entry overlapping this TMEM range is now stale.
    // (RGBA32/YUV16 also fill the same range of the high half: count everything up to the end.)
    u32 bytes = k64_tex_split(t) ? 4096 - tmem : k64_tex_tmem_bytes(t);
    for (int i = 0; i < 4; i++)
      if (tmem_cache[i].pixels && tmem_cache[i].tmem < tmem + bytes && tmem < tmem_cache[i].tmem + 4096)
        tmem_cache[i].pixels = 0;
    sync_for_load();
    sync_for_tile();
    n = cmds_load(c, tmem, t);
    tmem_cache[slot].pixels = t->pixels; tmem_cache[slot].tmem = tmem;
  } else {
    // Already resident: only the tile descriptor needs to change.
    sync_for_tile();
  }
  n += cmds_tile(c + n, tile, tmem, t, wrap_s, wrap_t);
  for (u32 i = 0; i < n; i++) k64_rdp_put(c[i]);
}

u32 k64_tex_load_cmds(u64 *out, u32 tile, u32 tmem, const k64_tex_t *t, u32 wrap_s, u32 wrap_t) {
  u32 n = 0;
  out[n++] = k64_rdpc_sync_pipe();
  out[n++] = k64_rdpc_sync_load();
  out[n++] = k64_rdpc_sync_tile();
  n += cmds_load(out + n, tmem, t);
  n += cmds_tile(out + n, tile, tmem, t, wrap_s, wrap_t);
  return n;
}

void k64_tex_tmem_forget(void) {
  for (int i = 0; i < 4; i++) tmem_cache[i].pixels = 0;
}

void k64_rdp_tex_rect(u32 tile, int x, int y, int w, int h, int s, int t, int dsdx, int dtdy) {
  bool copy = k64_rdp_mode_is_copyfill();
  int s5 = s << 5, t5 = t << 5;                          // s10.5
  if (x < 0) { s5 += (-x * dsdx) >> 5; w += x; x = 0; }
  if (y < 0) { t5 += (-y * dtdy) >> 5; h += y; y = 0; }
  if (w <= 0 || h <= 0) return;
  int x1 = (x + w) * 4, y1 = (y + h) * 4;
  if (copy) { x1 -= 4; y1 -= 4; dsdx *= 4; }            // copy: inclusive end, 4 texels/clk
  k64_rdp_put2(k64_rdpc_tex_rect0(tile, x * 4, y * 4, x1, y1), k64_rdpc_tex_rect1(s5, t5, dsdx, dtdy));
  k64_rdps.prim_pending = k64_rdps.tile_pending = k64_rdps.load_pending = true;
  k64_rdps.stat_prims++;
}

// ---- text ---------------------------------------------------------------------------
extern const u8 k64_font8x8[96][8];
// Built by k64_text_init: the 128x48 I4 font sheet and its descriptor.
static k64_tex_t font_tex;
static u8 *font_pixels;

void k64_text_init(void) {
  // 16x6 glyphs of 8x8 -> 128x48 I4 = 3 KB (fits TMEM with room for nothing else: fine,
  // text is drawn as a batch).
  const u32 W = 128, H = 48, line = W / 2;
  font_pixels = k64_mem_alloc(K64_ZONE_TEX, line * H, 64);
  for (u32 c = 0; c < 96; c++) {
    u32 gx = (c % 16) * 8, gy = (c / 16) * 8;
    for (u32 r = 0; r < 8; r++) for (u32 b = 0; b < 8; b += 2) {
      u8 g = k64_font8x8[c][r];
      u8 hi = (g >> b) & 1 ? 0xF : 0, lo = (g >> (b + 1)) & 1 ? 0xF : 0;
      font_pixels[(gy + r) * line + (gx + b) / 2] = (u8)((hi << 4) | lo);
    }
  }
  k64_dcache_writeback(font_pixels, line * H);
  font_tex = (k64_tex_t){ .width = W, .height = H, .fmt = K64_FMT_I, .siz = K64_SIZ_4,
                          .line_bytes = line, .tmem_line = line / 8, .pixels = font_pixels };
}

void k64_text_draw(int x, int y, u32 rgba, const char *s) {
  k64_rdp_set_other_modes(K64_OM_BASE | K64_OM_CYCLE_1 | K64_OM_ALPHA_COMPARE |
                          K64_OM_BLEND(K64_BL_IN, K64_BL_A_IN, K64_BL_IN, K64_BL_B_1MA, 0,0,0,0));
  // Colour is the primitive colour; alpha is the font texel (I4 intensity), i.e. the glyph coverage.
  k64_rdp_set_combine(K64_CC_1CYC(CC_ZERO,CC_ZERO,CC_C_ZERO,CC_PRIM, CA_ZERO,CA_ZERO,CA_ZERO,CA_TEX0));
  k64_rdp_set_blend_color(0x00000080);                   // alpha-compare threshold
  k64_rdp_set_prim_color(rgba);
  // Tile 3 holds the font for the whole batch; load_tex skips the upload while it is resident.
  k64_rdp_load_tex(3, 0, &font_tex, K64_TEX_CLAMP, K64_TEX_CLAMP);
  int x0 = x;
  for (; *s; s++) {
    u8 c = (u8)*s;
    if (c == '\n') { y += 9; x = x0; continue; }
    // Glyph g = c - 32 in the 16-column sheet; the space (32) and controls draw nothing.
    if (c > 32 && c < 128) {
      u32 g = c - 32;
      k64_rdp_tex_rect(3, x, y, 8, 8, (g % 16) * 8, (g / 16) * 8, 1 << 10, 1 << 10);
    }
    x += 8;
  }
}

void k64_text_printf(int x, int y, u32 rgba, const char *fmt, ...) {
  char buf[128];
  va_list ap; va_start(ap, fmt); k64_vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  k64_text_draw(x, y, rgba, buf);
}
