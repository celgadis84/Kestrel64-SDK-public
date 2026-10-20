// Demo 04 — kgfx microcode self-test.
//
// Stage A (vertex transform):
// Loads a matrix, a viewport and 32 vertices through a kgfx command list, reads the RSP
// vertex cache back and checks every field against a CPU reference:
//   clip     exact (s64 integer math on the same fixed-point inputs)
//   1/w      error in s15.16 lsb (RSP reciprocal table + one Newton-Raphson step)
//   screen   float reference, error reported in s13.2 units
// Also exercises odd vertex counts (slot 31 must survive a 31-vertex load at slot 0).
//
// Stage B (triangle setup): random triangles over the same vertex cache, emitted by the RSP
// straight into the RDP ring, decoded and compared field by field against k64_rdp_tri fed
// with the very same cached screen vertices. Errors in s15.16 lsb.
//
// Stage C (clipping): a camera inside the vertex cloud, so triangles cross the near plane,
// the guard band and (every other seed) the far plane. The RSP clips them; the reference is a
// float Sutherland-Hodgman clipper here with the same plane order and fan, fed into
// k64_rdp_tri. Clip flags are checked exactly in stage A for these scenes too.
//
// Stage E (lighting): random normals, colors, 2 or 4 lights + ambient and a model rotation;
// the lit vertex colors must match a CPU model of the microcode's fixed-point math exactly,
// and a float evaluation (with the 5.6.5-decoded normal) within 2 levels. Texgen: random
// normals and modelview rotations, s/t against the fixed-point model exactly and a float
// evaluation within 2 s10.5 units.
#include <k64/k64.h>

#define NV 32
static k64_vtx_t verts[NV] K64_ALIGN(16);
static k64_mtxfx_t mtx;
static k64_viewport_t vp;
static u64 list_buf[64] K64_ALIGN(16);
static k64_kgfx_vcache_t vc[NV] K64_ALIGN(16);

static u32 rng = 0x12345678;
static s16 rnd(s16 lo, s16 hi) {
  rng = rng * 1664525u + 1013904223u;
  return (s16)(lo + (s32)((rng >> 8) % (u32)(hi - lo + 1)));
}

typedef struct { s32 max_clip, max_sx, max_sy, max_sz; u32 max_invw; u32 attr_bad, flag_bad, checked, cut; } stats_t;

// Clip flags as the RSP computes them: exact 32-bit compares against +-w and +-4w (4w
// saturates to s32, like the vector unit's clamp).
static u32 clip_flags(const s32 c[4]) {
  s64 w = c[3], w4 = 4 * w;
  if (w4 > 0x7FFFFFFF) w4 = 0x7FFFFFFF;
  if (w4 < -0x7FFFFFFF - 1) w4 = -0x7FFFFFFF - 1;
  u32 f = 0;
  for (int k = 0; k < 3; k++) {
    if (c[k] <= -w) f |= 0x01u << k;
    if (c[k] >= w)  f |= 0x10u << k;
  }
  for (int k = 0; k < 2; k++) {
    if (c[k] <= -w4) f |= 0x100u << k;
    if (c[k] >= w4)  f |= 0x1000u << k;
  }
  return f;
}

static void check_vertex(stats_t *st, int slot, const k64_vtx_t *in) {
  const k64_kgfx_vcache_t *o = &vc[slot];
  s32 clip[4];
  for (int r = 0; r < 4; r++) {
    s64 acc = (s64)((s32)((u32)(u16)mtx.i[3][r] << 16) | mtx.f[3][r]);
    const s16 xyz[3] = { in->x, in->y, in->z };
    for (int c = 0; c < 3; c++)
      acc += (s64)((s32)((u32)(u16)mtx.i[c][r] << 16) | mtx.f[c][r]) * xyz[c];
    clip[r] = (s32)acc;
    s32 got = (s32)(((u32)(u16)o->clip_i[r] << 16) | o->clip_f[r]);
    s32 d = got - clip[r]; if (d < 0) d = -d;
    if (d > st->max_clip) st->max_clip = d;
  }
  bool behind = clip[3] <= 0;
  u32 want = clip_flags(clip);
  if (o->flags != want) {
    if (st->flag_bad++ < 4)
      k64_printf("[04] flags slot %d got %x want %x clip %x %x %x %x\n", slot, o->flags, want, clip[0], clip[1],
                 clip[2], clip[3]);
  }
  if (want & K64_KGFX_CLIP_CUT) st->cut++;
  if (o->s != in->s || o->t != in->t || o->r != in->r || o->g != in->g || o->b != in->b || o->a != in->a)
    st->attr_bad++;
  st->checked++;
  if (behind || clip[3] < 0x8000 || (want & K64_KGFX_CLIP_CUT)) return;   // screen values meaningless / overflow
  float w = clip[3] / 65536.f, iw = 1.f / w;
  s32 got_iw = (s32)(((u32)(u16)o->invw_i << 16) | o->invw_f);
  s32 di = got_iw - k64_floori(iw * 65536.f); if (di < 0) di = -di;
  if ((u32)di > st->max_invw) st->max_invw = (u32)di;
  float nx = clip[0] / 65536.f * iw, ny = clip[1] / 65536.f * iw, nz = clip[2] / 65536.f * iw;
  s32 sx = k64_floori(vp.trans[0] + nx * vp.scale[0]);
  s32 sy = k64_floori(vp.trans[1] + ny * vp.scale[1]);
  s32 sz = k64_floori(vp.trans[2] + nz * vp.scale[2]);
  s32 d;
  d = o->sx - sx; if (d < 0) d = -d; if (d > st->max_sx) st->max_sx = d;
  d = o->sy - sy; if (d < 0) d = -d; if (d > st->max_sy) st->max_sy = d;
  if (nz >= -1.f && nz <= 1.f) { d = o->sz - sz; if (d < 0) d = -d; if (d > st->max_sz) st->max_sz = d; }
}

// Scene 0: camera 4..7 units from a +-1 cloud. Scene 1: camera inside it (clipping).
static int scene;

static bool run_test(u32 seed, stats_t *st, u32 *rsp_us) {
  rng = seed;
  // Model space: +-4096 units = +-1.0 world unit after the 1/4096 scale in the matrix.
  for (int i = 0; i < NV; i++)
    verts[i] = (k64_vtx_t){ rnd(-4096, 4096), rnd(-4096, 4096), rnd(-4096, 4096), 0,
                            (u8)rng, (u8)(rng >> 8), (u8)(rng >> 16), (u8)(rng >> 24),
                            rnd(-1024, 1024), rnd(-1024, 1024) };
  k64_mat4_t proj, view, rot, sc, tmp, vpm, mvp;
  if (scene == 0) {
    k64_mat4_perspective(&proj, 60.f * K64_PI / 180.f, 320.f / 240.f, 1.f, 100.f);
    k64_mat4_lookat(&view, (k64_vec3_t){ 0, 1.5f, 4.f + (seed & 3) }, (k64_vec3_t){ 0, 0, 0 }, (k64_vec3_t){ 0, 1, 0 });
  } else {
    k64_mat4_perspective(&proj, 60.f * K64_PI / 180.f, 320.f / 240.f, 0.25f, (seed & 1) ? 1.5f : 50.f);
    k64_mat4_lookat(&view, (k64_vec3_t){ 0, 0.3f, 0.5f }, (k64_vec3_t){ 0, 0, -1 }, (k64_vec3_t){ 0, 1, 0 });
  }
  k64_mat4_rotate_y(&rot, (seed & 255) * 0.1f);
  k64_mat4_scale(&sc, 1.f / 4096.f, 1.f / 4096.f, 1.f / 4096.f);
  k64_mat4_mul(&vpm, &proj, &view);
  k64_mat4_mul(&tmp, &rot, &sc);
  k64_mat4_mul(&mvp, &vpm, &tmp);
  k64_mtx_to_fixed(&mtx, &mvp);
  k64_viewport_set(&vp, 320, 240);
  k64_dcache_writeback(verts, sizeof verts);
  k64_dcache_writeback(&mtx, sizeof mtx);
  k64_dcache_writeback(&vp, sizeof vp);

  k64_kgfx_list_t l;
  k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
  k64_kgfx_viewport(&l, &vp);
  k64_kgfx_mtx(&l, &mtx);
  k64_kgfx_vtx(&l, &verts[31], 1, 31);     // slot 31 first...
  k64_kgfx_vtx(&l, &verts[0], 31, 0);      // ...odd count must not clobber it
  k64_kgfx_end(&l);
  u32 t0 = k64_count();
  k64_kgfx_run(&l);
  u32 status = k64_kgfx_wait();
  *rsp_us = K64_US(k64_count() - t0);
  if (status) { k64_printf("[04] kgfx status %x\n", status); return false; }
  k64_kgfx_read_vcache(vc, 0, NV);
  for (int i = 0; i < NV; i++) check_vertex(st, i, &verts[i]);
  return true;
}

// ---- stage B ----------------------------------------------------------------------------
#define NT 64
#define TRI_WORDS 22                         // shade + tex + z
static u64 ref_buf[NT * TRI_WORDS];

typedef struct {
  u32 tris, slivers, hdr_bad, count_bad, rejected, cut, span_w, span_h;
  s32 xend, z_span, edge, slope, shade, shade_d, tex, tex_d, texw, z, z_d;
  u32 rsp_us, cpu_us;
} tstats_t;

static s32 fx(u64 w, int lane) { return (s16)(u16)(w >> (48 - 16 * lane)); }
static s32 attr(const u64 *w, int word_i, int word_f, int lane) {
  return (s32)(((u32)(u16)fx(w[word_i], lane) << 16) | (u16)fx(w[word_f], lane));
}
static void upd(s32 *m, s32 a, s32 b) { s32 d = a - b; if (d < 0) d = -d; if (d > *m) *m = d; }

static void cmp_attr4(const u64 *a, const u64 *b, int lanes, s32 *base, s32 *deriv) {
  for (int i = 0; i < lanes; i++) {
    upd(base, attr(a, 0, 2, i), attr(b, 0, 2, i));
    upd(deriv, attr(a, 1, 3, i), attr(b, 1, 3, i));     // dx
    upd(deriv, attr(a, 4, 6, i), attr(b, 4, 6, i));     // de
    upd(deriv, attr(a, 5, 7, i), attr(b, 5, 7, i));     // dy
  }
}

// Gradients past +-4096 units per pixel (any attribute): a sliver whose planes are numerically
// meaningless (the CPU saturates them differently). Counted, not compared.
static bool sliver(const u64 *b) {
  static const u8 words[] = { 5, 9, 13, 17 };    // dx/dy integer halves of shade + tex
  for (u32 i = 0; i < sizeof words; i++)
    for (int k = 0; k < 4; k++) { s32 v = fx(b[words[i]], k); if (v >= 4096 || v < -4096) return true; }
  return false;
}

static k64_svtx_t svtx_of(const k64_kgfx_vcache_t *c) {
  return (k64_svtx_t){ c->sx / 4.f, c->sy / 4.f, c->sz / 32767.f,
                       (s32)(((u32)(u16)c->invw_i << 16) | c->invw_f) / 65536.f,
                       c->r, c->g, c->b, c->a, c->s / 32.f, c->t / 32.f };
}

static void cmp_tris(tstats_t *st, const u64 *got, const u64 *ref, u32 n);


static void run_tri_test(u32 seed, tstats_t *st) {
  stats_t vst = { 0 };
  u32 us;
  if (!run_test(seed, &vst, &us)) { st->count_bad++; return; }   // fills vc[] + mtx/vp
  u8 idx[NT][3];
  for (int i = 0; i < NT; i++)
    do {
      for (int k = 0; k < 3; k++) idx[i][k] = (u8)(rnd(0, NV - 1));
    } while ((vc[idx[i][0]].flags | vc[idx[i][1]].flags | vc[idx[i][2]].flags) & K64_KGFX_CLIP_CUT);
  const u32 flags = K64_TRI_SHADE | K64_TRI_TEX | K64_TRI_Z;

  // CPU reference, copied out of the ring.
  k64_rdp_wait_idle();
  u64 *start = k64_rdp.cur;
  u32 t0 = k64_count();
  for (int i = 0; i < NT; i++) {
    if (vc[idx[i][0]].flags & vc[idx[i][1]].flags & vc[idx[i][2]].flags & K64_KGFX_CLIP_REJECT) {
      st->rejected++;
      continue;
    }
    k64_svtx_t a = svtx_of(&vc[idx[i][0]]), b = svtx_of(&vc[idx[i][1]]), c = svtx_of(&vc[idx[i][2]]);
    k64_rdp_tri(flags, 0, &a, &b, &c);
  }
  st->cpu_us += K64_US(k64_count() - t0);
  u32 nref = (u32)(k64_rdp.cur - start);
  for (u32 i = 0; i < nref; i++) ref_buf[i] = start[i];

  // RSP: same vertices (reloaded), same triangles.
  k64_rdp_wait_idle();
  start = k64_rdp.cur;
  k64_kgfx_list_t l;
  k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
  k64_kgfx_viewport(&l, &vp);
  k64_kgfx_mtx(&l, &mtx);
  k64_kgfx_vtx(&l, verts, NV, 0);
  k64_kgfx_geommode(&l, flags, 0, 0);
  for (int i = 0; i < NT; i += 2)
    k64_kgfx_tri2(&l, idx[i][0], idx[i][1], idx[i][2], idx[i + 1][0], idx[i + 1][1], idx[i + 1][2]);
  k64_kgfx_end(&l);
  t0 = k64_count();
  k64_kgfx_run(&l);
  u32 status = k64_kgfx_wait();
  st->rsp_us += K64_US(k64_count() - t0);
  const u64 *got = K64_KSEG1(start);
  u32 ngot = (u32)(k64_rdp.cur - start);
  if (status || ngot != nref || ngot % TRI_WORDS) {
    st->count_bad++;
    k64_printf("[04] tri status %x words rsp %u cpu %u\n", status, ngot, nref);
    return;
  }

  cmp_tris(st, got, ref_buf, ngot / TRI_WORDS);
}

static void cmp_tris(tstats_t *st, const u64 *got, const u64 *ref, u32 n) {
  for (u32 t = 0; t < n; t++) {
    const u64 *a = got + t * TRI_WORDS, *b = ref + t * TRI_WORDS;
    st->tris++;
    if (a[0] != b[0]) { st->hdr_bad++; continue; }
    if (sliver(b)) { st->slivers++; continue; }
    {
      static int dumped;
      s32 de = 0, dt = 0;
      for (int e = 1; e <= 3; e++) { upd(&de, (s32)(a[e] >> 32), (s32)(b[e] >> 32)); upd(&de, (s32)a[e], (s32)b[e]); }
      s32 dd = 0;
      cmp_attr4(a + 12, b + 12, 2, &dt, &dd);
      if ((de > 0x1000 || dt > 0x40000 || dd > 0x40000) && dumped < 6) {
        dumped++;
        k64_printf("[04] bad tri %u edge %d tex %d\n", t, de, dt);
        for (int w = 0; w < TRI_WORDS; w++)
          k64_printf("  %2d %08x%08x %08x%08x\n", w, (u32)(a[w] >> 32), (u32)a[w], (u32)(b[w] >> 32), (u32)b[w]);
      }
    }
    // Edge words: 1 = L (YM..YL), 2 = H (YH..YL), 3 = M (YH..YM). xend = X error at the end
    // of the edge (start error + slope error x height): what the rasterizer actually sees.
    s32 yh = (s32)(b[0] & 0x3FFF), ym = (s32)((b[0] >> 16) & 0x3FFF), yl = (s32)((b[0] >> 32) & 0x3FFF);
    const s32 lines[4] = { 0, (yl - ym + 3) / 4, (yl - yh + 3) / 4, (ym - yh + 3) / 4 };
    for (int e = 1; e <= 3; e++) {
      s32 dx = 0, ds = 0;
      upd(&dx, (s32)(a[e] >> 32), (s32)(b[e] >> 32));
      upd(&ds, (s32)a[e], (s32)b[e]);
      if (dx > st->edge) st->edge = dx;
      if (ds > st->slope) st->slope = ds;
      s32 xe = dx + ds * (lines[e] > 1 ? lines[e] : 1);
      if (xe > st->xend) st->xend = xe;
    }
    cmp_attr4(a + 4, b + 4, 4, &st->shade, &st->shade_d);
    // s/w, t/w (lanes 0, 1); W (lane 2) has its own base tolerance: the CPU scales the largest
    // 1/w to 32767, the RSP to (minW - lsb) times its reciprocal, ~4 lsb lower. S/W carries the
    // same scale, so the ratio the RDP divides out is unaffected. Lane 3 is unused.
    cmp_attr4(a + 12, b + 12, 2, &st->tex, &st->tex_d);
    upd(&st->texw, attr(a + 12, 0, 2, 2), attr(b + 12, 0, 2, 2));
    upd(&st->tex_d, attr(a + 12, 1, 3, 2), attr(b + 12, 1, 3, 2));
    upd(&st->tex_d, attr(a + 12, 4, 6, 2), attr(b + 12, 4, 6, 2));
    upd(&st->tex_d, attr(a + 12, 5, 7, 2), attr(b + 12, 5, 7, 2));
    if ((s32)(b[20] >> 32) != 0x7FFFFFFF)                 // CPU float base saturated at the far plane
      upd(&st->z, (s32)(a[20] >> 32), (s32)(b[20] >> 32));
    upd(&st->z_d, (s32)a[20], (s32)b[20]);
    upd(&st->z_d, (s32)(a[21] >> 32), (s32)(b[21] >> 32));
    upd(&st->z_d, (s32)a[21], (s32)b[21]);
    if (st->span_w) {      // z error at the far side of the polygon, in 1/256 z steps
      s64 db = (s64)(s32)(a[20] >> 32) - (s32)(b[20] >> 32), dx = (s64)(s32)a[20] - (s32)b[20];
      s64 de = (s64)(s32)(a[21] >> 32) - (s32)(b[21] >> 32);
      if ((s32)(b[20] >> 32) == 0x7FFFFFFF) db = 0;
      s64 e = (db < 0 ? -db : db) + (dx < 0 ? -dx : dx) * st->span_w + (de < 0 ? -de : de) * st->span_h;
      if ((s32)(e >> 8) > st->z_span) st->z_span = (s32)(e >> 8);
    }
  }
}

// ---- stage C ----------------------------------------------------------------------------
// One TRI per list, so the RSP's clipped vertices can be read back from its DMEM pool after
// the run. Checked separately:
//   - clipped vertices: RSP pool entries vs the float clipper (clip coords, screen, attributes);
//   - structure: same reject / same number of fan triangles, same order;
//   - setup: the reference fans are built from the RSP's own pool entries (as stage B uses the
//     RSP's cached screen vertices), so the stage B tolerances apply unchanged.
#define NTC 24
static u64 cgot_buf[K64_KGFX_CLIP_POOL * TRI_WORDS];

typedef struct { float c[4], r, g, b, a, s, t; int slot; } cvtx_t;   // slot >= 32: clipper pool
typedef struct { s32 clip, scr, z, color, st; u32 n; } vstats_t;

static k64_kgfx_vcache_t pool[K64_KGFX_CLIP_POOL] K64_ALIGN(8);   // RSP pool after the run
static cvtx_t made[K64_KGFX_CLIP_POOL];                            // CPU clipper, same order
static int nmade;
static bool dropped;

static cvtx_t cvtx_of(int slot) {
  const k64_kgfx_vcache_t *v = &vc[slot];
  cvtx_t o = { .r = v->r, .g = v->g, .b = v->b, .a = v->a, .s = v->s, .t = v->t, .slot = slot };
  for (int k = 0; k < 4; k++) o.c[k] = (s32)(((u32)(u16)v->clip_i[k] << 16) | v->clip_f[k]) / 65536.f;
  return o;
}

// Same planes and order as kgfx.S CLIP_PLANE_TABLE.
static const struct { u16 flag; u8 k, mode; } planes[6] = {
  { K64_KGFX_CLIP_NEAR, 2, 0 }, { K64_KGFX_CLIP_FAR, 2, 1 },
  { K64_KGFX_GUARD_NX, 0, 2 }, { K64_KGFX_GUARD_PX, 0, 3 },
  { K64_KGFX_GUARD_NY, 1, 2 }, { K64_KGFX_GUARD_PY, 1, 3 },
};
static float pdist(const cvtx_t *v, int p) {
  float c = planes[p].mode & 1 ? -v->c[planes[p].k] : v->c[planes[p].k];
  return (planes[p].mode & 2 ? c * 0.25f : c) + v->c[3];   // guard: (4w +- c) / 4, as the RSP
}
static cvtx_t lerp(const cvtx_t *i, const cvtx_t *o, float t) {
  cvtx_t n = { .slot = K64_KGFX_VCACHE + nmade };
  for (int k = 0; k < 4; k++) n.c[k] = i->c[k] + (o->c[k] - i->c[k]) * t;
  n.r = i->r + (o->r - i->r) * t; n.g = i->g + (o->g - i->g) * t;
  n.b = i->b + (o->b - i->b) * t; n.a = i->a + (o->a - i->a) * t;
  n.s = i->s + (o->s - i->s) * t; n.t = i->t + (o->t - i->t) * t;
  if (nmade == K64_KGFX_CLIP_POOL || n.c[3] <= 0) dropped = true;   // the RSP drops the triangle
  else made[nmade++] = n;
  return n;
}
static k64_svtx_t project(const cvtx_t *v) {
  return svtx_of(v->slot < K64_KGFX_VCACHE ? &vc[v->slot] : &pool[v->slot - K64_KGFX_VCACHE]);
}

static const k64_kgfx_vcache_t *fan[10];
static int fan_n;

// CPU reference of one TRI: reject / clip / fan, as the microcode does it.
static void ref_tri(u32 flags, const u8 *ix, tstats_t *st) {
  u32 f0 = vc[ix[0]].flags, f1 = vc[ix[1]].flags, f2 = vc[ix[2]].flags;
  nmade = 0; dropped = false;
  if (f0 & f1 & f2 & K64_KGFX_CLIP_REJECT) { st->rejected++; return; }
  cvtx_t buf[2][10];
  int n = 3, cur = 0;
  for (int k = 0; k < 3; k++) buf[0][k] = cvtx_of(ix[k]);
  u32 cut = (f0 | f1 | f2) & K64_KGFX_CLIP_CUT;
  if (cut) st->cut++;
  for (int p = 0; p < 6 && n >= 3 && !dropped; p++) {
    if (!(cut & planes[p].flag)) continue;
    const cvtx_t *in = buf[cur];
    cvtx_t *out = buf[cur ^ 1];
    int m = 0;
    const cvtx_t *prev = &in[n - 1];
    float dp = pdist(prev, p);
    for (int i = 0; i < n && !dropped; i++) {
      const cvtx_t *c = &in[i];
      float dc = pdist(c, p);
      if ((dp < 0) != (dc < 0))
        out[m++] = dp >= 0 ? lerp(prev, c, dp / (dp - dc)) : lerp(c, prev, dc / (dc - dp));
      if (dc >= 0) out[m++] = *c;
      prev = c; dp = dc;
    }
    n = m; cur ^= 1;
  }
  fan_n = 0;
  if (dropped) return;
  fan_n = n;
  for (int i = 0; i < n; i++) {
    int sl = buf[cur][i].slot;
    fan[i] = sl < K64_KGFX_VCACHE ? &vc[sl] : &pool[sl - K64_KGFX_VCACHE];
  }
  for (int i = 1; i + 1 < n; i++) {
    k64_svtx_t a = project(&buf[cur][0]), b = project(&buf[cur][i]), c = project(&buf[cur][i + 1]);
    k64_rdp_tri(flags, 0, &a, &b, &c);
  }
}

static void vupd(s32 *m, float got, float want) {
  s32 d = (s32)(got - want);
  if (d < 0) d = -d;
  if (d > *m) *m = d;
}

// RSP pool entries vs the float clipper's vertices; errors in s15.16 lsb (clip), s13.2 units
// (screen x/y), z steps, color levels and s10.5 units.
static void cmp_made(vstats_t *vs) {
  for (int k = 0; k < nmade; k++) {
    const k64_kgfx_vcache_t *g = &pool[k];
    const cvtx_t *w = &made[k];
    for (int c = 0; c < 4; c++)
      vupd(&vs->clip, (float)(s32)(((u32)(u16)g->clip_i[c] << 16) | g->clip_f[c]), w->c[c] * 65536.f);
    float iw = 1.f / w->c[3];
    vupd(&vs->scr, g->sx, vp.trans[0] + w->c[0] * iw * vp.scale[0]);
    vupd(&vs->scr, g->sy, vp.trans[1] + w->c[1] * iw * vp.scale[1]);
    vupd(&vs->z, g->sz, vp.trans[2] + w->c[2] * iw * vp.scale[2]);
    vupd(&vs->color, g->r, w->r); vupd(&vs->color, g->g, w->g);
    vupd(&vs->color, g->b, w->b); vupd(&vs->color, g->a, w->a);
    vupd(&vs->st, g->s, w->s); vupd(&vs->st, g->t, w->t);
    vs->n++;
  }
}

static void run_clip_test(u32 seed, tstats_t *st, vstats_t *vs) {
  stats_t vst = { 0 };
  u32 us;
  if (!run_test(seed, &vst, &us)) { st->count_bad++; return; }
  const u32 flags = K64_TRI_SHADE | K64_TRI_TEX | K64_TRI_Z;
  for (int i = 0; i < NTC; i++) {
    u8 ix[3];
    for (int k = 0; k < 3; k++) ix[k] = (u8)(rnd(0, NV - 1));

    k64_rdp_wait_idle();
    u64 *start = k64_rdp.cur;
    k64_kgfx_list_t l;
    k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
    k64_kgfx_viewport(&l, &vp);
    k64_kgfx_mtx(&l, &mtx);
    k64_kgfx_vtx(&l, verts, NV, 0);
    k64_kgfx_geommode(&l, flags, 0, 0);
    k64_kgfx_tri(&l, ix[0], ix[1], ix[2]);
    k64_kgfx_end(&l);
    k64_kgfx_run(&l);
    u32 status = k64_kgfx_wait();
    u32 ngot = (u32)(k64_rdp.cur - start);
    const u64 *got = K64_KSEG1(start);
    for (u32 w = 0; w < ngot && w < K64_ARRAY_LEN(cgot_buf); w++) cgot_buf[w] = got[w];
    k64_kgfx_read_vcache(pool, K64_KGFX_VCACHE, K64_KGFX_CLIP_POOL);

    k64_rdp_wait_idle();
    start = k64_rdp.cur;
    ref_tri(flags, ix, st);
    u32 nref = (u32)(k64_rdp.cur - start);
    cmp_made(vs);
    if (status || ngot != nref || ngot % TRI_WORDS) {
      if (st->count_bad++ < 4)
        k64_printf("[04] clip tri %d %d %d flags %x %x %x status %x words rsp %u cpu %u\n", ix[0], ix[1], ix[2],
                   vc[ix[0]].flags, vc[ix[1]].flags, vc[ix[2]].flags, status, ngot, nref);
      continue;
    }
    s32 x0 = 0x7FFF, x1 = -0x8000, y0 = 0x7FFF, y1 = -0x8000;
    for (int k = 0; k < fan_n; k++) {
      if (fan[k]->sx < x0) x0 = fan[k]->sx;
      if (fan[k]->sx > x1) x1 = fan[k]->sx;
      if (fan[k]->sy < y0) y0 = fan[k]->sy;
      if (fan[k]->sy > y1) y1 = fan[k]->sy;
    }
    st->span_w = (u32)(x1 - x0 + 3) / 4;
    st->span_h = (u32)(y1 - y0 + 3) / 4;
    cmp_tris(st, cgot_buf, start, ngot / TRI_WORDS);   // CPU wrote it through D$
  }
}

// ---- stage E ----------------------------------------------------------------------------
static k64_kgfx_lights_t lights;

static s32 clamp16(s64 v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (s32)v; }

// The microcode's lighting of channel `c` of vertex `v`, bit exact.
static u8 light_fixed(const k64_vtx_t *v, int c) {
  u16 p = v->norm;
  s32 n[3] = { (s16)(p & 0xF800), (s16)((p << 5) & 0xFC00), (s16)((p << 11) & 0xF800) };
  s32 dot[K64_KGFX_LIGHTS];
  for (int i = 0; i < K64_KGFX_LIGHTS; i++) {
    s64 acc = 0x8000;
    for (int k = 0; k < 3; k++) acc += (s64)lights.dir[k][i] * n[k] * 2;
    dot[i] = clamp16(acc >> 16);
    if (dot[i] < 0) dot[i] = 0;
  }
  s64 acc = (s64)lights.amb[c] << 16;
  for (int i = 0; i < K64_KGFX_LIGHTS; i++) acc += (s64)lights.col[i][c] * dot[i] * 2;
  s32 l = clamp16(acc >> 16);
  s32 col = (&v->r)[c] << 7;
  s32 r = clamp16(((s64)l * col * 2 + 0x8000) >> 16);
  r = clamp16((s64)r + r);
  return (u8)(r >> 7);
}

static void run_light_test(u32 seed, u32 *bad, u32 *checked, s32 *maxf) {
  rng = seed;
  static const u8 lc[K64_KGFX_LIGHTS][3] = { { 255, 240, 200 }, { 40, 60, 200 }, { 180, 30, 30 }, { 0, 120, 0 } };
  k64_kgfx_light_t lt[K64_KGFX_LIGHTS];
  for (int i = 0; i < K64_KGFX_LIGHTS; i++)
    lt[i] = (k64_kgfx_light_t){ { rnd(-100, 100) / 100.f, rnd(-100, 100) / 100.f, rnd(1, 100) / 100.f },
                                lc[i][0], lc[i][1], lc[i][2] };
  u32 nl = (seed & 1) ? 4 : 2;
  k64_mat4_t model;
  k64_mat4_rotate_y(&model, (seed & 255) * 0.1f);
  u8 amb = (u8)rnd(0, 80);
  k64_kgfx_lights_set(&lights, lt, nl, amb, amb, (u8)(amb / 2), &model);
  for (int i = 0; i < NV; i++) {
    float x = rnd(-1000, 1000), y = rnd(-1000, 1000), z = rnd(-1000, 1000);
    float inv = 1.f / k64_sqrtf(x * x + y * y + z * z + 1.f);
    verts[i] = (k64_vtx_t){ rnd(-4096, 4096), rnd(-4096, 4096), rnd(-4096, 4096),
                            k64_kgfx_pack_normal(x * inv, y * inv, z * inv),
                            (u8)rnd(0, 255), (u8)rnd(0, 255), (u8)rnd(0, 255), (u8)rnd(0, 255), 0, 0 };
  }
  k64_dcache_writeback(verts, sizeof verts);
  k64_dcache_writeback(&lights, sizeof lights);
  k64_kgfx_list_t l;
  k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
  k64_kgfx_viewport(&l, &vp);
  k64_kgfx_mtx(&l, &mtx);
  k64_kgfx_lights(&l, &lights);
  k64_kgfx_geommode(&l, K64_TRI_SHADE, 0, K64_KGFX_LIGHTING);
  k64_kgfx_vtx(&l, &verts[31], 1, 31);
  k64_kgfx_vtx(&l, &verts[0], 31, 0);
  k64_kgfx_geommode(&l, K64_TRI_SHADE, 0, 0);
  k64_kgfx_end(&l);
  k64_kgfx_run(&l);
  u32 status = k64_kgfx_wait();
  if (status) { k64_printf("[04] light status %x\n", status); (*bad)++; return; }
  k64_kgfx_read_vcache(vc, 0, NV);
  for (int i = 0; i < NV; i++) {
    const k64_vtx_t *v = &verts[i];
    u16 p = v->norm;
    // decoded normal (as the microcode sees it, unscaled) rotated to world space
    float nd[3] = {(s8)((p >> 8) & 0xF8) / 8 / 15.5f, (s8)((p >> 3) & 0xFC) / 4 / 31.5f,
                   (s8)((p << 3) & 0xF8) / 8 / 15.5f};
    float nw[3];
    for (int r = 0; r < 3; r++) nw[r] = model.m[0][r] * nd[0] + model.m[1][r] * nd[1] + model.m[2][r] * nd[2];
    for (int c = 0; c < 4; c++) {
      u8 got = (&vc[i].r)[c], want = light_fixed(v, c);
      (*checked)++;
      if (got != want) {
        if (*bad < 4) k64_printf("[04] light v%d c%d got %u want %u\n", i, c, got, want);
        (*bad)++;
      }
      float f = c == 3 ? 1.f : (c == 2 ? amb / 2 : amb) / 255.f;
      for (u32 k = 0; k < nl && c < 3; k++) {
        const k64_vec3_t *d = &lt[k].dir;
        float dl = (nw[0] * d->x + nw[1] * d->y + nw[2] * d->z) / k64_sqrtf(d->x * d->x + d->y * d->y + d->z * d->z);
        if (dl > 0) f += dl * lc[k][c] / 255.f;
      }
      if (f > 2.f) f = 2.f;
      float wf = (&v->r)[c] * f;
      if (wf > 255.f) wf = 255.f;
      float e = got - wf;
      if (e < 0) e = -e;
      if ((s32)(e + 0.5f) > *maxf) *maxf = (s32)(e + 0.5f);
    }
  }
}

static k64_kgfx_texgen_t texgen;

// The microcode's texgen of coordinate `a` (0 = s, 1 = t) of vertex `v`, bit exact.
static s16 texgen_fixed(const k64_vtx_t *v, int a) {
  u16 p = v->norm;
  s32 n[3] = { (s16)(p & 0xF800), (s16)((p << 5) & 0xFC00), (s16)((p << 11) & 0xF800) };
  s64 acc = 0x8000;
  for (int k = 0; k < 3; k++) acc += (s64)texgen.dir[k][a] * n[k] * 2;
  s32 dot = clamp16(acc >> 16);
  acc = ((s64)texgen.off[a] << 16) + (s64)texgen.scale[a] * dot * 2;
  return (s16)clamp16(acc >> 16);
}

static void run_texgen_test(u32 seed, u32 *bad, u32 *checked, s32 *maxf) {
  rng = seed;
  static const u16 sizes[4][2] = { { 32, 32 }, { 64, 32 }, { 16, 64 }, { 128, 128 } };
  u32 tw = sizes[seed & 3][0], th = sizes[seed & 3][1];
  k64_mat4_t rx, ry, mv;
  k64_mat4_rotate_x(&rx, rnd(-314, 314) / 100.f);
  k64_mat4_rotate_y(&ry, rnd(-314, 314) / 100.f);
  k64_mat4_mul(&mv, &rx, &ry);
  k64_kgfx_texgen_set(&texgen, &mv, tw, th);
  for (int i = 0; i < NV; i++) {
    float x = rnd(-1000, 1000), y = rnd(-1000, 1000), z = rnd(-1000, 1000);
    float inv = 1.f / k64_sqrtf(x * x + y * y + z * z + 1.f);
    verts[i] = (k64_vtx_t){ rnd(-4096, 4096), rnd(-4096, 4096), rnd(-4096, 4096),
                            k64_kgfx_pack_normal(x * inv, y * inv, z * inv), 255, 255, 255, 255,
                            (s16)rnd(-999, 999), (s16)rnd(-999, 999) };
  }
  k64_dcache_writeback(verts, sizeof verts);
  k64_dcache_writeback(&texgen, sizeof texgen);
  k64_kgfx_list_t l;
  k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
  k64_kgfx_viewport(&l, &vp);
  k64_kgfx_mtx(&l, &mtx);
  k64_kgfx_texgen(&l, &texgen);
  k64_kgfx_geommode(&l, K64_TRI_TEX, 0, K64_KGFX_TEXGEN);
  k64_kgfx_vtx(&l, &verts[0], 7, 0);        // odd count: the pass rounds up to pairs
  k64_kgfx_vtx(&l, &verts[7], NV - 7, 7);
  k64_kgfx_geommode(&l, K64_TRI_SHADE, 0, 0);
  k64_kgfx_end(&l);
  k64_kgfx_run(&l);
  u32 status = k64_kgfx_wait();
  if (status) { k64_printf("[04] texgen status %x\n", status); (*bad)++; return; }
  k64_kgfx_read_vcache(vc, 0, NV);
  float ax[2][3];
  for (int a = 0; a < 2; a++) {
    float len = k64_sqrtf(mv.m[0][a] * mv.m[0][a] + mv.m[1][a] * mv.m[1][a] + mv.m[2][a] * mv.m[2][a]);
    for (int k = 0; k < 3; k++) ax[a][k] = (a ? -mv.m[k][a] : mv.m[k][a]) / len;
  }
  for (int i = 0; i < NV; i++) {
    const k64_vtx_t *v = &verts[i];
    u16 p = v->norm;
    float nd[3] = {(s8)((p >> 8) & 0xF8) / 8 / 15.5f, (s8)((p >> 3) & 0xFC) / 4 / 31.5f,
                   (s8)((p << 3) & 0xF8) / 8 / 15.5f};
    for (int a = 0; a < 2; a++) {
      s16 got = a ? vc[i].t : vc[i].s, want = texgen_fixed(v, a);
      (*checked)++;
      if (got != want) {
        if (*bad < 4) k64_printf("[04] texgen v%d %c got %d want %d\n", i, a ? 't' : 's', got, want);
        (*bad)++;
      }
      float size = a ? th : tw;
      float f = size * 16.f * (1.f + nd[0] * ax[a][0] + nd[1] * ax[a][1] + nd[2] * ax[a][2]);
      float e = got - f;
      if (e < 0) e = -e;
      if ((s32)(e + 0.5f) > *maxf) *maxf = (s32)(e + 0.5f);
    }
  }
}

// Indexed colours: a CALLed list loads two colour tables (COL) from a base that is 4 bytes
// off an 8-byte boundary and runs VTX_COLIDX after each; every vertex must come out with the
// table entry its `a` names (no lighting or fog: the colour passes through).
static u32 coltab[2 * 64 + 4] K64_ALIGN(16);
static u64 sub_buf[16] K64_ALIGN(16);

static void run_colidx_test(u32 seed, u32 *bad, u32 *checked) {
  rng = seed;
  for (u32 i = 0; i < K64_ARRAY_LEN(coltab); i++) coltab[i] = ((u32)rnd(0, 0xFFFF) << 16) | (u32)rnd(0, 0xFFFF);
  const u32 *base = &coltab[1];               // 4 bytes past an 8-byte boundary
  const u32 ofs[2] = { 0, 60 * 4 };            // second table: base + 240 (also misaligned)
  const u32 n = 24;
  u8 idx[NV];
  for (u32 i = 0; i < n; i++) {
    idx[i] = (u8)rnd(0, 63);
    u32 mis = (K64_PHYS(base) + ofs[i >= 12]) & 7;
    verts[i] = (k64_vtx_t){ rnd(-4096, 4096), rnd(-4096, 4096), rnd(-4096, 4096), 0,
                            0, 0, 0, (u8)(mis + idx[i] * 4), 0, 0 };
  }
  k64_dcache_writeback(verts, sizeof verts);
  k64_dcache_writeback(coltab, sizeof coltab);
  k64_kgfx_list_t sl;
  k64_kgfx_begin(&sl, sub_buf, K64_ARRAY_LEN(sub_buf));
  k64_kgfx_col(&sl, ofs[0]);
  k64_kgfx_vtx_colidx(&sl, &verts[0], 12, 0);
  k64_kgfx_col(&sl, ofs[1]);
  k64_kgfx_vtx_colidx(&sl, &verts[12], 12, 12);
  k64_kgfx_ret(&sl);
  k64_dcache_writeback(sub_buf, sizeof sub_buf);
  k64_kgfx_list_t l;
  k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
  k64_kgfx_viewport(&l, &vp);
  k64_kgfx_mtx(&l, &mtx);
  k64_kgfx_geommode(&l, K64_TRI_SHADE, 0, 0);
  k64_kgfx_call_col(&l, sub_buf, base);
  k64_kgfx_end(&l);
  k64_kgfx_run(&l);
  u32 status = k64_kgfx_wait();
  if (status) { k64_printf("[04] colidx status %x\n", status); (*bad)++; return; }
  k64_kgfx_read_vcache(vc, 0, n);
  for (u32 i = 0; i < n; i++) {
    u32 want = base[ofs[i >= 12] / 4 + idx[i]];
    u32 got = (u32)vc[i].r << 24 | (u32)vc[i].g << 16 | (u32)vc[i].b << 8 | vc[i].a;
    (*checked)++;
    if (got != want) {
      if (*bad < 4) k64_printf("[04] colidx v%u got %08x want %08x\n", i, got, want);
      (*bad)++;
    }
  }
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();

  // The triangle test makes the RDP draw: give it a surface first.
  const k64_surface_t *fb0 = k64_display_acquire();
  k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb0->width, fb0->height, fb0->pixels, NULL);

  stats_t st = { 0 };
  u32 rsp_us = 0, runs = 0;
  bool ok = true;
  for (u32 s = 1; s <= 16 && ok; s++, runs++) ok = run_test(s * 2654435761u, &st, &rsp_us);
  u32 rsp_us1 = 0;
  scene = 1;
  for (u32 s = 1; s <= 8 && ok; s++, runs++) ok = run_test(s * 2654435761u, &st, &rsp_us1);
  scene = 0;
  // Tolerances: clip exact; 1/w within 2 lsb; screen within 1 s13.2 unit (0.25 px); z within 4.
  bool pass = ok && st.max_clip == 0 && st.attr_bad == 0 && st.flag_bad == 0 && st.max_invw <= 2 &&
              st.max_sx <= 1 && st.max_sy <= 1 && st.max_sz <= 4;
  k64_printf("[04] runs %u verts %u (cut %u) clip %d invw %u lsb sx %d sy %d sz %d attr %u flags %u rsp %u us -> %s\n",
             runs, st.checked, st.cut, st.max_clip, st.max_invw, st.max_sx, st.max_sy, st.max_sz, st.attr_bad,
             st.flag_bad, rsp_us, pass ? "PASS" : "FAIL");

  tstats_t ts = { 0 };
  for (u32 s = 1; s <= 8; s++) run_tri_test(s * 0x9E3779B9u, &ts);
  k64_printf(
      "[04] tris %u (slivers %u, rejected %u) hdr %u count %u edge %d slope %d shade %d/%d tex %d/%d w %d z %d/%d "
      "(base/deriv) rsp %u us cpu %u us\n",
      ts.tris, ts.slivers, ts.rejected, ts.hdr_bad, ts.count_bad, ts.edge, ts.slope, ts.shade, ts.shade_d, ts.tex,
      ts.tex_d, ts.texw, ts.z, ts.z_d, ts.rsp_us, ts.cpu_us);
  // Tolerances (s15.16 lsb): edges/slopes 1/256 px; shade 1/64 and 1/16 of a level; tex 4 units
  // of s10.5, W base 8 of 32767 (a scale difference, see cmp_tris); z 1/16 and 1/8 of a step.
  bool tpass = ts.tris > 0 && ts.hdr_bad == 0 && ts.count_bad == 0 && ts.edge <= 256 && ts.slope <= 256
               && ts.shade <= 1024 && ts.shade_d <= 4096 && ts.tex <= 0x40000 && ts.tex_d <= 0x40000
               && ts.texw <= 0x80000 && ts.z <= 0x1000 && ts.z_d <= 0x2000;
  k64_printf("[04] triangle setup -> %s\n", tpass ? "PASS" : "FAIL");

  tstats_t cs = { 0 };
  vstats_t vs = { 0 };
  scene = 1;
  for (u32 s = 1; s <= 16; s++) run_clip_test(s * 0x85EBCA6Bu, &cs, &vs);
  scene = 0;
  k64_printf("[04] clip: %u verts made: clip %d lsb, screen %d, z %d, color %d, st %d\n",
             vs.n, vs.clip, vs.scr, vs.z, vs.color, vs.st);
  k64_printf(
      "[04] clip: %u tris cut, %u rejected -> %u out (slivers %u) hdr %u count %u edge %d slope %d (x at edge end %d) "
      "shade %d/%d tex %d/%d w %d z %d/%d (span %d/256 steps)\n",
      cs.cut, cs.rejected, cs.tris, cs.slivers, cs.hdr_bad, cs.count_bad, cs.edge, cs.slope, cs.xend, cs.shade,
      cs.shade_d, cs.tex, cs.tex_d, cs.texw, cs.z, cs.z_d, cs.z_span);
  // Clipped vertices: clip within 16 lsb, screen 1 s13.2 unit, z 4, color and st 2 (both
  // truncate). Setup with the RSP's own vertices: stage B tolerances, except edges are judged
  // by the X error at the edge end (<= 1/16 px), z base 1/8 step and the z error at the far
  // side of the polygon (<= 128 steps): guard-band triangles span up to 4x the screen,
  // near-horizontal edges have slopes of 100 px/line and near the near plane z changes by
  // ~2000 steps per pixel, where the 15-16 bit 1/NZ' (~4e-5 relative) shows.
  bool cpass = cs.cut > 0 && cs.tris > 0 && vs.n > 0 && cs.hdr_bad == 0 && cs.count_bad == 0 &&
               vs.clip <= 16 && vs.scr <= 1 && vs.z <= 4 && vs.color <= 2 && vs.st <= 2 &&
               cs.edge <= 256 && cs.xend <= 0x1000 && cs.shade <= 1024 && cs.shade_d <= 4096 &&
               cs.tex <= 0x40000 && cs.tex_d <= 0x40000 && cs.texw <= 0x80000 && cs.z <= 0x2000 && cs.z_span <= 0x8000;
  k64_printf("[04] clipping -> %s\n", cpass ? "PASS" : "FAIL");

  u32 lbad = 0, lchecked = 0;
  s32 lmaxf = 0;
  for (u32 s = 1; s <= 8; s++) run_light_test(s * 0x27D4EB2Fu, &lbad, &lchecked, &lmaxf);
  bool lpass = lchecked > 0 && lbad == 0 && lmaxf <= 2;
  k64_printf("[04] lighting: %u channels, %u differ from the fixed-point model, float error %d -> %s\n",
             lchecked, lbad, lmaxf, lpass ? "PASS" : "FAIL");

  u32 gbad = 0, gchecked = 0;
  s32 gmaxf = 0;
  for (u32 s = 1; s <= 8; s++) run_texgen_test(s * 0x9E3779B9u, &gbad, &gchecked, &gmaxf);
  bool gpass = gchecked > 0 && gbad == 0 && gmaxf <= 2;
  k64_printf("[04] texgen: %u coordinates, %u differ from the fixed-point model, float error %d -> %s\n",
             gchecked, gbad, gmaxf, gpass ? "PASS" : "FAIL");

  u32 cbad = 0, cchecked = 0;
  for (u32 s = 1; s <= 8; s++) run_colidx_test(s * 0x165667B1u, &cbad, &cchecked);
  bool cipass = cchecked > 0 && cbad == 0;
  k64_printf("[04] indexed colours: %u vertices, %u wrong -> %s\n", cchecked, cbad, cipass ? "PASS" : "FAIL");
  k64_rdp_wait_idle();
  k64_display_show(fb0);

  for (;;) {
    const k64_surface_t *fb = k64_display_acquire();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, NULL);
    k64_rdp_clear(k64_rgba16(16, 20, 40, 255) * 0x10001u, false);
    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  04 kgfx self-test");
    k64_text_printf(8, 32, 0xC0C0C0FF, "%u runs, %u vertices", runs, st.checked);
    k64_text_printf(8, 48, 0xC0C0C0FF, "clip err  %d (exact)", st.max_clip);
    k64_text_printf(8, 60, 0xC0C0C0FF, "1/w err   %u lsb", st.max_invw);
    k64_text_printf(8, 72, 0xC0C0C0FF, "screen    x %d  y %d  z %d", st.max_sx, st.max_sy, st.max_sz);
    k64_text_printf(8, 84, 0xC0C0C0FF, "attr %u  flags %u", st.attr_bad, st.flag_bad);
    k64_text_printf(8, 100, 0xC0C0C0FF, "32 verts on RSP: %u us", rsp_us);
    k64_text_printf(8, 124, pass ? 0x40FF40FF : 0xFF4040FF, "vertices  %s", pass ? "PASS" : "FAIL");
    k64_text_printf(8, 148, 0xC0C0C0FF, "%u tris (+%u slivers) vs CPU setup", ts.tris - ts.slivers, ts.slivers);
    k64_text_printf(8, 160, 0xC0C0C0FF, "edge %d slope %d  shade %d/%d", ts.edge, ts.slope, ts.shade, ts.shade_d);
    k64_text_printf(8, 172, 0xC0C0C0FF, "tex %d/%d  z %d/%d lsb", ts.tex, ts.tex_d, ts.z, ts.z_d);
    k64_text_printf(8, 184, 0xC0C0C0FF, "64 tris: RSP %u us  CPU %u us", ts.rsp_us / 8, ts.cpu_us / 8);
    k64_text_printf(8, 200, tpass ? 0x40FF40FF : 0xFF4040FF, "triangles %s", tpass ? "PASS" : "FAIL");
    k64_rdp_wait_idle();
    k64_display_show(fb);
  }
}
