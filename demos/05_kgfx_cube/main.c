// Demo 05 — the demo 03 cubes, now through the kgfx RSP microcode: the CPU only builds one
// matrix per cube and a short command list; vertex transform, culling and triangle setup run
// on the RSP, which writes the RDP commands straight into the RDP ring.
//
// A toggles the CPU path (float transform + k64_rdp_tri, as demo 03) for comparison; the
// demo also switches by itself every 256 frames so a headless run logs both.
#include <k64/k64.h>

#define GRID_X 5
#define GRID_Y 3
#define NCUBES (GRID_X * GRID_Y)
#define UNIT   256                         // model units per world unit (matrix scales by 1/UNIT)

static const k64_vtx_t cube_v[8] K64_ALIGN(16) = {
  {-UNIT,-UNIT,-UNIT,0, 255, 40, 40,255, 0,0}, { UNIT,-UNIT,-UNIT,0,  40,255, 40,255, 0,0},
  { UNIT, UNIT,-UNIT,0,  40, 40,255,255, 0,0}, {-UNIT, UNIT,-UNIT,0, 255,255, 40,255, 0,0},
  {-UNIT,-UNIT, UNIT,0, 255, 40,255,255, 0,0}, { UNIT,-UNIT, UNIT,0,  40,255,255,255, 0,0},
  { UNIT, UNIT, UNIT,0, 255,255,255,255, 0,0}, {-UNIT, UNIT, UNIT,0,  60, 60, 60,255, 0,0},
};
// counter-clockwise when seen from outside
static const u8 cube_i[12][3] = {
  {0,2,1},{0,3,2}, {4,5,6},{4,6,7}, {0,1,5},{0,5,4}, {3,6,2},{3,7,6}, {0,4,7},{0,7,3}, {1,2,6},{1,6,5},
};

static k64_mtxfx_t mtx[NCUBES] K64_ALIGN(16);
static k64_viewport_t vp K64_ALIGN(16);
static u64 list_buf[4 + NCUBES * 8] K64_ALIGN(16);

static void cube_mvp(k64_mat4_t *mvp, const k64_mat4_t *viewproj, u32 i, float ang) {
  k64_mat4_t ry, rx, tr, model, tmp;
  float fx = (float)(i % GRID_X) - (GRID_X - 1) * 0.5f, fy = (float)(i / GRID_X) - (GRID_Y - 1) * 0.5f;
  k64_mat4_rotate_y(&ry, ang * (1.f + 0.13f * i));
  k64_mat4_rotate_x(&rx, ang * 0.7f + i);
  k64_mat4_translate(&tr, fx * 2.4f, fy * 2.4f, -2.f);
  k64_mat4_mul(&tmp, &ry, &rx);
  k64_mat4_mul(&model, &tr, &tmp);
  k64_mat4_mul(mvp, viewproj, &model);
}

// RSP path: one MTX + VTX + 6 TRI2 per cube.
static void draw_rsp(const k64_mat4_t *viewproj, float ang) {
  k64_kgfx_list_t l;
  k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
  k64_kgfx_viewport(&l, &vp);
  k64_kgfx_geommode(&l, K64_TRI_SHADE | K64_TRI_Z, 0, K64_KGFX_CULL_BACK);
  for (u32 i = 0; i < NCUBES; i++) {
    k64_mat4_t mvp, sc, m;
    k64_mat4_scale(&sc, 1.f / UNIT, 1.f / UNIT, 1.f / UNIT);
    cube_mvp(&mvp, viewproj, i, ang);
    k64_mat4_mul(&m, &mvp, &sc);
    k64_mtx_to_fixed(&mtx[i], &m);
    k64_kgfx_mtx(&l, &mtx[i]);
    k64_kgfx_vtx(&l, cube_v, 8, 0);
    for (int t = 0; t < 12; t += 2)
      k64_kgfx_tri2(&l, cube_i[t][0], cube_i[t][1], cube_i[t][2], cube_i[t + 1][0], cube_i[t + 1][1], cube_i[t + 1][2]);
  }
  k64_kgfx_end(&l);
  k64_dcache_writeback(mtx, sizeof mtx);
  k64_kgfx_run(&l);
}

// CPU path: demo 03's float transform + k64_rdp_tri.
static int draw_cpu(const k64_mat4_t *viewproj, float ang, float sw, float sh) {
  int drawn = 0;
  for (u32 i = 0; i < NCUBES; i++) {
    k64_mat4_t mvp;
    cube_mvp(&mvp, viewproj, i, ang);
    k64_svtx_t sv[8];
    for (int k = 0; k < 8; k++) {
      const k64_vtx_t *v = &cube_v[k];
      float x = (float)v->x / UNIT, y = (float)v->y / UNIT, z = (float)v->z / UNIT;
      const float (*m)[4] = mvp.m;
      float cx = m[0][0]*x + m[1][0]*y + m[2][0]*z + m[3][0];
      float cy = m[0][1]*x + m[1][1]*y + m[2][1]*z + m[3][1];
      float cz = m[0][2]*x + m[1][2]*y + m[2][2]*z + m[3][2];
      float cw = m[0][3]*x + m[1][3]*y + m[2][3]*z + m[3][3];
      float iw = 1.f / cw;
      sv[k] = (k64_svtx_t){ .x = (cx * iw * 0.5f + 0.5f) * sw, .y = (0.5f - cy * iw * 0.5f) * sh,
                            .z = cz * iw * 0.5f + 0.5f, .inv_w = iw, .r = v->r, .g = v->g, .b = v->b, .a = 255 };
    }
    for (int t = 0; t < 12; t++) {
      const k64_svtx_t *a = &sv[cube_i[t][0]], *b = &sv[cube_i[t][1]], *c = &sv[cube_i[t][2]];
      float cross = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
      if (cross >= 0) continue;
      k64_rdp_tri(K64_TRI_SHADE | K64_TRI_Z, 0, a, b, c);
      drawn++;
    }
  }
  return drawn;
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  u16 *zbuf = k64_mem_alloc(K64_ZONE_Z, 320 * 240 * 2, 64);
  k64_viewport_set(&vp, 320, 240);
  k64_dcache_writeback(&vp, sizeof vp);

  k64_mat4_t proj, view, viewproj;
  k64_mat4_perspective(&proj, 60.f * K64_PI / 180.f, 320.f / 240.f, 1.f, 100.f);
  k64_mat4_lookat(&view, (k64_vec3_t){ 0, 1.5f, 9.f }, (k64_vec3_t){ 0, 0, -2.f }, (k64_vec3_t){ 0, 1, 0 });
  k64_mat4_mul(&viewproj, &proj, &view);

  bool use_rsp = true;
  u32 cpu_us = 0, rsp_us = 0, rdp_clk = 0, tris = 0;
  for (u32 frame = 0;; frame++) {
    k64_pad_poll_start();
    const k64_surface_t *fb = k64_display_acquire();
    u32 t0 = k64_count(), c0 = k64_rdp_clock();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, zbuf);
    k64_rdp_clear(k64_rgba16(16, 20, 40, 255) * 0x10001u, true);
    k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF);
    k64_rdp_set_combine(K64_CC_SHADE);

    float ang = frame * 0.02f;
    if (use_rsp) {
      draw_rsp(&viewproj, ang);
      u32 t1 = k64_count();
      u32 st = k64_kgfx_wait();
      rsp_us = K64_US(k64_count() - t1);
      cpu_us = K64_US(t1 - t0);
      tris = k64_kgfx_tri_count();
      if (st) k64_printf("[05] kgfx status %x\n", st);
    } else {
      tris = (u32)draw_cpu(&viewproj, ang, fb->width, fb->height);
      cpu_us = K64_US(k64_count() - t0);
      rsp_us = 0;
    }

    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  05 cubes  %s setup", use_rsp ? "RSP" : "CPU");
    k64_text_printf(8, 208, 0xFFE040FF, "tris %u  cpu %5u us  rsp %5u us", tris, cpu_us, rsp_us);
    k64_text_printf(8, 220, 0xFFE040FF, "rdp %6u clk   A: toggle path", rdp_clk);
    k64_pad_poll_end();
    k64_rdp_wait_idle();
    rdp_clk = (k64_rdp_clock() - c0) & 0xFFFFFF;
    k64_display_show(fb);
    if ((frame & 63) == 63)
      k64_printf("[05] frame %u %s tris %u cpu %u us rsp %u us rdp %u clk\n", frame,
                 use_rsp ? "rsp" : "cpu", tris, cpu_us, rsp_us, rdp_clk);
    if ((k64_pads[0].pressed & K64_PAD_A) || (frame & 255) == 255) use_rsp = !use_rsp;
  }
}
