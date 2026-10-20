// Demo 03 — first 3D: Gouraud-shaded, Z-buffered cubes. Transform + triangle setup run on
// the CPU (k64_rdp_tri, the float reference path); the RDP rasterizes with Z compare/update.
// Next step moves vertex transform + setup to the kgfx RSP microcode.
#include <k64/k64.h>

typedef struct { float x, y, z; u8 r, g, b; } vert_t;
static const vert_t cube_v[8] = {
  {-1,-1,-1, 255, 40, 40}, { 1,-1,-1,  40,255, 40}, { 1, 1,-1,  40, 40,255}, {-1, 1,-1, 255,255, 40},
  {-1,-1, 1, 255, 40,255}, { 1,-1, 1,  40,255,255}, { 1, 1, 1, 255,255,255}, {-1, 1, 1,  60, 60, 60},
};
// counter-clockwise when seen from outside
static const u8 cube_i[12][3] = {
  {0,2,1},{0,3,2}, {4,5,6},{4,6,7}, {0,1,5},{0,5,4}, {3,6,2},{3,7,6}, {0,4,7},{0,7,3}, {1,2,6},{1,6,5},
};

static int draw_mesh(const k64_mat4_t *mvp, float sw, float sh) {
  k64_svtx_t sv[8];
  for (int i = 0; i < 8; i++) {
    const vert_t *v = &cube_v[i];
    const float (*m)[4] = mvp->m;
    float cx = m[0][0]*v->x + m[1][0]*v->y + m[2][0]*v->z + m[3][0];
    float cy = m[0][1]*v->x + m[1][1]*v->y + m[2][1]*v->z + m[3][1];
    float cz = m[0][2]*v->x + m[1][2]*v->y + m[2][2]*v->z + m[3][2];
    float cw = m[0][3]*v->x + m[1][3]*v->y + m[2][3]*v->z + m[3][3];
    float iw = 1.f / cw;
    sv[i] = (k64_svtx_t){ .x = (cx * iw * 0.5f + 0.5f) * sw, .y = (0.5f - cy * iw * 0.5f) * sh,
                          .z = cz * iw * 0.5f + 0.5f, .inv_w = iw,
                          .r = v->r, .g = v->g, .b = v->b, .a = 255 };
  }
  int drawn = 0;
  for (int t = 0; t < 12; t++) {
    const k64_svtx_t *a = &sv[cube_i[t][0]], *b = &sv[cube_i[t][1]], *c = &sv[cube_i[t][2]];
    float cross = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (cross >= 0) continue;                    // back-face (screen y points down)
    k64_rdp_tri(K64_TRI_SHADE | K64_TRI_Z, 0, a, b, c);
    drawn++;
  }
  return drawn;
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  u16 *zbuf = k64_mem_alloc(K64_ZONE_Z, 320 * 240 * 2, 64);

  k64_mat4_t proj, view, vp;
  k64_mat4_perspective(&proj, 60.f * K64_PI / 180.f, 320.f / 240.f, 1.f, 100.f);
  k64_mat4_lookat(&view, (k64_vec3_t){ 0, 2.5f, 7.f }, (k64_vec3_t){ 0, 0, 0 }, (k64_vec3_t){ 0, 1, 0 });
  k64_mat4_mul(&vp, &proj, &view);

  u32 cpu_us = 0, rdp_clk = 0;
  for (u32 frame = 0;; frame++) {
    u32 t0 = k64_count();
    k64_pad_poll_start();
    const k64_surface_t *fb = k64_display_acquire();
    u32 c0 = k64_rdp_clock();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, zbuf);
    k64_rdp_clear(k64_rgba16(16, 20, 40, 255) * 0x10001u, true);

    k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF);
    k64_rdp_set_combine(K64_CC_SHADE);
    int tris = 0;
    float ang = frame * 0.02f;
    for (int i = 0; i < 3; i++) {
      k64_mat4_t ry, rx, tr, model, tmp, mvp;
      k64_mat4_rotate_y(&ry, ang * (1.f + 0.3f * i));
      k64_mat4_rotate_x(&rx, ang * 0.7f + i);
      k64_mat4_translate(&tr, (i - 1) * 2.6f, 0, -(float)(i & 1) * 1.5f);
      k64_mat4_mul(&tmp, &ry, &rx);
      k64_mat4_mul(&model, &tr, &tmp);
      k64_mat4_mul(&mvp, &vp, &model);
      tris += draw_mesh(&mvp, fb->width, fb->height);
    }

    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  03 cube (CPU setup)");
    k64_text_printf(8, 220, 0xFFE040FF, "tris %d  cpu %4u us  rdp %6u clk", tris, cpu_us, rdp_clk);
    cpu_us = K64_US(k64_count() - t0);
    k64_pad_poll_end();
    k64_rdp_wait_idle();
    rdp_clk = (k64_rdp_clock() - c0) & 0xFFFFFF;
    k64_display_show(fb);
    if ((frame & 63) == 0) k64_printf("[03] frame %u tris %d cpu %u us rdp %u clk\n", frame, tris, cpu_us, rdp_clk);
  }
}
