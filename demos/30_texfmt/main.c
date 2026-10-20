// Demo 30 — every texture format the baker emits, drawn by the RDP and checked.
// One 32x32 RGBA gradient (varying alpha) baked as RGBA16, RGBA32, CI4, CI8, I4, I8, IA4,
// IA8, IA16, YUV16 and CI4/CI8 with IA16 palettes; each is drawn with a 1-cycle
// point-sampled tex rect, combiner = TEXEL0, no dither, into a 16bpp framebuffer. Self-test:
// every pixel's RGB equals the texel decoded on the CPU from the baked blob, expanded to 8
// bits the way the RDP does it (n64brew "Texture formats": 5 -> 8 bits = c << 3 | c >> 2,
// I4 = i * 0x11, IA4 intensity = i << 5 | i << 2 | i >> 1; YUV through the filter's
// conversion, k64/tex.h) and truncated to 5 bits by the colour image write.
#include <k64/k64.h>

extern const u8 t_rgba16[], t_rgba32[], t_ci4[], t_ci8[], t_i4[], t_i8[], t_ia4[], t_ia8[], t_ia16[],
                t_yuv16[], t_ci4ia[], t_ci8ia[];

static const struct { const char *name; const u8 *blob; } fmts[] = {
  { "rgba16", t_rgba16 }, { "rgba32", t_rgba32 }, { "ci4", t_ci4 }, { "ci8", t_ci8 },
  { "i4", t_i4 }, { "i8", t_i8 }, { "ia4", t_ia4 }, { "ia8", t_ia8 }, { "ia16", t_ia16 },
  { "yuv16", t_yuv16 }, { "ci4ia", t_ci4ia }, { "ci8ia", t_ci8ia },
};
static const int yuv_k[6] = { K64_YUV_CONVERT };
#define NFMT (sizeof fmts / sizeof fmts[0])

static u32 fails;
#define CHECK(c, ...)                              \
  do {                                             \
    if (!(c)) {                                    \
      fails++;                                     \
      k64_printf("[30_texfmt] FAIL " __VA_ARGS__); \
      k64_printf("\n");                            \
    }                                              \
  } while (0)

static u32 x5(u32 c) { return c << 3 | c >> 2; }
static s32 sext9(s32 v) { return (s32)((u32)v << 23) >> 23; }
// Combiner pass-through of a 9-bit filter output (parallel-rdp combiner.h/clamping.h).
static u32 cc_clamp(s32 v) {
  v = sext9(sext9(v - 0x80) + 0x80 - 0x80) + 0x80;
  return v < 0 ? 0 : v > 255 ? 255 : (u32)v;
}

// Texel (x, y) of a baked texture as 8-bit R, G, B.
static void texel_rgb(const k64_tex_t *t, u32 x, u32 y, u32 rgb[3]) {
  const u8 *row = (const u8 *)t->pixels + y * t->line_bytes;
  u32 v;
  if (t->fmt == K64_FMT_YUV) {
    s32 Y = row[2 * x + 1], U = row[4 * (x / 2)] - 128, V = row[4 * (x / 2) + 2] - 128, f[4];
    for (int i = 0; i < 4; i++) f[i] = 2 * sext9(yuv_k[i]) + 1;
    rgb[0] = cc_clamp(Y + ((f[0] * V + 0x80) >> 8));
    rgb[1] = cc_clamp(Y + ((f[1] * U + f[2] * V + 0x80) >> 8));
    rgb[2] = cc_clamp(Y + ((f[3] * U + 0x80) >> 8));
    return;
  }
  switch (t->siz) {
    case K64_SIZ_4:  v = (row[x / 2] >> (x & 1 ? 0 : 4)) & 15; break;
    case K64_SIZ_8:  v = row[x]; break;
    case K64_SIZ_16: v = (u32)row[2 * x] << 8 | row[2 * x + 1]; break;
    default:
      v = (u32)row[4 * x] << 24 | (u32)row[4 * x + 1] << 16 | (u32)row[4 * x + 2] << 8 | row[4 * x + 3];
      break;
  }
  u32 i;
  if (t->fmt == K64_FMT_CI) v = t->tlut[v];
  if (t->fmt == K64_FMT_CI && (t->flags & K64_TEX_TLUT_IA)) { rgb[0] = rgb[1] = rgb[2] = v >> 8; return; }
  if (t->fmt == K64_FMT_CI || (t->fmt == K64_FMT_RGBA && t->siz == K64_SIZ_16)) {
    rgb[0] = x5(v >> 11); rgb[1] = x5(v >> 6 & 31); rgb[2] = x5(v >> 1 & 31);
    return;
  }
  if (t->fmt == K64_FMT_RGBA) { rgb[0] = v >> 24; rgb[1] = v >> 16 & 255; rgb[2] = v >> 8 & 255; return; }
  if (t->fmt == K64_FMT_I) i = t->siz == K64_SIZ_4 ? v * 0x11 : v;
  else if (t->siz == K64_SIZ_4) { u32 j = v >> 1; i = j << 5 | j << 2 | j >> 1; }
  else if (t->siz == K64_SIZ_8) i = (v >> 4) * 0x11;
  else i = v >> 8;
  rgb[0] = rgb[1] = rgb[2] = i;
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  k64_rdp_set_convert(K64_YUV_CONVERT);
  k64_tex_t tex[NFMT];
  for (u32 k = 0; k < NFMT; k++) K64_ASSERT(k64_tex_from_blob(&tex[k], fmts[k].blob));

  for (u32 frame = 0;; frame++) {
    const k64_surface_t *fb = k64_display_acquire();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, 0);
    k64_rdp_set_other_modes(K64_OM_FILL);
    k64_rdp_set_fill_color(0x00010001);
    k64_rdp_fill_rect(0, 0, fb->width, fb->height);
    for (u32 k = 0; k < NFMT; k++) {
      const k64_tex_t *t = &tex[k];
      // YUV: clear BILERP0/1 so the filter converts to RGB instead.
      u64 base = t->fmt == K64_FMT_YUV ? K64_OM_BASE & ~(K64_OM_BILERP0 | K64_OM_BILERP1) : K64_OM_BASE;
      k64_rdp_set_other_modes(base | K64_OM_CYCLE_1 | k64_tex_tlut_mode(t) |
                              K64_OM_BLEND(K64_BL_IN, K64_BL_A_IN, K64_BL_IN, K64_BL_B_1MA, 0,0,0,0));
      k64_rdp_set_combine(K64_CC_TEX);
      k64_rdp_load_tex(0, 0, t, K64_TEX_CLAMP, K64_TEX_CLAMP);
      k64_rdp_sprite(0, 16 + (k % 5) * 56, 40 + (k / 5) * 56, t->width, t->height);
    }
    for (u32 k = 0; k < NFMT; k++)
      k64_text_printf(16 + (k % 5) * 56, 76 + (k / 5) * 56, 0xFFFFFFFF, "%s", fmts[k].name);
    k64_text_printf(16, 16, 0xFFFFFFFF, "kestrel64-sdk  30 texture formats  %s",
                    frame ? (fails ? "FAIL" : "PASS") : "");
    k64_rdp_wait_idle();

    if (frame == 0) {
      k64_dcache_invalidate(fb->pixels, fb->stride * fb->height);
      for (u32 k = 0; k < NFMT; k++) {
        const k64_tex_t *t = &tex[k];
        u32 bad = 0, fx = 0, fy = 0, got = 0, want = 0;
        for (u32 y = 0; y < t->height; y++)
          for (u32 x = 0; x < t->width; x++) {
            u32 rgb[3];
            texel_rgb(t, x, y, rgb);
            u32 w = (rgb[0] >> 3) << 11 | (rgb[1] >> 3) << 6 | (rgb[2] >> 3) << 1;
            u32 p =
                ((const u16 *)((const u8 *)fb->pixels + (40 + (k / 5) * 56 + y) * fb->stride))[16 + (k % 5) * 56 + x]
                & 0xFFFE;
            if (p != w && !bad++) { fx = x; fy = y; got = p; want = w; }
          }
        CHECK(!bad, "%s: %u pixels wrong, first (%u,%u) %04x want %04x", fmts[k].name, bad, fx, fy, got, want);
      }
      k64_printf("[30_texfmt] self-test %s (%u failures)\n", fails ? "FAIL" : "PASS", fails);
    }
    k64_display_show(fb);
  }
}
