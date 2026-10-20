// Demo 06 — kgfx lighting: a smooth sphere and flat-shaded cubes lit on the RSP by a warm key
// light, a blue fill light and ambient. Normals are packed into the vertex (5.6.5); per
// object the CPU only moves the world-space lights into model space (k64_kgfx_lights_set).
//
// The sphere is drawn band by band with its rings alternating between two halves of the
// vertex cache, so each vertex is transformed and lit once.
//
// A toggles lighting (off = the vertex colors as they are); it also flips by itself every
// 256 frames so a headless run logs the RSP cost of both.
#include <k64/k64.h>

#define UNIT     256                      // model units per world unit
#define SEGS     15                       // sphere: segments around (ring = SEGS + 1 vertices)
#define RINGS    11                       // rings from pole to pole
#define NCUBES   4
#define NOBJ     (1 + NCUBES)

static k64_vtx_t sphere_v[RINGS][SEGS + 1] K64_ALIGN(16);
static k64_vtx_t cube_v[24] K64_ALIGN(16);            // 4 per face, face normals
static k64_mtxfx_t mtx[NOBJ] K64_ALIGN(16);
static k64_kgfx_lights_t lights[NOBJ];
static k64_viewport_t vp K64_ALIGN(16);
static u64 list_buf[256] K64_ALIGN(16);

static void build_sphere(void) {
  for (int r = 0; r < RINGS; r++) {
    float th = K64_PI * r / (RINGS - 1), y = k64_cosf(th), s = k64_sinf(th);
    for (int i = 0; i <= SEGS; i++) {
      float ph = 2 * K64_PI * i / SEGS, x = s * k64_cosf(ph), z = s * k64_sinf(ph);
      // a soft band pattern so lighting and color are both visible
      u8 g = (u8)(170 + 60 * ((r + i) & 1));
      sphere_v[r][i] = (k64_vtx_t){ (s16)(x * UNIT), (s16)(y * UNIT), (s16)(z * UNIT),
                                    k64_kgfx_pack_normal(x, y, z), 230, g, 200, 255, 0, 0 };
    }
  }
}

static void build_cube(void) {
  // corners as in demo 05; faces counter-clockwise from outside, with their normals
  static const s8 corner[8][3] = { {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1}, {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1} };
  static const u8 face[6][4] = { {0,3,2,1}, {4,5,6,7}, {0,1,5,4}, {3,7,6,2}, {0,4,7,3}, {1,2,6,5} };
  static const s8 n[6][3] = { {0,0,-1}, {0,0,1}, {0,-1,0}, {0,1,0}, {-1,0,0}, {1,0,0} };
  static const u8 col[6][3] = { {230,60,60}, {60,200,80}, {70,110,240}, {240,200,60}, {220,220,220}, {200,80,220} };
  for (int f = 0; f < 6; f++)
    for (int c = 0; c < 4; c++) {
      const s8 *p = corner[face[f][c]];
      cube_v[f * 4 + c] = (k64_vtx_t){ (s16)(p[0] * UNIT / 2), (s16)(p[1] * UNIT / 2), (s16)(p[2] * UNIT / 2),
                                       k64_kgfx_pack_normal(n[f][0], n[f][1], n[f][2]),
                                       col[f][0], col[f][1], col[f][2], 255, 0, 0 };
    }
}

static void object_model(k64_mat4_t *model, u32 o, float ang) {
  k64_mat4_t ry, rx, tr, tmp, sc;
  if (o == 0) {
    k64_mat4_rotate_y(&ry, ang * 0.6f);
    k64_mat4_rotate_x(&rx, 0.3f);
    k64_mat4_mul(&tmp, &rx, &ry);
    k64_mat4_scale(&sc, 1.7f, 1.7f, 1.7f);
    k64_mat4_translate(&tr, 0, 0, -1.f);
    k64_mat4_mul(model, &tmp, &sc);
    k64_mat4_mul(&tmp, &tr, model);
    *model = tmp;
    return;
  }
  float a = ang * 0.8f + (o - 1) * (K64_PI / 2);
  k64_mat4_rotate_y(&ry, ang * (1.1f + 0.2f * o));
  k64_mat4_rotate_x(&rx, ang * 0.7f + o);
  k64_mat4_mul(&tmp, &ry, &rx);
  k64_mat4_translate(&tr, 3.3f * k64_cosf(a), 0.6f * k64_sinf(a * 2), -1.f + 2.2f * k64_sinf(a));
  k64_mat4_mul(model, &tr, &tmp);
}

static void draw(const k64_mat4_t *viewproj, float ang, bool lit) {
  // world-space lights, slowly circling
  k64_kgfx_light_t lt[2] = {
    { { -0.6f * k64_cosf(ang * 0.3f), 0.7f, 0.6f * k64_sinf(ang * 0.3f) + 0.4f }, 255, 230, 190 },
    { { 0.9f, -0.2f, -0.3f }, 40, 70, 170 },
  };
  k64_kgfx_list_t l;
  k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
  k64_kgfx_viewport(&l, &vp);
  k64_kgfx_geommode(&l, K64_TRI_SHADE | K64_TRI_Z, 0, K64_KGFX_CULL_BACK | (lit ? K64_KGFX_LIGHTING : 0));
  for (u32 o = 0; o < NOBJ; o++) {
    k64_mat4_t model, mvp, sc, m;
    object_model(&model, o, ang);
    k64_mat4_scale(&sc, 1.f / UNIT, 1.f / UNIT, 1.f / UNIT);
    k64_mat4_mul(&mvp, viewproj, &model);
    k64_mat4_mul(&m, &mvp, &sc);
    k64_mtx_to_fixed(&mtx[o], &m);
    k64_kgfx_mtx(&l, &mtx[o]);
    if (lit) {
      k64_kgfx_lights_set(&lights[o], lt, 2, 36, 36, 48, &model);
      k64_kgfx_lights(&l, &lights[o]);
    }
    if (o == 0) {
      // ring r lives in cache half (r & 1): each band draws between the two halves
      k64_kgfx_vtx(&l, sphere_v[0], SEGS + 1, 0);
      for (int r = 1; r < RINGS; r++) {
        u32 cur = (u32)(r & 1) * 16, prev = 16 - cur;
        k64_kgfx_vtx(&l, sphere_v[r], SEGS + 1, cur);
        for (u32 i = 0; i < SEGS; i++)       // counter-clockwise from outside
          k64_kgfx_tri2(&l, prev + i, cur + i + 1, cur + i, prev + i, prev + i + 1, cur + i + 1);
      }
    } else {
      k64_kgfx_vtx(&l, cube_v, 24, 0);
      for (u32 f = 0; f < 6; f++)
        k64_kgfx_tri2(&l, f * 4, f * 4 + 1, f * 4 + 2, f * 4, f * 4 + 2, f * 4 + 3);
    }
  }
  k64_kgfx_end(&l);
  k64_dcache_writeback(mtx, sizeof mtx);
  k64_dcache_writeback(lights, sizeof lights);
  k64_kgfx_run(&l);
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  u16 *zbuf = k64_mem_alloc(K64_ZONE_Z, 320 * 240 * 2, 64);
  k64_viewport_set(&vp, 320, 240);
  build_sphere();
  build_cube();
  k64_dcache_writeback(&vp, sizeof vp);
  k64_dcache_writeback(sphere_v, sizeof sphere_v);
  k64_dcache_writeback(cube_v, sizeof cube_v);

  k64_mat4_t proj, view, viewproj;
  k64_mat4_perspective(&proj, 60.f * K64_PI / 180.f, 320.f / 240.f, 1.f, 100.f);
  k64_mat4_lookat(&view, (k64_vec3_t){ 0, 2.f, 7.f }, (k64_vec3_t){ 0, 0, -1.f }, (k64_vec3_t){ 0, 1, 0 });
  k64_mat4_mul(&viewproj, &proj, &view);

  bool lit = true;
  u32 cpu_us = 0, rsp_us = 0, rdp_clk = 0, tris = 0;
  for (u32 frame = 0;; frame++) {
    k64_pad_poll_start();
    const k64_surface_t *fb = k64_display_acquire();
    u32 t0 = k64_count(), c0 = k64_rdp_clock();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, zbuf);
    k64_rdp_clear(k64_rgba16(10, 12, 24, 255) * 0x10001u, true);
    k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF);
    k64_rdp_set_combine(K64_CC_SHADE);

    draw(&viewproj, frame * 0.02f, lit);
    u32 t1 = k64_count();
    u32 st = k64_kgfx_wait();
    rsp_us = K64_US(k64_count() - t1);
    cpu_us = K64_US(t1 - t0);
    tris = k64_kgfx_tri_count();
    if (st) k64_printf("[06] kgfx status %x\n", st);

    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  06 lighting  %s", lit ? "on" : "off");
    k64_text_printf(8, 208, 0xFFE040FF, "tris %u  cpu %5u us  rsp %5u us", tris, cpu_us, rsp_us);
    k64_text_printf(8, 220, 0xFFE040FF, "rdp %6u clk   A: toggle light", rdp_clk);
    k64_pad_poll_end();
    k64_rdp_wait_idle();
    rdp_clk = (k64_rdp_clock() - c0) & 0xFFFFFF;
    k64_display_show(fb);
    if ((frame & 63) == 63)
      k64_printf("[06] frame %u light %s tris %u cpu %u us rsp %u us rdp %u clk\n", frame,
                 lit ? "on" : "off", tris, cpu_us, rsp_us, rdp_clk);
    if ((k64_pads[0].pressed & K64_PAD_A) || (frame & 255) == 255) lit = !lit;
  }
}
