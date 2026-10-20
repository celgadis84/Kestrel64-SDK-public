// Demo 23 — impostors: a herd of T-Rexes as Y billboards (include/k64/impostor.h).
//
// The 10.9k-triangle glTF of demo 10 is baked into 8 views of 64x32 RGBA16 (k64bake.py
// impostor, IMPOSTORS in CMakeLists.txt); each of the NHERD animals is then 2 triangles with
// the view closest to the direction the camera sees it from. k64_impostor_draw groups the
// quads by view: one TMEM load per view in use, from inside the kgfx list.
//
// Self-test first: view choice against yaw and eye direction, and one impostor seen
// head-on, pixel by pixel against the alpha of its view's texels.
//
// Model: "Tyrannosaurus Rex Lowpoly" by pikabobalex, CC BY 4.0 — assets/trex/LICENSE.txt.
#include <k64/k64.h>
#include <k64/impostor.h>

#define NHERD 64

extern u8 trex_imp[];

static k64_impostor_inst_t herd[NHERD];
static k64_vtx_t herd_v[NHERD * 4] K64_ALIGN(16);
static k64_vtx_t floor_v[4] K64_ALIGN(16);
static k64_mtxfx_t mtx K64_ALIGN(16);
static k64_viewport_t vp K64_ALIGN(16);
static u64 list_buf[256] K64_ALIGN(16);
static u16 *zbuf;
static k64_mat4_t mvp;
static k64_impostor_t im;
static u32 build_us;                        // CPU time of the last k64_impostor_draw

static u32 fails;
#define CHECK(c, ...)                                \
  do {                                               \
    if (!(c)) {                                      \
      fails++;                                       \
      k64_printf("[23_impostor] FAIL " __VA_ARGS__); \
      k64_printf("\n");                              \
    }                                                \
  } while (0)

static void set_camera(k64_vec3_t eye, k64_vec3_t at) {
  k64_mat4_t proj, view;
  k64_mat4_perspective(&proj, 60.f * K64_PI / 180.f, 320.f / 240.f, 50.f, 20000.f);
  k64_mat4_lookat(&view, eye, at, (k64_vec3_t){ 0, 1, 0 });
  k64_mat4_mul(&mvp, &proj, &view);
  k64_mtx_to_fixed(&mtx, &mvp);
  k64_dcache_writeback(&mtx, sizeof mtx);
}

static void to_screen(k64_vec3_t p, float *sx, float *sy) {
  float c[4];
  for (int k = 0; k < 4; k++) c[k] = mvp.m[0][k] * p.x + mvp.m[1][k] * p.y + mvp.m[2][k] * p.z + mvp.m[3][k];
  *sx = 160.f + 160.f * c[0] / c[3];
  *sy = 120.f - 120.f * c[1] / c[3];
}

static void begin_scene(const k64_surface_t *fb, u16 bg) {
  k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, 320, 240, fb->pixels, zbuf);
  k64_rdp_clear(bg * 0x10001u, true);
}

// One list: [floor], the impostors. Returns the number of view loads.
static u32 draw(bool floor, const k64_impostor_inst_t *in, u32 n, k64_vec3_t eye) {
  k64_kgfx_list_t l;
  k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
  k64_kgfx_viewport(&l, &vp);
  k64_kgfx_mtx(&l, &mtx);
  if (floor) {
    k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF);
    k64_rdp_set_combine(K64_CC_SHADE);
    k64_kgfx_geommode(&l, K64_TRI_SHADE | K64_TRI_Z, 0, 0);
    k64_kgfx_vtx(&l, floor_v, 4, 0);
    k64_kgfx_tri2(&l, 0, 1, 2, 0, 2, 3);
    k64_kgfx_end(&l);
    k64_kgfx_run(&l);
    k64_kgfx_wait();
    k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
    k64_kgfx_viewport(&l, &vp);
    k64_kgfx_mtx(&l, &mtx);
  }
  k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF | K64_OM_PERSP_TEX | K64_OM_SAMPLE_BILINEAR |
                          K64_OM_ALPHA_COMPARE);
  k64_rdp_set_blend_color(0x00000080);
  k64_rdp_set_combine(K64_CC_TEX_SHADE);
  k64_kgfx_geommode(&l, K64_TRI_SHADE | K64_TRI_Z | K64_TRI_TEX, im.tile, K64_KGFX_CULL_BACK);
  u32 tb = k64_count();
  u32 loads = k64_impostor_draw(&l, &im, in, n, eye, 0xFFFFFFFF, herd_v);
  build_us = K64_US(k64_count() - tb);
  k64_kgfx_end(&l);
  k64_kgfx_run(&l);
  u32 st = k64_kgfx_wait();
  k64_tex_tmem_forget();
  if (st) k64_printf("[23_impostor] kgfx status %x\n", st);
  return loads;
}

static u32 scene_hash(const k64_surface_t *fb) {
  const u32 *p = K64_KSEG1((const u16 *)fb->pixels + 24 * 320);
  u32 h = 2166136261u;
  for (u32 i = 0; i < 184 * 320 / 2; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

static bool texel_opaque(const k64_tex_t *t, int u, int v) {
  return ((const u16 *)t->pixels)[v * (t->line_bytes / 2) + u] & 1;
}

static void self_test(const k64_surface_t *fb) {
  // 1. view choice: view k faces phi = 2 pi k / N from +Z toward +X, turned by the yaw
  u32 n = im.views;
  k64_impostor_inst_t a = { { 0, 0, 0 }, 0, 1 };
  CHECK(k64_impostor_view(&im, &a, (k64_vec3_t){ 0, 0, 5000 }) == 0, "view +z");
  CHECK(k64_impostor_view(&im, &a, (k64_vec3_t){ 5000, 300, 0 }) == n / 4, "view +x");
  CHECK(k64_impostor_view(&im, &a, (k64_vec3_t){ 0, -300, -5000 }) == n / 2, "view -z");
  CHECK(k64_impostor_view(&im, &a, (k64_vec3_t){ -5000, 0, 5100 }) == n - n / 8, "view -x+z");
  a.yaw = K64_PI / 2;
  CHECK(k64_impostor_view(&im, &a, (k64_vec3_t){ 5000, 0, 0 }) == 0, "yawed view +x");
  CHECK(k64_impostor_view(&im, &a, (k64_vec3_t){ 0, 0, 5000 }) == n - n / 4, "yawed view +z");
  a.yaw = 0; a.pos = (k64_vec3_t){ 3000, 0, 3000 };
  CHECK(k64_impostor_view(&im, &a, (k64_vec3_t){ 3000, 0, 9000 }) == 0, "moved view +z");

  // 2. one impostor head-on from +X (view n/4): wherever its texels are opaque away from
  //    the cut, the pixel is drawn, and wherever they are clear, the background shows
  const u16 bg = (u16)k64_rgba16(248, 0, 248, 255);
  k64_impostor_inst_t b = { { 0, 0, 0 }, 0, 1 };
  float cy = im.y0 + im.height * 0.5f, cx = im.cx, cz = im.cz;
  k64_vec3_t eye = { cx + 470.f, cy, cz };
  set_camera(eye, (k64_vec3_t){ cx, cy, cz });
  begin_scene(fb, bg);
  u32 loads = draw(false, &b, 1, eye);
  k64_rdp_wait_idle();
  k64_dcache_invalidate(fb->pixels, 320 * 240 * 2);
  CHECK(loads == 1, "loads %u", loads);
  const k64_tex_t *t = k64_impostor_tex(&im, n / 4);
  float x0, y0, x1, y1;                              // quad: left top .. right bottom on screen
  to_screen((k64_vec3_t){ cx, im.y0 + im.height, cz + im.half_w }, &x0, &y0);
  to_screen((k64_vec3_t){ cx, im.y0, cz - im.half_w }, &x1, &y1);
  u32 samples = 0, opaque = 0, bad = 0;
  for (int v = 1; v < t->height - 1; v++)
    for (int u = 1; u < t->width - 1; u++) {
      bool o = texel_opaque(t, u, v), uniform = true;
      for (int dv = -1; dv <= 1; dv++)
        for (int du = -1; du <= 1; du++) uniform &= texel_opaque(t, u + du, v + dv) == o;
      if (!uniform) continue;
      float sx = x0 + (u + 0.5f) * (x1 - x0) / t->width, sy = y0 + (v + 0.5f) * (y1 - y0) / t->height;
      u16 p = ((const u16 *)fb->pixels)[(int)sy * 320 + (int)sx];
      samples++; opaque += o;
      if ((p != bg) != o) {
        if (bad++ < 4) k64_printf("[23_impostor] texel %d,%d (%s) pixel %d,%d %04x\n", u, v, o ? "opaque" : "clear",
                                  (int)sx, (int)sy, p);
      }
    }
  CHECK(x1 - x0 > 2 * t->width && y1 - y0 > 2 * t->height, "quad %d x %d px", (int)(x1 - x0), (int)(y1 - y0));
  CHECK(opaque > samples / 10 && opaque < samples - samples / 10, "texel mix %u of %u", opaque, samples);
  CHECK(bad == 0, "%u of %u pixels disagree with the texels", bad, samples);

  // 3. three instances on three views, two of them sharing one: two loads
  k64_impostor_inst_t c[3] = { { { -600, 0, 0 }, 0, 1 }, { { 600, 0, 0 }, K64_PI, 1 }, { { 0, 0, -800 }, 0.05f, 1 } };
  eye = (k64_vec3_t){ 0, 400, 4000 };
  set_camera(eye, (k64_vec3_t){ 0, 100, 0 });
  begin_scene(fb, bg);
  loads = draw(false, c, 3, eye);
  CHECK(loads == 2, "3 instances, %u loads", loads);
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  zbuf = k64_mem_alloc(K64_ZONE_Z, 320 * 240 * 2, 64);
  k64_viewport_set(&vp, 320, 240);
  k64_viewport_wnorm(&vp, 50.f, 20000.f);
  k64_dcache_writeback(&vp, sizeof vp);
  K64_ASSERT(k64_impostor_init(&im, trex_imp, 0, 0));
  k64_printf("[23_impostor] %u views of %ux%u, quad %d x %d\n", im.views, k64_impostor_tex(&im, 0)->width,
             k64_impostor_tex(&im, 0)->height, (int)(2 * im.half_w), (int)im.height);

  const k64_surface_t *fb = k64_display_acquire();
  self_test(fb);
  k64_display_show(fb);
  k64_printf("[23_impostor] self-test %s (%u failures)\n", fails ? "FAIL" : "PASS", fails);

  u32 seed = 12345;
  for (u32 i = 0; i < NHERD; i++) {
    seed = seed * 1103515245u + 12345u;
    float jx = (float)((seed >> 8) & 1023) / 1023.f - 0.5f, jz = (float)((seed >> 18) & 1023) / 1023.f - 0.5f;
    float sc = 0.8f + (float)((seed >> 4) & 15) / 40.f;
    herd[i] = (k64_impostor_inst_t){ { ((i & 7) - 3.5f) * 1100.f + jx * 600.f, -im.y0 * sc,     // feet on y = 0
                                       ((i >> 3) - 3.5f) * 1100.f + jz * 600.f },
                                     (float)(seed & 255) * (2 * K64_PI / 256), sc };
  }
  for (int i = 0; i < 4; i++)
    floor_v[i] = (k64_vtx_t){ (s16)(i == 1 || i == 2 ? 6000 : -6000), 0, (s16)(i >= 2 ? -6000 : 6000), 0,
                              (u8)(i & 1 ? 60 : 90), (u8)(i & 2 ? 110 : 90), 50, 255, 0, 0 };
  k64_dcache_writeback(floor_v, sizeof floor_v);

  u32 list_us = 0, rdp_clk = 0, loads = 0, tris = 0;
  for (u32 frame = 0;; frame++) {
    fb = k64_display_acquire();
    u32 c0 = k64_rdp_clock();
    float a = frame * (2 * K64_PI / 512);
    k64_vec3_t eye = { 6500.f * k64_cosf(a), 1400.f + 600.f * k64_sinf(a * 3), 6500.f * k64_sinf(a) };
    set_camera(eye, (k64_vec3_t){ 0, 300, 0 });
    begin_scene(fb, (u16)k64_rgba16(110, 140, 190, 255));
    u32 t1 = k64_count();
    loads = draw(true, herd, NHERD, eye);
    list_us = K64_US(k64_count() - t1);
    tris = k64_kgfx_tri_count();
    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  23 impostor  %s", fails ? "FAIL" : "PASS");
    k64_text_printf(8, 208, 0xFFE040FF, "%u t-rex %u tris %u loads", NHERD, tris, loads);
    k64_text_printf(8, 220, 0xFFE040FF, "cpu %u us  lists %u us  rdp %u clk", build_us, list_us, rdp_clk);
    k64_rdp_wait_idle();
    rdp_clk = (k64_rdp_clock() - c0) & 0xFFFFFF;
    k64_display_show(fb);
    if ((frame & 63) == 63)
      k64_printf("[23_impostor] frame %u: %u loads, %u tris, cpu %u us (impostor_draw), lists %u us, rdp %u clk, "
                 "scene %08X\n", frame, loads, tris, build_us, list_us, rdp_clk, scene_hash(fb));
  }
}
