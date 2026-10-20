// Demo 13 — flat-shaded 3D geometry in FILL mode versus 1-cycle.
//
// A Star Fox style scene (floor + a grid of spinning cubes, one flat color per face, lit on
// the CPU) drawn through kgfx in four modes, switching every 128 frames:
//   Z1C   1-cycle + Z buffer, PRIM color per face, cubes front to back (Z rejects hidden pixels)
//   P1C   1-cycle without Z, PRIM color per face, painter's order (back to front)
//   FILL  FILL cycle without Z, SYNC_PIPE + FILL color per face, painter's order
//   CLR   clear only (baseline)
// The cubes are convex and do not intersect, so back-face culling + sorting the cubes by
// depth draws them correctly without Z. Per mode the log prints the RDP busy / wall clocks of
// the 3D part of the frame (clear included), averaged over 64 frames.
#include <k64/k64.h>

#define W 320
#define H 240
#define GX 6
#define GZ 5
#define NCUBES (GX * GZ)
#define UNIT 256                           // model units per world unit
#define FLOOR 24.f                         // floor half size (world units)

enum { M_Z1C, M_P1C, M_FILL, M_CLR, M_COUNT };
static const char *const mode_name[M_COUNT] = { "Z1C", "P1C", "FILL", "CLR" };

static const k64_vtx_t cube_v[8] K64_ALIGN(16) = {
  {-UNIT,-UNIT,-UNIT,0, 255,255,255,255, 0,0}, { UNIT,-UNIT,-UNIT,0, 255,255,255,255, 0,0},
  { UNIT, UNIT,-UNIT,0, 255,255,255,255, 0,0}, {-UNIT, UNIT,-UNIT,0, 255,255,255,255, 0,0},
  {-UNIT,-UNIT, UNIT,0, 255,255,255,255, 0,0}, { UNIT,-UNIT, UNIT,0, 255,255,255,255, 0,0},
  { UNIT, UNIT, UNIT,0, 255,255,255,255, 0,0}, {-UNIT, UNIT, UNIT,0, 255,255,255,255, 0,0},
};
// Two counter-clockwise triangles per face; faces -z, +z, -y, +y, -x, +x.
static const u8 cube_i[6][2][3] = {
  {{0,2,1},{0,3,2}}, {{4,5,6},{4,6,7}}, {{0,1,5},{0,5,4}}, {{3,6,2},{3,7,6}}, {{0,4,7},{0,7,3}}, {{1,2,6},{1,6,5}},
};
static const float face_n[6][3] = { {0,0,-1}, {0,0,1}, {0,-1,0}, {0,1,0}, {-1,0,0}, {1,0,0} };

#define FU ((s16)(FLOOR * UNIT / 4))       // floor model is scaled x4 by its matrix
static const k64_vtx_t floor_v[4] K64_ALIGN(16) = {
  {-FU,0,-FU,0, 255,255,255,255, 0,0}, { FU,0,-FU,0, 255,255,255,255, 0,0},
  { FU,0, FU,0, 255,255,255,255, 0,0}, {-FU,0, FU,0, 255,255,255,255, 0,0},
};

static k64_mtxfx_t mtx[NCUBES + 1] K64_ALIGN(16);
static k64_viewport_t vp K64_ALIGN(16);
static u64 rdp_cmds[NCUBES * 6 * 2 + 8] K64_ALIGN(16);
static u64 list_buf[16 + NCUBES * 16] K64_ALIGN(16);

typedef struct { k64_mat4_t model; float depth; u8 r, g, b; } cube_t;
static cube_t cubes[NCUBES];
static u8 order[NCUBES];

static k64_vec3_t light = { -0.45f, 0.80f, 0.40f };   // towards the light, normalized below

static void xform_dir(float out[3], const k64_mat4_t *m, const float v[3]) {
  for (int r = 0; r < 3; r++) out[r] = m->m[0][r] * v[0] + m->m[1][r] * v[1] + m->m[2][r] * v[2];
}
static void xform_pt(float out[3], const k64_mat4_t *m, const float v[3]) {
  for (int r = 0; r < 3; r++) out[r] = m->m[0][r] * v[0] + m->m[1][r] * v[1] + m->m[2][r] * v[2] + m->m[3][r];
}

static u32 face_color(const cube_t *c, const float n_world[3], u32 mode) {
  float d = n_world[0] * light.x + n_world[1] * light.y + n_world[2] * light.z;
  float k = 0.30f + (d > 0 ? d * 0.75f : 0);
  u32 r = (u32)(c->r * k), g = (u32)(c->g * k), b = (u32)(c->b * k);
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  if (mode == M_FILL) return k64_rgba16(r, g, b, 255) * 0x10001u;
  return (r << 24) | (g << 16) | (b << 8) | 255;
}

// Appends the color change for a face (or the floor) to the raw RDP command buffer and to the
// list. Same color as the last one: nothing (no pipe sync in FILL mode).
static u64 *rc;
static u32 last_color, color_changes;
static void set_color(k64_kgfx_list_t *l, u32 c, u32 mode) {
  if (c == last_color) return;
  last_color = c;
  color_changes++;
  u64 *p = rc;
  if (mode == M_FILL) { *rc++ = k64_rdpc_sync_pipe(); *rc++ = k64_rdpc_fill_color(c); }
  else *rc++ = k64_rdpc_prim_color(c, 0, 0);
  k64_kgfx_rdp(l, p, (u32)(rc - p) * 8);
}

static void draw_floor(k64_kgfx_list_t *l, const k64_mat4_t *viewproj, u32 mode) {
  k64_mat4_t sc, m;
  k64_mat4_scale(&sc, 4.f / UNIT, 4.f / UNIT, 4.f / UNIT);
  k64_mat4_mul(&m, viewproj, &sc);
  k64_mtx_to_fixed(&mtx[NCUBES], &m);
  k64_kgfx_mtx(l, &mtx[NCUBES]);
  k64_kgfx_vtx(l, floor_v, 4, 0);
  set_color(l, mode == M_FILL ? k64_rgba16(40, 90, 50, 255) * 0x10001u : 0x285A32FF, mode);
  k64_kgfx_tri2(l, 0, 2, 1, 0, 3, 2);
}

static void draw_cube(k64_kgfx_list_t *l, const k64_mat4_t *viewproj, const k64_mat4_t *view, u32 i, u32 mode) {
  const cube_t *c = &cubes[i];
  k64_mat4_t mvp, sc, m, mv;
  k64_mat4_scale(&sc, 1.f / UNIT, 1.f / UNIT, 1.f / UNIT);
  k64_mat4_mul(&mvp, viewproj, &c->model);
  k64_mat4_mul(&m, &mvp, &sc);
  k64_mtx_to_fixed(&mtx[i], &m);
  k64_kgfx_mtx(l, &mtx[i]);
  k64_kgfx_vtx(l, cube_v, 8, 0);
  // Faces the camera can see (exact back-face test in view space), so that hidden faces
  // cost no color change; the RSP still back-face culls as usual.
  k64_mat4_mul(&mv, view, &c->model);
  for (int f = 0; f < 6; f++) {
    float nv[3], fc[3], nw[3];
    xform_dir(nv, &mv, face_n[f]);
    xform_pt(fc, &mv, face_n[f]);         // face center = normal * half size (1)
    if (nv[0] * fc[0] + nv[1] * fc[1] + nv[2] * fc[2] >= 0) continue;
    xform_dir(nw, &c->model, face_n[f]);
    set_color(l, face_color(c, nw, mode), mode);
    k64_kgfx_tri2(l, cube_i[f][0][0], cube_i[f][0][1], cube_i[f][0][2],
                     cube_i[f][1][0], cube_i[f][1][1], cube_i[f][1][2]);
  }
}

static void scene_update(const k64_mat4_t *view, float t) {
  for (u32 i = 0; i < NCUBES; i++) {
    cube_t *c = &cubes[i];
    float x = ((float)(i % GX) - (GX - 1) * 0.5f) * 3.2f, z = -(float)(i / GX) * 3.6f;
    k64_mat4_t ry, tr;
    k64_mat4_rotate_y(&ry, t * (0.6f + 0.11f * (float)(i % 7)) + (float)i);
    k64_mat4_translate(&tr, x, 1.f, z);
    k64_mat4_mul(&c->model, &tr, &ry);
    float p[3] = { 0, 0, 0 }, v[3];
    xform_pt(v, &c->model, p);
    float pv[3];
    xform_pt(pv, view, v);
    c->depth = -pv[2];                    // distance along the view axis
  }
  // insertion sort, nearest first
  for (u32 i = 0; i < NCUBES; i++) order[i] = (u8)i;
  for (u32 i = 1; i < NCUBES; i++) {
    u8 o = order[i]; u32 j = i;
    while (j && cubes[order[j - 1]].depth > cubes[o].depth) { order[j] = order[j - 1]; j--; }
    order[j] = o;
  }
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = W, .height = H, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  u16 *zbuf = k64_mem_alloc(K64_ZONE_Z, W * H * 2, 64);
  k64_viewport_set(&vp, W, H);
  k64_dcache_writeback(&vp, sizeof vp);

  static const u8 pal[6][3] = { {230,70,60}, {70,140,230}, {240,200,60}, {200,90,220}, {80,210,200}, {235,235,235} };
  for (u32 i = 0; i < NCUBES; i++) { cubes[i].r = pal[i % 6][0]; cubes[i].g = pal[i % 6][1]; cubes[i].b = pal[i % 6][2]; }
  float ll = 1.f / k64_sqrtf(light.x * light.x + light.y * light.y + light.z * light.z);
  light = (k64_vec3_t){ light.x * ll, light.y * ll, light.z * ll };

  k64_mat4_t proj, view, viewproj;
  k64_mat4_perspective(&proj, 60.f * K64_PI / 180.f, (float)W / H, 1.f, 100.f);
  k64_mat4_lookat(&view, (k64_vec3_t){ 0, 5.f, 8.f }, (k64_vec3_t){ 0, 0, -6.f }, (k64_vec3_t){ 0, 1, 0 });
  k64_mat4_mul(&viewproj, &proj, &view);

  u32 acc_busy = 0, acc_wall = 0, acc_rsp = 0, acc_n = 0, tris = 0, changes = 0;
  u32 show_busy = 0, show_wall = 0;
  for (u32 frame = 0;; frame++) {
    u32 mode = (frame >> 7) % M_COUNT;
    k64_pad_poll_start();
    const k64_surface_t *fb = k64_display_acquire();
    scene_update(&view, (float)frame * 0.02f);

    k64_rdp_wait_idle();
    u32 c0 = k64_rdp_clock(), p0 = DPC_PIPEBUSY & 0xFFFFFF;
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, W, H, fb->pixels, zbuf);
    k64_rdp_clear(k64_rgba16(90, 140, 210, 255) * 0x10001u, mode == M_Z1C);
    u32 rsp_us = 0;
    if (mode != M_CLR) {
      if (mode == M_FILL) k64_rdp_set_other_modes(K64_OM_FILL);
      else {
        k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | (mode == M_Z1C ? K64_OM_ZBUF : 0));
        k64_rdp_set_combine(K64_CC_PRIM);
      }
      k64_kgfx_list_t l;
      k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
      k64_kgfx_viewport(&l, &vp);
      k64_kgfx_geommode(&l, mode == M_Z1C ? K64_TRI_Z : 0, 0, K64_KGFX_CULL_BACK);
      rc = rdp_cmds; last_color = 0; color_changes = 0;
      if (mode == M_Z1C) {                 // front to back, floor last
        for (u32 k = 0; k < NCUBES; k++) draw_cube(&l, &viewproj, &view, order[k], mode);
        draw_floor(&l, &viewproj, mode);
      } else {                             // painter: floor, then back to front
        draw_floor(&l, &viewproj, mode);
        for (u32 k = NCUBES; k--;) draw_cube(&l, &viewproj, &view, order[k], mode);
      }
      k64_kgfx_end(&l);
      k64_dcache_writeback(mtx, sizeof mtx);
      k64_dcache_writeback(rdp_cmds, (u32)(rc - rdp_cmds) * 8);
      u32 t1 = k64_count();
      k64_kgfx_run(&l);
      u32 st = k64_kgfx_wait();
      rsp_us = K64_US(k64_count() - t1);
      tris = k64_kgfx_tri_count();
      changes = color_changes;
      // The raw color commands are invisible to the CPU's RDP state tracking (rdp.h).
      k64_rdps.fill_color = k64_rdps.prim_color = 0xDEADBEEF;
      if (st) k64_printf("[13] kgfx status %x\n", st);
    } else {
      tris = changes = 0;
    }
    k64_rdp_wait_idle();
    u32 wall = (k64_rdp_clock() - c0) & 0xFFFFFF, busy = ((DPC_PIPEBUSY & 0xFFFFFF) - p0) & 0xFFFFFF;
    if ((frame & 127) >= 64) { acc_busy += busy; acc_wall += wall; acc_rsp += rsp_us; acc_n++; }

    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  13 fill geometry  %s", mode_name[mode]);
    k64_text_printf(8, 208, 0xFFE040FF, "tris %u  colors %u  rsp %u us", tris, changes, rsp_us);
    k64_text_printf(8, 220, 0xFFE040FF, "rdp busy %6u  wall %6u clk", show_busy, show_wall);
    show_busy = busy; show_wall = wall;
    k64_pad_poll_end();
    k64_rdp_wait_idle();
    k64_display_show(fb);
    if ((frame & 127) == 127) {
      k64_printf("[13] %s tris %u colors %u busy %u wall %u rsp %u us (avg %u frames)\n", mode_name[mode],
                 tris, changes, acc_busy / acc_n, acc_wall / acc_n, acc_rsp / acc_n, acc_n);
      acc_busy = acc_wall = acc_rsp = acc_n = 0;
    }
  }
}
