// Demo 21 — particles: the kgfx PARTICLES command. Each particle is a point through the
// matrix and one screen-aligned RDP rectangle sized by perspective (no triangles, no vertex
// cache). A fountain of textured sparks (4 sprites in one atlas, per-particle color, z against
// the floor) over a triangle floor. Self-test first: positions, sizes, rejects, edge cuts and
// prim depth against CPU projection.
#include <k64/k64.h>

#define UNIT  64                            // model units per world unit
#define NPART 768
#define ATLAS 32                            // 2 x 2 sprites of 16 texels

static u8 blk_buf[16 + NPART * 16] K64_ALIGN(16);
static k64_particles_t *const blk = (k64_particles_t *)blk_buf;
static u16 atlas_px[ATLAS * ATLAS] K64_ALIGN(16);
static u16 quad_px[16 * 16] K64_ALIGN(16);
static k64_vtx_t floor_v[4] K64_ALIGN(16);
#define NTREE 8
static k64_vtx_t tree_v[NTREE][4] K64_ALIGN(16);
static u16 tree_px[16 * 32] K64_ALIGN(16);
static k64_mtxfx_t mtx K64_ALIGN(16);
static k64_viewport_t vp K64_ALIGN(16);
static u64 list_buf[256] K64_ALIGN(16);
static u16 *zbuf;
static k64_mat4_t mvp;                      // world (model units) -> clip

static u32 fails;
#define CHECK(c, ...)                                 \
  do {                                                \
    if (!(c)) {                                       \
      fails++;                                        \
      k64_printf("[21_particles] FAIL " __VA_ARGS__); \
      k64_printf("\n");                               \
    }                                                 \
  } while (0)

static void set_camera(k64_vec3_t eye, k64_vec3_t at) {
  k64_mat4_t proj, view, vpm, sc;
  k64_mat4_perspective(&proj, 60.f * K64_PI / 180.f, 320.f / 240.f, 1.f, 100.f);
  k64_mat4_lookat(&view, eye, at, (k64_vec3_t){ 0, 1, 0 });
  k64_mat4_mul(&vpm, &proj, &view);
  k64_mat4_scale(&sc, 1.f / UNIT, 1.f / UNIT, 1.f / UNIT);
  k64_mat4_mul(&mvp, &vpm, &sc);
  k64_mtx_to_fixed(&mtx, &mvp);
  k64_dcache_writeback(&mtx, sizeof mtx);
}

// Runs one list: [floor triangles], particles.
static void run(bool floor, u32 count, u32 flags) {
  blk->scale = k64_kgfx_particles_scale(&mvp, &vp);
  blk->count = (u16)count;
  blk->flags = (u8)flags;
  blk->tile = 0;
  blk->tex_shift = 4;
  k64_dcache_writeback(blk_buf, 16 + count * 16);
  k64_kgfx_list_t l;
  k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
  k64_kgfx_viewport(&l, &vp);
  k64_kgfx_mtx(&l, &mtx);
  if (floor) {
    k64_kgfx_geommode(&l, K64_TRI_SHADE | K64_TRI_Z, 0, 0);
    k64_kgfx_vtx(&l, floor_v, 4, 0);
    k64_kgfx_tri2(&l, 0, 1, 2, 0, 2, 3);
  }
  if (count) k64_kgfx_particles(&l, blk);
  k64_kgfx_end(&l);
  k64_kgfx_run(&l);
  u32 st = k64_kgfx_wait();
  if (st) k64_printf("[21_particles] kgfx status %x\n", st);
}

static void put(u32 i, float x, float y, float z, float size, u32 rgba, int sprite) {
  blk->p[i] = (k64_particle_t){ (s16)(x * UNIT), (s16)(y * UNIT), (s16)(z * UNIT), (u16)(size * UNIT),
                                (u8)(rgba >> 24), (u8)(rgba >> 16), (u8)(rgba >> 8), (u8)rgba,
                                (s16)((sprite & 1) * 16 * 32), (s16)((sprite >> 1) * 16 * 32) };
}

// CPU reference: screen center and radius in pixels of particle i.
static void project(u32 i, float *sx, float *sy, float *r) {
  const k64_particle_t *p = &blk->p[i];
  float c[4];
  for (int k = 0; k < 4; k++)
    c[k] = mvp.m[0][k] * p->x + mvp.m[1][k] * p->y + mvp.m[2][k] * p->z + mvp.m[3][k];
  *sx = 160.f + 160.f * c[0] / c[3];
  *sy = 120.f - 120.f * c[1] / c[3];
  float row = k64_sqrtf(mvp.m[0][0] * mvp.m[0][0] + mvp.m[1][0] * mvp.m[1][0] + mvp.m[2][0] * mvp.m[2][0]);
  *r = p->size * row * 160.f / c[3];
}

// FNV-1a over rows 24..207 (no text there): the scene is a function of the frame number only,
// so this pins the RSP / RDP output of a frame across microcode changes.
static u32 scene_hash(const k64_surface_t *fb) {
  const u32 *p = K64_KSEG1((const u16 *)fb->pixels + 24 * 320);
  u32 h = 2166136261u;
  for (u32 i = 0; i < 184 * 320 / 2; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

static u16 px(const k64_surface_t *fb, float x, float y) {
  return ((const u16 *)fb->pixels)[(int)y * 320 + (int)x];
}

static void self_test(const k64_surface_t *fb) {
  const u16 bg = (u16)k64_rgba16(16, 16, 32, 255);
  set_camera((k64_vec3_t){ 0, 0, 10 }, (k64_vec3_t){ 0, 0, 0 });
  // 1. flat colored squares: center, inside / outside the radius, reject behind the eye,
  //    cut at the left edge
  k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, 320, 240, fb->pixels, NULL);
  k64_rdp_clear(bg * 0x10001u, false);
  k64_rdp_set_other_modes(K64_OM_1C_OPAQUE);
  k64_rdp_set_combine(K64_CC_PRIM);
  float edge = 10.f * 0.57735f * 320.f / 240.f;  // half width at z = 0
  put(0, 0, 0, 0, 0.5f, 0xF80000FF, 0);
  put(1, -3, 2, -5, 0.6f, 0x00F800FF, 0);
  put(2, 3, -2, 3, 0.3f, 0x0000F8FF, 0);
  put(3, 0, 0, 20, 1.f, 0xF8F8F8FF, 0);         // behind the eye
  put(4, -edge, -1, 0, 1.f, 0xF8F800FF, 0);     // centered on x = 0
  put(5, 0, 0, -200, 1.f, 0xF8F8F8FF, 0);       // past the far plane
  run(false, 6, K64_PART_COLOR);
  k64_rdp_wait_idle();
  k64_dcache_invalidate(fb->pixels, 320 * 240 * 2);
  CHECK(k64_kgfx_part_count() == 4, "rect count %u", k64_kgfx_part_count());
  for (u32 i = 0; i < 5; i++) {
    if (i == 3) continue;
    float sx, sy, r;
    project(i, &sx, &sy, &r);
    const k64_particle_t *p = &blk->p[i];
    u16 c = (u16)k64_rgba16(p->r, p->g, p->b, 255);
    CHECK(px(fb, sx > 0 ? sx : 0, sy) == c, "p%u center (%d,%d) %04x vs %04x", i, (int)sx, (int)sy,
          px(fb, sx > 0 ? sx : 0, sy), c);
    CHECK(px(fb, sx + r - 1.5f, sy) == c && px(fb, sx, sy - r + 1.5f) == c, "p%u inside r=%d", i, (int)r);
    CHECK(px(fb, sx + r + 1.5f, sy) == bg && px(fb, sx, sy + r + 1.5f) == bg, "p%u outside r=%d", i, (int)r);
  }
  CHECK(px(fb, 160, 120 - 50) == bg, "behind-eye particle drawn");

  // 2. textured: quadrant colors locate the texels; cuts at the left and the top edge must
  //    move the texture start with them
  static const u16 qc[4] = { 0xF801, 0x07C1, 0x003F, 0xFFC1 };   // red, green, blue, yellow
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) quad_px[y * 16 + x] = qc[(y >= 8) * 2 + (x >= 8)];
  k64_dcache_writeback(quad_px, sizeof quad_px);
  k64_tex_t qt = { .width = 16, .height = 16, .fmt = K64_FMT_RGBA, .siz = K64_SIZ_16,
                   .line_bytes = 32, .tmem_line = 4, .pixels = quad_px };
  k64_rdp_clear(bg * 0x10001u, false);
  k64_rdp_set_other_modes(K64_OM_1C_OPAQUE);
  k64_rdp_set_combine(K64_CC_TEX);
  k64_rdp_load_tex(0, 0, &qt, K64_TEX_CLAMP, K64_TEX_CLAMP);
  float top = 10.f * 0.57735f;
  put(0, 0, 0, 0, 1.f, 0, 0);
  put(1, -edge, 1, 0, 1.f, 0, 0);
  put(2, 2, top, 0, 1.f, 0, 0);
  run(false, 3, K64_PART_TEX);
  k64_rdp_wait_idle();
  k64_dcache_invalidate(fb->pixels, 320 * 240 * 2);
  float sx, sy, r;
  project(0, &sx, &sy, &r);
  for (int q = 0; q < 4; q++) {
    float x = sx + (q & 1 ? r : -r) / 2, y = sy + (q & 2 ? r : -r) / 2;
    CHECK(px(fb, x, y) == qc[q], "tex quadrant %d %04x", q, px(fb, x, y));
  }
  project(1, &sx, &sy, &r);
  CHECK(px(fb, 3, sy - r / 2) == qc[1] && px(fb, 3, sy + r / 2) == qc[3], "left cut %04x %04x",
        px(fb, 3, sy - r / 2), px(fb, 3, sy + r / 2));
  project(2, &sx, &sy, &r);
  CHECK(px(fb, sx - r / 2, 3) == qc[2] && px(fb, sx + r / 2, 3) == qc[3], "top cut %04x %04x",
        px(fb, sx - r / 2, 3), px(fb, sx + r / 2, 3));

  // 3. prim depth against a z-buffered quad at z = 0: the one behind it is hidden
  k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, 320, 240, fb->pixels, zbuf);
  k64_rdp_clear(bg * 0x10001u, true);
  k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF);
  k64_rdp_set_combine(K64_CC_SHADE);
  for (int i = 0; i < 4; i++)
    floor_v[i] = (k64_vtx_t){ (s16)((i == 1 || i == 2 ? 3 : -3) * UNIT), (s16)((i >= 2 ? 3 : -3) * UNIT), 0, 0,
                              128, 128, 128, 255, 0, 0 };
  k64_dcache_writeback(floor_v, sizeof floor_v);
  put(0, -1, 0, -3, 0.4f, 0xF80000FF, 0);       // behind the quad
  put(1, 1, 0, 3, 0.4f, 0x00F800FF, 0);         // in front
  put(2, 5, 0, -3, 0.4f, 0x0000F8FF, 0);        // behind, beside it
  // the triangles first (shade combine), then the particles with prim z: two lists
  run(true, 0, 0);
  k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF | K64_OM_Z_SOURCE_PRIM);
  k64_rdp_set_combine(K64_CC_PRIM);
  run(false, 3, K64_PART_COLOR | K64_PART_Z);
  k64_rdp_wait_idle();
  k64_dcache_invalidate(fb->pixels, 320 * 240 * 2);
  u16 gray = (u16)k64_rgba16(128, 128, 128, 255);
  project(0, &sx, &sy, &r);
  CHECK(px(fb, sx, sy) == gray, "z: hidden particle drawn %04x", px(fb, sx, sy));
  project(1, &sx, &sy, &r);
  CHECK(px(fb, sx, sy) == (u16)k64_rgba16(0, 248, 0, 255), "z: front particle %04x", px(fb, sx, sy));
  project(2, &sx, &sy, &r);
  CHECK(px(fb, sx, sy) == (u16)k64_rgba16(0, 0, 248, 255), "z: free particle %04x", px(fb, sx, sy));

  // 4. Y billboards seen from two sides of the eye: both face it (cull back keeps them),
  //    spanning the projected base..top
  k64_rdp_clear(bg * 0x10001u, true);
  k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF);
  k64_rdp_set_combine(K64_CC_SHADE);
  k64_vec3_t eye = { 0, 0, 10 * UNIT };
  k64_kgfx_billboard_y(tree_v[0], (k64_vec3_t){-3 * UNIT, -1 * UNIT, 0}, 0.5f * UNIT, 2 * UNIT, eye, 0xF80000FF, 0, 0,
                       0, 0);
  k64_kgfx_billboard_y(tree_v[1], (k64_vec3_t){4 * UNIT, -2 * UNIT, -4 * UNIT}, 0.5f * UNIT, 2 * UNIT, eye, 0x00F800FF,
                       0, 0, 0, 0);
  k64_dcache_writeback(tree_v, sizeof tree_v);
  k64_kgfx_list_t l;
  k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
  k64_kgfx_viewport(&l, &vp);
  k64_kgfx_mtx(&l, &mtx);
  k64_kgfx_geommode(&l, K64_TRI_SHADE | K64_TRI_Z, 0, K64_KGFX_CULL_BACK);
  k64_kgfx_vtx(&l, tree_v[0], 8, 0);
  k64_kgfx_tri2(&l, 0, 1, 2, 0, 2, 3);
  k64_kgfx_tri2(&l, 4, 5, 6, 4, 6, 7);
  k64_kgfx_end(&l);
  k64_kgfx_run(&l);
  k64_kgfx_wait();
  k64_rdp_wait_idle();
  k64_dcache_invalidate(fb->pixels, 320 * 240 * 2);
  for (u32 b = 0; b < 2; b++) {
    u16 c = b ? (u16)k64_rgba16(0, 248, 0, 255) : (u16)k64_rgba16(248, 0, 0, 255);
    put(0, tree_v[b][0].x / (float)UNIT, tree_v[b][0].y / (float)UNIT + 0.1f, tree_v[b][0].z / (float)UNIT, 0, 0, 0);
    put(1, tree_v[b][2].x / (float)UNIT, tree_v[b][2].y / (float)UNIT - 0.1f, tree_v[b][2].z / (float)UNIT, 0, 0, 0);
    float ax, ay, bx, by;
    project(0, &ax, &ay, &r);
    project(1, &bx, &by, &r);
    float mx = (ax + bx) / 2, my = (ay + by) / 2;
    c &= ~1;                                    // triangles write coverage into bit 0
    CHECK((px(fb, mx, my) & ~1) == c, "billboard %u center %04x", b, px(fb, mx, my));
    CHECK((px(fb, ax + 2, ay - 2) & ~1) == c && (px(fb, bx - 2, by + 2) & ~1) == c, "billboard %u corners", b);
    CHECK(px(fb, ax - 3, my) == bg && px(fb, bx + 3, my) == bg, "billboard %u outside", b);
  }
}

// ---- fountain ----------------------------------------------------------------------------
typedef struct { float x, y, z, vx, vy, vz, life; } spark_t;
static spark_t sparks[NPART];
static u32 rng = 12345;
static float frand(void) { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 16777216.f; }

static void spawn(spark_t *s) {
  float a = frand() * 2 * K64_PI, v = 0.6f + frand() * 0.8f;
  *s = (spark_t){ 0, 0.2f, 0, k64_cosf(a) * v, 5.5f + frand() * 2.f, k64_sinf(a) * v, 2.2f + frand() * 1.2f };
}

// Tree: green cone over a brown trunk, alpha 0 around it.
static void build_tree(void) {
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 16; x++) {
      float dx = x - 7.5f; if (dx < 0) dx = -dx;
      u32 c = 0;
      if (y < 24 && dx < y * 0.33f + 1) c = k64_rgba16((u8)(30 + y * 2), (u8)(110 + y * 3), 40, 255);
      else if (y >= 24 && dx < 1.6f) c = k64_rgba16(110, 70, 30, 255);
      tree_px[y * 16 + x] = (u16)c;
    }
  k64_dcache_writeback(tree_px, sizeof tree_px);
}

static void build_atlas(void) {
  for (int y = 0; y < ATLAS; y++)
    for (int x = 0; x < ATLAS; x++) {
      int sp = (y >= 16) * 2 + (x >= 16);
      float dx = (x & 15) - 7.5f, dy = (y & 15) - 7.5f, d = k64_sqrtf(dx * dx + dy * dy);
      float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
      float v;
      switch (sp) {
        case 0:  v = d < 7.5f ? 1.f - d / 8.f : 0; break;                       // soft disc
        case 1:  v = (adx < 1.5f || ady < 1.5f) && d < 7.5f ? 1.f - d / 9.f : 0; break; // cross
        case 2:  v = d > 4.5f && d < 7.5f ? 1.f : 0; break;                     // ring
        default: v = adx + ady < 7.5f ? 1.f - (adx + ady) / 9.f : 0; break;     // diamond
      }
      u8 c = (u8)(v * 255);
      atlas_px[y * ATLAS + x] = (u16)k64_rgba16(c, c, c, v > 0.1f ? 255 : 0);
    }
  k64_dcache_writeback(atlas_px, sizeof atlas_px);
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  zbuf = k64_mem_alloc(K64_ZONE_Z, 320 * 240 * 2, 64);
  k64_viewport_set(&vp, 320, 240);
  k64_viewport_wnorm(&vp, 1.f, 100.f);
  k64_dcache_writeback(&vp, sizeof vp);

  const k64_surface_t *fb = k64_display_acquire();
  self_test(fb);
  k64_display_show(fb);
  k64_printf("[21_particles] self-test %s (%u failures)\n", fails ? "FAIL" : "PASS", fails);

  build_atlas();
  build_tree();
  k64_tex_t tree = { .width = 16, .height = 32, .fmt = K64_FMT_RGBA, .siz = K64_SIZ_16,
                     .line_bytes = 32, .tmem_line = 4, .pixels = tree_px };
  k64_tex_t atlas = { .width = ATLAS, .height = ATLAS, .fmt = K64_FMT_RGBA, .siz = K64_SIZ_16,
                      .line_bytes = ATLAS * 2, .tmem_line = ATLAS * 2 / 8, .pixels = atlas_px };
  for (int i = 0; i < 4; i++)
    floor_v[i] = (k64_vtx_t){ (s16)((i == 1 || i == 2 ? 6 : -6) * UNIT), 0, (s16)((i >= 2 ? -6 : 6) * UNIT), 0,
                              (u8)(i & 1 ? 40 : 70), 60, (u8)(i & 2 ? 110 : 80), 255, 0, 0 };
  k64_dcache_writeback(floor_v, sizeof floor_v);
  for (u32 i = 0; i < NPART; i++) { spawn(&sparks[i]); sparks[i].life *= frand(); }

  float t = 0;
  u32 rsp_us = 0, cpu_us = 0, rdp_clk = 0, rects = 0;
  for (u32 frame = 0;; frame++) {
    fb = k64_display_acquire();
    u32 t0 = k64_count(), c0 = k64_rdp_clock();
    float a = t * 0.25f;
    set_camera((k64_vec3_t){ 9.f * k64_cosf(a), 4.f, 9.f * k64_sinf(a) }, (k64_vec3_t){ 0, 2.5f, 0 });
    const float dt = 1.f / 30;
    for (u32 i = 0; i < NPART; i++) {
      spark_t *s = &sparks[i];
      s->life -= dt;
      if (s->life <= 0) spawn(s);
      s->vy -= 6.f * dt;
      s->x += s->vx * dt; s->y += s->vy * dt; s->z += s->vz * dt;
      if (s->y < 0) { s->y = 0; s->vy *= -0.4f; }
      float l = s->life > 1.f ? 1.f : s->life;
      u8 r = 255, g = (u8)(120 + 135 * l), b = (u8)(40 + 200 * l * l);
      put(i, s->x, s->y, s->z, 0.08f + 0.1f * l, (u32)r << 24 | (u32)g << 16 | (u32)b << 8 | 255, (int)(i & 3));
    }
    u32 t1 = k64_count();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, 320, 240, fb->pixels, zbuf);
    k64_rdp_clear(k64_rgba16(8, 8, 20, 255) * 0x10001u, true);
    k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF);
    k64_rdp_set_combine(K64_CC_SHADE);
    run(true, 0, 0);
    // trees: Y billboards through the triangle path, alpha compare cuts the shape
    k64_vec3_t eye = { 9.f * UNIT * k64_cosf(a), 4.f * UNIT, 9.f * UNIT * k64_sinf(a) };
    for (u32 i = 0; i < NTREE; i++) {
      float ta = i * (2 * K64_PI / NTREE) + 0.3f;
      k64_kgfx_billboard_y(tree_v[i], (k64_vec3_t){ 5.f * UNIT * k64_cosf(ta), 0, 5.f * UNIT * k64_sinf(ta) },
                           0.7f * UNIT, 2.6f * UNIT, eye, 0xFFFFFFFF, 0, 0, 16 * 32, 32 * 32);
    }
    k64_dcache_writeback(tree_v, sizeof tree_v);
    k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF | K64_OM_PERSP_TEX | K64_OM_ALPHA_COMPARE);
    k64_rdp_set_blend_color(0x00000080);
    k64_rdp_set_combine(K64_CC_TEX_SHADE);
    k64_rdp_load_tex(1, 0, &tree, K64_TEX_CLAMP, K64_TEX_CLAMP);
    {
      k64_kgfx_list_t l;
      k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
      k64_kgfx_viewport(&l, &vp);
      k64_kgfx_mtx(&l, &mtx);
      k64_kgfx_geommode(&l, K64_TRI_SHADE | K64_TRI_Z | K64_TRI_TEX, 1, K64_KGFX_CULL_BACK);
      for (u32 i = 0; i < NTREE; i += 4) {
        k64_kgfx_vtx(&l, tree_v[i], 16, 0);
        for (u32 q = 0; q < 16; q += 4) k64_kgfx_tri2(&l, q, q + 1, q + 2, q, q + 2, q + 3);
      }
      k64_kgfx_end(&l);
      k64_kgfx_run(&l);
      k64_kgfx_wait();
    }
    // sparks: texel * color, alpha compare cuts the sprite shapes, z tested but not written
    k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_Z_COMPARE | K64_OM_Z_SOURCE_PRIM | K64_OM_ALPHA_COMPARE);
    k64_rdp_set_blend_color(0x00000080);
    k64_rdp_set_combine(K64_CC_TEX_PRIM);
    k64_rdp_load_tex(0, 0, &atlas, K64_TEX_CLAMP, K64_TEX_CLAMP);
    u32 t2 = k64_count();
    run(false, NPART, K64_PART_TEX | K64_PART_Z | K64_PART_COLOR);
    rsp_us = K64_US(k64_count() - t2);
    cpu_us = K64_US(t1 - t0);
    rects = k64_kgfx_part_count();
    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  21 particles  %s", fails ? "FAIL" : "PASS");
    k64_text_printf(8, 208, 0xFFE040FF, "%u sparks %u rects  cpu %u us", NPART, rects, cpu_us);
    k64_text_printf(8, 220, 0xFFE040FF, "rsp %u us  rdp %u clk", rsp_us, rdp_clk);
    k64_rdp_wait_idle();
    rdp_clk = (k64_rdp_clock() - c0) & 0xFFFFFF;
    k64_display_show(fb);
    if ((frame & 63) == 63)
      k64_printf("[21_particles] frame %u: %u rects, cpu %u us, rsp %u us, rdp %u clk, scene %08X\n", frame, rects,
                 cpu_us, rsp_us, rdp_clk, scene_hash(fb));
    t += dt;
  }
}
