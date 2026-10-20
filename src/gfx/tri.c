// kestrel64-sdk — RDP triangle setup on the CPU (reference implementation).
//
// RDP triangle = 3 edges (H: top->bottom major, M: top->mid, L: mid->bottom) walked
// scanline by scanline, plus per-attribute gradients along x (DxD) and along the major edge
// (DeD). Every attribute is s15.16; the shade/tex/z blocks interleave the integer and
// fractional halves (see n64brew "Triangle" commands).
#include <k64/tri.h>
#include <k64/fmath.h>

typedef struct {
  float hx, hy, mx, my;   // major / mid edge vectors (from top vertex)
  float fy;               // floor(y1) - y1: offset from top vertex to its scanline start
  float ish;              // dx/dy of the major edge
  float inv_area;         // -1 / (cross product), scales attribute plane gradients
} edge_t;

static inline u32 hi16(s32 v) { return (u32)v >> 16; }
static inline u32 lo16(s32 v) { return (u32)v & 0xFFFF; }

// Gradient of a scalar attribute over the triangle plane: d/dx, d/dy, d/de (along H).
static inline void grad(const edge_t *e, float a1, float a2, float a3, float *dx, float *dy, float *de) {
  float ma = a2 - a1, ha = a3 - a1;
  *dx = (e->hy * ma - e->my * ha) * e->inv_area;
  *dy = (e->mx * ha - e->hx * ma) * e->inv_area;
  *de = *dy + *dx * e->ish;
}

// Four attributes packed as the RDP wants them: 8 words (4 x {int,frac} x {base,dx,de,dy}).
static u64 *put_attr4(u64 *w, const s32 base[4], const s32 dx[4], const s32 de[4], const s32 dy[4]) {
  w[0] = ((u64)hi16(base[0]) << 48) | ((u64)hi16(base[1]) << 32) | (hi16(base[2]) << 16) | hi16(base[3]);
  w[1] = ((u64)hi16(dx[0]) << 48)   | ((u64)hi16(dx[1]) << 32)   | (hi16(dx[2]) << 16)   | hi16(dx[3]);
  w[2] = ((u64)lo16(base[0]) << 48) | ((u64)lo16(base[1]) << 32) | (lo16(base[2]) << 16) | lo16(base[3]);
  w[3] = ((u64)lo16(dx[0]) << 48)   | ((u64)lo16(dx[1]) << 32)   | (lo16(dx[2]) << 16)   | lo16(dx[3]);
  w[4] = ((u64)hi16(de[0]) << 48)   | ((u64)hi16(de[1]) << 32)   | (hi16(de[2]) << 16)   | hi16(de[3]);
  w[5] = ((u64)hi16(dy[0]) << 48)   | ((u64)hi16(dy[1]) << 32)   | (hi16(dy[2]) << 16)   | hi16(dy[3]);
  w[6] = ((u64)lo16(de[0]) << 48)   | ((u64)lo16(de[1]) << 32)   | (lo16(de[2]) << 16)   | lo16(de[3]);
  w[7] = ((u64)lo16(dy[0]) << 48)   | ((u64)lo16(dy[1]) << 32)   | (lo16(dy[2]) << 16)   | lo16(dy[3]);
  return w + 8;
}

static inline s32 clamp_y2(float y) {      // s11.2, floor
  s32 v = k64_floori(y * 4.f);
  return v < -16384 ? -16384 : v > 16383 ? 16383 : v;
}

void k64_rdp_tri(u32 flags, u32 tile, const k64_svtx_t *v1, const k64_svtx_t *v2, const k64_svtx_t *v3) {
  const k64_svtx_t *t;
  if (v1->y > v2->y) { t = v1; v1 = v2; v2 = t; }
  if (v2->y > v3->y) { t = v2; v2 = v3; v3 = t; }
  if (v1->y > v2->y) { t = v1; v1 = v2; v2 = t; }

  s32 y1f = clamp_y2(v1->y), y2f = clamp_y2(v2->y), y3f = clamp_y2(v3->y);
  float y1 = y1f * 0.25f, y2 = y2f * 0.25f, y3 = y3f * 0.25f;

  edge_t e;
  e.hx = v3->x - v1->x; e.hy = y3 - y1;
  e.mx = v2->x - v1->x; e.my = y2 - y1;
  float lx = v3->x - v2->x, ly = y3 - y2;
  float nz = e.hx * e.my - e.hy * e.mx;
  if (nz == 0.f) return;                                   // degenerate: nothing to draw
  e.inv_area = -1.f / nz;
  u32 lft = nz < 0;
  e.ish = e.hy != 0.f ? e.hx / e.hy : 0.f;
  float ism = e.my != 0.f ? e.mx / e.my : 0.f;
  float isl = ly != 0.f ? lx / ly : 0.f;
  e.fy = k64_floorf(y1) - y1;
  float xh = v1->x + e.fy * e.ish, xm = v1->x + e.fy * ism, xl = v2->x;

  u32 n = 4 + ((flags & K64_TRI_SHADE) ? 8 : 0) + ((flags & K64_TRI_TEX) ? 8 : 0) + ((flags & K64_TRI_Z) ? 2 : 0);
  u64 *w = k64_rdp_reserve(n);
  w[0] = K64_RDP_OP(0x08 | (flags & 7)) | ((u64)lft << 55) | ((u64)(tile & 7) << 48) |
         ((u64)(y3f & 0x3FFF) << 32) | ((u32)(y2f & 0x3FFF) << 16) | (u32)(y1f & 0x3FFF);
  w[1] = ((u64)(u32)k64_f2fx16(xl) << 32) | (u32)k64_f2fx16(isl);
  w[2] = ((u64)(u32)k64_f2fx16(xh) << 32) | (u32)k64_f2fx16(e.ish);
  w[3] = ((u64)(u32)k64_f2fx16(xm) << 32) | (u32)k64_f2fx16(ism);
  w += 4;

  if (flags & K64_TRI_SHADE) {
    s32 base[4], dx[4], de[4], dy[4];
    const float c1[4] = { v1->r, v1->g, v1->b, v1->a };
    const float c2[4] = { v2->r, v2->g, v2->b, v2->a };
    const float c3[4] = { v3->r, v3->g, v3->b, v3->a };
    for (int i = 0; i < 4; i++) {
      float fdx, fdy, fde;
      grad(&e, c1[i], c2[i], c3[i], &fdx, &fdy, &fde);
      base[i] = k64_f2fx16(c1[i] + e.fy * fde);
      dx[i] = k64_f2fx16(fdx); de[i] = k64_f2fx16(fde); dy[i] = k64_f2fx16(fdy);
    }
    w = put_attr4(w, base, dx, de, dy);
  }

  if (flags & K64_TRI_TEX) {
    // Perspective: the RDP interpolates s/w, t/w, 1/w and divides per pixel. Normalise 1/w so
    // the largest is ~1.0 (max precision in the s15 W channel), texel coords in s10.5.
    float maxw = v1->inv_w > v2->inv_w ? v1->inv_w : v2->inv_w;
    if (v3->inv_w > maxw) maxw = v3->inv_w;
    float k = 1.f / maxw;
    float w1 = v1->inv_w * k, w2 = v2->inv_w * k, w3 = v3->inv_w * k;
    float s1 = v1->s * 32.f * w1, s2 = v2->s * 32.f * w2, s3 = v3->s * 32.f * w3;
    float t1 = v1->t * 32.f * w1, t2 = v2->t * 32.f * w2, t3 = v3->t * 32.f * w3;
    w1 *= 32767.f; w2 *= 32767.f; w3 *= 32767.f;
    const float a1[3] = { s1, t1, w1 }, a2[3] = { s2, t2, w2 }, a3[3] = { s3, t3, w3 };
    s32 base[4] = { 0 }, dx[4] = { 0 }, de[4] = { 0 }, dy[4] = { 0 };
    for (int i = 0; i < 3; i++) {
      float fdx, fdy, fde;
      grad(&e, a1[i], a2[i], a3[i], &fdx, &fdy, &fde);
      base[i] = k64_f2fx16(a1[i] + e.fy * fde);
      dx[i] = k64_f2fx16(fdx); de[i] = k64_f2fx16(fde); dy[i] = k64_f2fx16(fdy);
    }
    w = put_attr4(w, base, dx, de, dy);
  }

  if (flags & K64_TRI_Z) {
    float fdx, fdy, fde;
    float z1 = v1->z * 32767.f, z2 = v2->z * 32767.f, z3 = v3->z * 32767.f;
    grad(&e, z1, z2, z3, &fdx, &fdy, &fde);
    w[0] = ((u64)(u32)k64_f2fx16(z1 + e.fy * fde) << 32) | (u32)k64_f2fx16(fdx);
    w[1] = ((u64)(u32)k64_f2fx16(fde) << 32) | (u32)k64_f2fx16(fdy);
  }

  k64_rdps.prim_pending = true;
  if (flags & K64_TRI_TEX) k64_rdps.tile_pending = k64_rdps.load_pending = true;
  k64_rdps.stat_prims++;
}
