// Demo 08 — camera inside geometry: a walk through a textured, lit room with pillars. The
// near plane (0.1) cuts walls, floor and pillars every frame, so kgfx's RSP clipping runs
// under load; the far walls rely on the guard band.
//
// Every surface is a grid of 1x1 quads drawn row by row between the two vertex cache halves.
// A pauses the walk.
#include <k64/k64.h>

#define UNIT   256                         // model units per world unit
#define TEX    32                          // wall texture size (one per quad, wraps)
#define MAXROW 9                           // vertices per grid row
#define NGRID  (7 + 4 * 4)                 // floor, ceiling, 4 walls, platform; 4 pillars x 4 sides
#define ROOM   8                           // room is ROOM x ROOM, height 3
#define HEIGHT 3

typedef struct {
  u32 nu, nv;                              // quads
  k64_vtx_t *v;                            // (nv + 1) rows of (nu + 1)
  k64_vtx_t corner[4];                     // hull for the CULL test
} grid_t;
#define CORNER 28                          // cache slots of the corners (rows use 0.., 16..)

static k64_vtx_t vbuf[512] K64_ALIGN(16);
static grid_t grids[NGRID];
static u32 ngrids, nverts;
static u16 tex_px[TEX * TEX] K64_ALIGN(16);
static k64_mtxfx_t mtx K64_ALIGN(16);
static k64_kgfx_lights_t lights;
static k64_viewport_t vp K64_ALIGN(16);
static u64 list_buf[1024] K64_ALIGN(16);

// A grid from `o` along u (nu quads) and v (nv quads); faces the side of u x v.
static void add_grid(k64_vec3_t o, k64_vec3_t u, k64_vec3_t v, u32 nu, u32 nv, u8 r, u8 g, u8 b) {
  K64_ASSERT(nu + 1 <= MAXROW && ngrids < NGRID);
  k64_vec3_t n = { u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
  float len = k64_sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
  u16 pn = k64_kgfx_pack_normal(n.x / len, n.y / len, n.z / len);
  grid_t *gr = &grids[ngrids++];
  gr->nu = nu; gr->nv = nv; gr->v = &vbuf[nverts];
  for (u32 j = 0; j <= nv; j++)
    for (u32 i = 0; i <= nu; i++) {
      float x = o.x + u.x * i + v.x * j, y = o.y + u.y * i + v.y * j, z = o.z + u.z * i + v.z * j;
      vbuf[nverts++] = (k64_vtx_t){ (s16)(x * UNIT), (s16)(y * UNIT), (s16)(z * UNIT), pn, r, g, b, 255,
                                    (s16)(i * TEX * 32), (s16)(j * TEX * 32) };
    }
  const k64_vtx_t *gv = gr->v;
  u32 last = (nu + 1) * nv;
  gr->corner[0] = gv[0]; gr->corner[1] = gv[nu]; gr->corner[2] = gv[last]; gr->corner[3] = gv[last + nu];
  K64_ASSERT(nverts <= K64_ARRAY_LEN(vbuf));
}

static void build_room(void) {
  const float h = ROOM / 2.f;
  // floor (normal +y), ceiling (-y), walls facing in
  add_grid((k64_vec3_t){ -h, 0, -h }, (k64_vec3_t){ 0, 0, 1 }, (k64_vec3_t){ 1, 0, 0 }, ROOM, ROOM, 150, 140, 120);
  add_grid((k64_vec3_t){ -h, HEIGHT, -h }, (k64_vec3_t){ 1, 0, 0 }, (k64_vec3_t){ 0, 0, 1 }, ROOM, ROOM, 120, 130, 150);
  add_grid((k64_vec3_t){ -h, 0, -h }, (k64_vec3_t){ 1, 0, 0 }, (k64_vec3_t){ 0, 1, 0 }, ROOM, HEIGHT, 230, 200, 170);
  add_grid((k64_vec3_t){ h, 0, h }, (k64_vec3_t){ -1, 0, 0 }, (k64_vec3_t){ 0, 1, 0 }, ROOM, HEIGHT, 170, 210, 230);
  add_grid((k64_vec3_t){ -h, 0, h }, (k64_vec3_t){ 0, 0, -1 }, (k64_vec3_t){ 0, 1, 0 }, ROOM, HEIGHT, 210, 230, 170);
  add_grid((k64_vec3_t){ h, 0, -h }, (k64_vec3_t){ 0, 0, 1 }, (k64_vec3_t){ 0, 1, 0 }, ROOM, HEIGHT, 230, 170, 200);
  // a low platform in the middle (top only)
  add_grid((k64_vec3_t){ -1, 0.3f, -1 }, (k64_vec3_t){ 0, 0, 1 }, (k64_vec3_t){ 1, 0, 0 }, 2, 2, 200, 120, 90);
  // pillars at (+-2, +-2), 1 wide, facing out
  for (int p = 0; p < 4; p++) {
    float cx = (p & 1) ? 2.f : -2.f, cz = (p & 2) ? 2.f : -2.f;
    float x0 = cx - 0.5f, x1 = cx + 0.5f, z0 = cz - 0.5f, z1 = cz + 0.5f;
    add_grid((k64_vec3_t){ x1, 0, z0 }, (k64_vec3_t){ -1, 0, 0 }, (k64_vec3_t){ 0, 1, 0 }, 1, HEIGHT, 220, 220, 220);
    add_grid((k64_vec3_t){ x0, 0, z1 }, (k64_vec3_t){ 1, 0, 0 }, (k64_vec3_t){ 0, 1, 0 }, 1, HEIGHT, 220, 220, 220);
    add_grid((k64_vec3_t){ x0, 0, z0 }, (k64_vec3_t){ 0, 0, 1 }, (k64_vec3_t){ 0, 1, 0 }, 1, HEIGHT, 200, 200, 200);
    add_grid((k64_vec3_t){ x1, 0, z1 }, (k64_vec3_t){ 0, 0, -1 }, (k64_vec3_t){ 0, 1, 0 }, 1, HEIGHT, 200, 200, 200);
  }
}

// Stone tiles: mortar border, per-tile shading noise.
static void build_tex(void) {
  for (int y = 0; y < TEX; y++)
    for (int x = 0; x < TEX; x++) {
      bool mortar = (x & 15) == 0 || (y & 15) == 0;
      u32 hsh = (u32)((x >> 4) * 73 + (y >> 4) * 151 + x * 7 + y * 13) * 2654435761u;
      int v = mortar ? 90 : 190 + (int)((hsh >> 27) & 15) * 3 - 20;
      tex_px[y * TEX + x] = (u16)k64_rgba16((u8)v, (u8)v, (u8)v, 255);
    }
}

static void band(k64_kgfx_list_t *l, u32 prev, u32 cur, u32 n) {
  u32 row = (2u << n) - 1;                 // n + 1 slots
  u64 *c = k64_kgfx_cull(l, row << prev | row << cur);
  for (u32 i = 0; i < n; i++)
    k64_kgfx_tri2(l, prev + i, cur + i + 1, cur + i, prev + i, prev + i + 1, cur + i + 1);
  k64_kgfx_cull_end(l, c);
}

static void draw(const k64_mat4_t *viewproj) {
  k64_kgfx_list_t l;
  k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
  k64_kgfx_viewport(&l, &vp);
  k64_kgfx_geommode(&l, K64_TRI_SHADE | K64_TRI_Z | K64_TRI_TEX, 0, K64_KGFX_CULL_BACK | K64_KGFX_LIGHTING);
  k64_mat4_t sc, m;
  k64_mat4_scale(&sc, 1.f / UNIT, 1.f / UNIT, 1.f / UNIT);
  k64_mat4_mul(&m, viewproj, &sc);
  k64_mtx_to_fixed(&mtx, &m);
  k64_kgfx_mtx(&l, &mtx);
  k64_kgfx_lights(&l, &lights);
  for (u32 g = 0; g < ngrids; g++) {
    const grid_t *gr = &grids[g];
    u32 row = gr->nu + 1;
    k64_kgfx_vtx(&l, gr->corner, 4, CORNER);
    u64 *c = k64_kgfx_cull(&l, 0xFu << CORNER);
    k64_kgfx_vtx(&l, gr->v, row, 0);
    for (u32 j = 1; j <= gr->nv; j++) {
      u32 cur = (j & 1) * 16;
      k64_kgfx_vtx(&l, gr->v + j * row, row, cur);
      band(&l, 16 - cur, cur, gr->nu);
    }
    k64_kgfx_cull_end(&l, c);
  }
  k64_kgfx_end(&l);
  k64_dcache_writeback(&mtx, sizeof mtx);
  k64_kgfx_run(&l);
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  u16 *zbuf = k64_mem_alloc(K64_ZONE_Z, 320 * 240 * 2, 64);
  k64_viewport_set(&vp, 320, 240);
  build_room();
  build_tex();
  k64_kgfx_light_t lt[2] = {
    { { 0.3f, 1.f, 0.5f }, 230, 220, 190 },
    { { -0.7f, 0.2f, -0.6f }, 90, 100, 140 },
  };
  k64_kgfx_lights_set(&lights, lt, 2, 70, 70, 80, 0);
  k64_dcache_writeback(&vp, sizeof vp);
  k64_dcache_writeback(vbuf, sizeof vbuf);
  k64_dcache_writeback(grids, sizeof grids);
  k64_dcache_writeback(tex_px, sizeof tex_px);
  k64_dcache_writeback(&lights, sizeof lights);
  k64_tex_t tex = { .width = TEX, .height = TEX, .fmt = K64_FMT_RGBA, .siz = K64_SIZ_16,
                    .line_bytes = TEX * 2, .tmem_line = TEX * 2 / 8, .pixels = tex_px };

  k64_mat4_t proj;
  k64_mat4_perspective(&proj, 70.f * K64_PI / 180.f, 320.f / 240.f, 0.1f, 40.f);

  bool walk = true;
  float t = 0;
  u32 cpu_us = 0, rsp_us = 0, rdp_clk = 0, tris = 0;
  for (u32 frame = 0;; frame++) {
    k64_pad_poll_start();
    const k64_surface_t *fb = k64_display_acquire();
    u32 t0 = k64_count(), c0 = k64_rdp_clock();
    // walk a loop that brushes past the pillars and the walls
    float a = t * 0.35f, r = 2.6f + 0.9f * k64_sinf(t * 0.8f);
    k64_vec3_t eye = { r * k64_cosf(a), 1.2f + 0.4f * k64_sinf(t * 0.5f), r * k64_sinf(a) };
    float la = a + 1.2f + 0.6f * k64_sinf(t * 0.3f);
    k64_vec3_t at = { eye.x + k64_cosf(la), eye.y - 0.15f, eye.z + k64_sinf(la) };
    k64_mat4_t view, viewproj;
    k64_mat4_lookat(&view, eye, at, (k64_vec3_t){ 0, 1, 0 });
    k64_mat4_mul(&viewproj, &proj, &view);

    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, zbuf);
    k64_rdp_clear(k64_rgba16(0, 0, 0, 255) * 0x10001u, true);
    k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF | K64_OM_PERSP_TEX | K64_OM_SAMPLE_BILINEAR);
    k64_rdp_set_combine(K64_CC_TEX_SHADE);
    k64_rdp_load_tex(0, 0, &tex, K64_TEX_WRAP, K64_TEX_WRAP);

    draw(&viewproj);
    u32 t1 = k64_count();
    u32 st = k64_kgfx_wait();
    rsp_us = K64_US(k64_count() - t1);
    cpu_us = K64_US(t1 - t0);
    tris = k64_kgfx_tri_count();
    if (st) k64_printf("[08] kgfx status %x\n", st);

    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  08 room clip");
    k64_text_printf(8, 208, 0xFFE040FF, "tris %u  cpu %5u us  rsp %5u us", tris, cpu_us, rsp_us);
    k64_text_printf(8, 220, 0xFFE040FF, "rdp %6u clk   A: pause", rdp_clk);
    k64_pad_poll_end();
    k64_rdp_wait_idle();
    rdp_clk = (k64_rdp_clock() - c0) & 0xFFFFFF;
    k64_display_show(fb);
    if ((frame & 63) == 63)
      k64_printf("[08] frame %u tris %u cpu %u us rsp %u us rdp %u clk\n", frame, tris, cpu_us, rsp_us, rdp_clk);
    if (k64_pads[0].pressed & K64_PAD_A) walk = !walk;
    if (walk) t += 1.f / 30;
  }
}
