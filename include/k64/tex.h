// kestrel64-sdk — textures (baked .k64t blobs) + TMEM loading + 2D primitives.
//
// TMEM is 4 KB: 4 KB of texels for RGBA16/IA/I, 2 KB texels + 2 KB TLUT for CI; RGBA32
// is split in the two 2 KB halves (red/green at tmem, blue/alpha at tmem + 2 KB), and so
// is YUV16 (U/V pairs at tmem, Y at tmem + 2 KB).
// The baker (tools/k64bake.py) emits TMEM-ready data: lines padded to 8 bytes, formats the
// RDP samples natively, palettes pre-quadrupled where the hardware wants it.
#pragma once
#include "hw.h"
#include "rdp.h"

typedef struct {
  u16 width, height;
  u8  fmt, siz;          // K64_FMT_*, K64_SIZ_*
  u16 tlut_count;       // palette entries (0 = none, 16 = CI4, 256 = CI8)
  u8  flags;             // K64_TEX_TLUT_IA
  u16 line_bytes;        // bytes per row in RDRAM (8-byte multiple)
  u16 tmem_line;         // TMEM line stride in 64-bit words (RGBA32: of one half)
  const void *pixels;    // KSEG0, 8-byte aligned
  const u16 *tlut;       // KSEG0, 8-byte aligned
} k64_tex_t;

#define K64_TEX_TLUT_IA 1   // palette entries are IA16 (intensity << 8 | alpha), not RGBA5551

// Other-mode TLUT bits a texture needs (OR them into the other modes it is drawn with).
K64_INLINE u64 k64_tex_tlut_mode(const k64_tex_t *t) {
  return !t->tlut_count ? 0 : (t->flags & K64_TEX_TLUT_IA) ? K64_OM_TLUT | K64_OM_TLUT_IA : K64_OM_TLUT;
}
// RGBA32 and YUV16 occupy both TMEM halves.
K64_INLINE bool k64_tex_split(const k64_tex_t *t) { return t->siz == K64_SIZ_32 || t->fmt == K64_FMT_YUV; }

// YUV16 textures (baker format yuv16: full-range BT.601, U Y0 V Y1) come out of the texture
// filter as RGB when the cycle's BILERP bit is clear: r = Y + (k0' V + 128) >> 8,
// g = Y + (k1' U + k2' V + 128) >> 8, b = Y + (k3' U + 128) >> 8, with U, V centred on 0 and
// kN' = 2 kN + 1 (parallel-rdp, angrylion). Draw them with the other modes' K64_OM_BILERP0
// (1-cycle) cleared, point sampled, after k64_rdp_set_convert(K64_YUV_CONVERT): 359/256 =
// 1.402, -87/256 = -0.340, -183/256 = -0.715, 453/256 = 1.770. K4/K5 (combiner inputs) are 0.
#define K64_YUV_CONVERT 179, -44, -92, 226, 0, 0

// Parse a resident .k64t blob ('K6T1' header). Returns false on bad magic.
bool k64_tex_from_blob(k64_tex_t *t, const void *blob);
u32  k64_tex_tmem_bytes(const k64_tex_t *t);

enum { K64_TEX_CLAMP = 0, K64_TEX_WRAP = 1, K64_TEX_MIRROR = 2 };

// Load a whole texture into TMEM at `tmem` (bytes) and describe it on render tile `tile`.
// Emits SYNC_LOAD/SYNC_TILE only when the previous primitive could still be using TMEM.
void k64_rdp_load_tex(u32 tile, u32 tmem, const k64_tex_t *t, u32 wrap_s, u32 wrap_t);
// The same load as raw RDP commands for a kgfx list (k64_kgfx_rdp), led by SYNC_PIPE,
// SYNC_LOAD and SYNC_TILE: writes at most K64_TEX_LOAD_CMDS words to `out`, returns the
// count. A list that loads TMEM this way leaves k64_rdp_load_tex's residency tracking stale:
// call k64_tex_tmem_forget() after its k64_kgfx_wait().
#define K64_TEX_LOAD_CMDS 11
u32  k64_tex_load_cmds(u64 *out, u32 tile, u32 tmem, const k64_tex_t *t, u32 wrap_s, u32 wrap_t);
void k64_tex_tmem_forget(void);

// Rectangles drawn with the texture on `tile`. Pixel coordinates; works in copy mode
// (4 px/clk, no filtering/blending) and 1/2-cycle modes (scaled, filtered, combined).
void k64_rdp_tex_rect(u32 tile, int x, int y, int w, int h, int s, int t, int scale_q10x, int scale_q10y);
K64_INLINE void k64_rdp_sprite(u32 tile, int x, int y, int w, int h) {
  k64_rdp_tex_rect(tile, x, y, w, h, 0, 0, 1 << 10, 1 << 10);
}

// ---- debug/HUD text (font8x8 as an I4 texture, drawn with tex rects) -----------------
void k64_text_init(void);
// Draws in 1-cycle mode: color = prim, alpha = font texel. Changes other-modes/combine.
void k64_text_draw(int x, int y, u32 rgba, const char *s);
void k64_text_printf(int x, int y, u32 rgba, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
