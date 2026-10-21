// kestrel64-sdk — K64V CPU reference back end: work list -> planes, YUV -> RGBA5551.
// Both are the bit-exact references of the RSP microcode (src/rsp/kvid.S).
//
// The IDCT is computed exactly as the RSP does it (k64/video.h): pass 1 down the columns,
// pass 2 along the rows, each output sat16((2 * sum M[i][k] x[k] + 0x8000) >> 16) like
// vmulf + 7 x vmacf, then round(x / 8) like vmulf by 0x1000. All-zero coefficient columns
// are skipped: their pass-1 outputs and pass-2 terms are exactly 0.
#include <k64/video.h>
#include <k64/cpu.h>

static const s16 M[8][8] = {
  {  11585,  16069,  15137,  13623,  11585,   9102,   6270,   3196 },
  {  11585,  13623,   6270,  -3196, -11585, -16069, -15137,  -9102 },
  {  11585,   9102,  -6270, -16069, -11585,   3196,  15137,  13623 },
  {  11585,   3196, -15137,  -9102,  11585,  13623,  -6270, -16069 },
  {  11585,  -3196, -15137,   9102,  11585, -13623,  -6270,  16069 },
  {  11585,  -9102,  -6270,  16069, -11585,  -3196,  15137, -13623 },
  {  11585, -13623,   6270,   3196, -11585,  16069, -15137,   9102 },
  {  11585, -16069,  15137, -13623,  11585,  -9102,   6270,  -3196 },
};

// vmulf/vmacf rounding: (2 * s + 0x8000) >> 16, saturated to s16
static inline s32 mac_round(s64 s) {
  s = (2 * s + 0x8000) >> 16;
  return s > 32767 ? 32767 : s < -32768 ? -32768 : (s32)s;
}

// x: coefficients [v][u] -> d: pixel deltas [row][col].
// 8x8 IDCT in two passes, as the RSP computes it (see the file header)
static void idct(const s16 x[64], s16 d[64]) {
  // bit u set: column u has a coefficient
  u32 cols = 0;
  for (int k = 0; k < 64; k++) if (x[k]) cols |= 1u << (k & 7);
  s32 t[8][8];                                    // t[i][u]
  for (int u = 0; u < 8; u++) {
    if (!(cols & (1u << u))) continue;
    for (int i = 0; i < 8; i++) {
      s64 s = 0;
      for (int vv = 0; vv < 8; vv++) s += (s32)M[i][vv] * x[vv * 8 + u];
      t[i][u] = mac_round(s);
    }
  }
  for (int i = 0; i < 8; i++)
    for (int j = 0; j < 8; j++) {
      s64 s = 0;
      for (int u = 0; u < 8; u++) if (cols & (1u << u)) s += (s32)M[j][u] * t[i][u];
      // final round(x / 8): the RSP's vmulf by 0x1000
      d[i * 8 + j] = (s16)((mac_round(s) * 0x2000 + 0x8000) >> 16);
    }
}

// clamp to 0..255
static inline u8 clamp8(s32 x) { return x < 0 ? 0 : x > 255 ? 255 : (u8)x; }

void k64_video_reconstruct_cpu(k64_video_t *v, const u16 *w, u32 row) {
  u32 yw = v->width, cw = v->width / 2;
  for (u32 mx = 0; mx < v->width / 16u; mx++) {
    // head word: mode in bits 0..1, coded block pattern from bit 2
    u32 head = *w++, mode = head & 3, cbp = head >> 2;
    // Prediction source in the previous frame: the same place, or displaced.
    const u8 *py = v->ry + row * 16 * yw + mx * 16;
    u32 co = row * 8 * cw + mx * 8;
    if (mode == K64V_MOTION) {
      py = v->ry + ((u32)w[0] << 16 | w[1]);
      co = (u32)w[2] << 16 | w[3];
      w += 4;
    }
    // blocks: 0..3 the luma quadrants, 4 = U, 5 = V
    for (int k = 0; k < 6; k++) {
      u8 *dst; const u8 *src; u32 stride;
      if (k < 4) {
        stride = yw;
        u32 o = (k >> 1) * 8 * yw + (k & 1) * 8;
        dst = v->y + (row * 16) * yw + mx * 16 + o;
        src = py + o;
      } else {
        stride = cw;
        dst = (k == 4 ? v->u : v->v) + row * 8 * cw + mx * 8;
        src = (k == 4 ? v->ru : v->rv) + co;
      }
      if (mode == K64V_INTRA) {
        if (!(cbp & (0x20u >> k))) {
          // intra block without coefficients: flat grey
          for (int r = 0; r < 8; r++) __builtin_memset(dst + r * stride, 128, 8);
          continue;
        }
      } else {
        // inter: the prediction is copied first, the residual is added below
        for (int r = 0; r < 8; r++) __builtin_memcpy(dst + r * stride, src + r * stride, 8);
        if (mode == K64V_SKIP || !(cbp & (0x20u >> k))) continue;
      }
      // coefficients: a count, then {natural index, value} pairs
      s16 x[64] = { 0 }, d[64];
      for (u32 n = *w++; n; n--, w += 2) x[w[0]] = (s16)w[1];
      idct(x, d);
      for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
          u8 *px = dst + r * stride + c;
          *px = clamp8(d[r * 8 + c] + (mode == K64V_INTRA ? 128 : *px));
        }
    }
  }
}

// Same arithmetic as the RSP (kvid.S): << 7 domain, vmulf rounding for the chroma terms.
static inline s32 mulf(s32 a, s32 k) { return (2 * a * k + 0x8000) >> 16; }
// clamp a value in << 7 units to 0..255 and keep 5 bits
static inline u32 chan5(s32 x7) { return (u32)(x7 < 0 ? 0 : x7 > 32640 ? 32640 : x7) >> 10; }

void k64_video_blit(const k64_video_t *v, const k64_surface_t *s, int x0, int y0) {
  u32 w = v->width, cw = w / 2;
  for (u32 r = 0; r < v->height; r++) {
    int sy = y0 + (int)r;
    if (sy < 0 || sy >= s->height) continue;
    u16 *line = (u16 *)((u8 *)s->pixels + sy * s->stride);
    const u8 *yp = v->y + r * w, *up = v->u + (r / 2) * cw, *vp = v->v + (r / 2) * cw;
    for (u32 c = 0; c < w; c++) {
      int sx = x0 + (int)c;
      if (sx < 0 || sx >= s->width) continue;
      // YCbCr to RGB (BT.601 with the RSP's constants): R = Y + 1.402 Cr, G = Y - 0.344 Cb
      // - 0.714 Cr, B = Y + 1.772 Cb
      s32 y7 = yp[c] << 7, e7 = (up[c / 2] - 128) << 7, d7 = (vp[c / 2] - 128) << 7;
      s32 rc = mulf(d7, 13173) + d7;
      s32 gc = (2 * e7 * -11277 + 2 * d7 * -23401 + 0x8000) >> 16;
      s32 bc = mulf(e7, 25297) + e7;
      // RGBA5551: 5 bits per colour, alpha bit set
      line[sx] = (u16)(chan5(y7 + rc) << 11 | chan5(y7 + gc) << 6 | chan5(y7 + bc) << 1 | 1);
    }
    // the line goes to RDRAM for the VI
    k64_dcache_writeback(line, s->stride);
  }
}
