// Demo 07 — kgfx texture coordinate generation: a chrome torus and two spheres, spherically
// environment mapped on the RSP (s, t from the view-space normal) and lit. The 32x32 RGBA16
// environment (sky, horizon, ground, sun) is drawn by the CPU at start-up.
//
// A cycles the modes: env map x lighting, env map only, lighting only. Every 256 frames it
// moves on by itself, so a headless run logs the RSP cost of each.
#include <k64/k64.h>

#define UNIT     256                      // model units per world unit
#define TMAJ     24                       // torus: segments around the main ring
#define TMIN     12                       // segments around the tube (ring = TMIN + 1 vertices)
#define SSEG     12                       // sphere: segments around
#define SRING    9                        // sphere: rings pole to pole
#define NOBJ     3
#define ENV      32                       // environment texture size

static k64_vtx_t torus_v[TMAJ + 1][TMIN + 1] K64_ALIGN(16);
static k64_vtx_t sphere_v[SRING][SSEG + 1] K64_ALIGN(16);
static u16 env_px[ENV * ENV] K64_ALIGN(16);
static k64_mtxfx_t mtx[NOBJ] K64_ALIGN(16);
static k64_kgfx_lights_t lights[NOBJ];
static k64_kgfx_texgen_t texgen[NOBJ];
static k64_viewport_t vp K64_ALIGN(16);
static u64 list_buf[1024] K64_ALIGN(16);

enum { MODE_ENV_LIT, MODE_ENV, MODE_LIT, MODE_COUNT };
static const char *const mode_name[MODE_COUNT] = { "env map + light", "env map", "light" };

static void build_torus(void) {
  for (int u = 0; u <= TMAJ; u++) {
    float a = 2 * K64_PI * u / TMAJ, ca = k64_cosf(a), sa = k64_sinf(a);
    for (int v = 0; v <= TMIN; v++) {
      float b = 2 * K64_PI * v / TMIN, cb = k64_cosf(b), sb = k64_sinf(b);
      float nx = cb * ca, ny = sb, nz = cb * sa;          // tube normal
      float r = 1.f + 0.42f * cb;
      torus_v[u][v] = (k64_vtx_t){ (s16)(r * ca * UNIT), (s16)(0.42f * sb * UNIT), (s16)(r * sa * UNIT),
                                   k64_kgfx_pack_normal(nx, ny, nz), 235, 220, 200, 255, 0, 0 };
    }
  }
}

static void build_sphere(void) {
  for (int r = 0; r < SRING; r++) {
    float th = K64_PI * r / (SRING - 1), y = k64_cosf(th), s = k64_sinf(th);
    for (int i = 0; i <= SSEG; i++) {
      float ph = 2 * K64_PI * i / SSEG, x = s * k64_cosf(ph), z = s * k64_sinf(ph);
      sphere_v[r][i] = (k64_vtx_t){ (s16)(x * UNIT), (s16)(y * UNIT), (s16)(z * UNIT),
                                    k64_kgfx_pack_normal(x, y, z), 200, 215, 240, 255, 0, 0 };
    }
  }
}

// Sphere map: texel (x, y) holds what a view-space normal (nx, -ny, nz) sees.
static void build_env(void) {
  const float sun[3] = { -0.45f, 0.6f, 0.66f };
  for (int y = 0; y < ENV; y++)
    for (int x = 0; x < ENV; x++) {
      float nx = (x + 0.5f) / (ENV / 2) - 1, ny = 1 - (y + 0.5f) / (ENV / 2);
      float r2 = nx * nx + ny * ny;
      if (r2 > 1) { float k = 1.f / k64_sqrtf(r2); nx *= k; ny *= k; r2 = 1; }
      float nz = k64_sqrtf(1 - r2);
      float cr, cg, cb;
      if (ny > -0.08f) {                                   // sky: deep blue to pale at the horizon
        float h = ny < 0 ? 0 : ny;
        cr = 200 - 150 * h; cg = 215 - 110 * h; cb = 255 - 40 * h;
      } else {                                             // ground: dark warm, lighter far away
        float h = -ny;
        cr = 110 - 60 * h; cg = 80 - 45 * h; cb = 50 - 25 * h;
      }
      float d = nx * sun[0] + ny * sun[1] + nz * sun[2];
      if (d > 0) {
        float sp = d * d; sp *= sp; sp *= sp; sp *= sp;    // d^16
        cr += 255 * sp; cg += 240 * sp; cb += 200 * sp;
      }
      cr = cr > 255 ? 255 : cr; cg = cg > 255 ? 255 : cg; cb = cb > 255 ? 255 : cb;
      env_px[y * ENV + x] = (u16)k64_rgba16((u8)cr, (u8)cg, (u8)cb, 255);
    }
}

static void object_model(k64_mat4_t *model, u32 o, float ang) {
  k64_mat4_t rx, ry, rz, tr, tmp, sc;
  if (o == 0) {
    k64_mat4_rotate_x(&rx, 0.9f + 0.4f * k64_sinf(ang * 0.5f));
    k64_mat4_rotate_y(&ry, ang * 0.7f);
    k64_mat4_rotate_z(&rz, ang * 0.3f);
    k64_mat4_mul(&tmp, &ry, &rx);
    k64_mat4_mul(model, &tmp, &rz);
    k64_mat4_scale(&sc, 1.6f, 1.6f, 1.6f);
    k64_mat4_mul(&tmp, model, &sc);
    k64_mat4_translate(&tr, 0, 0, -1.f);
    k64_mat4_mul(model, &tr, &tmp);
    return;
  }
  float a = ang * 0.9f + (o - 1) * K64_PI;
  k64_mat4_rotate_y(&ry, ang);
  k64_mat4_scale(&sc, 0.7f, 0.7f, 0.7f);
  k64_mat4_mul(&tmp, &ry, &sc);
  k64_mat4_translate(&tr, 3.4f * k64_cosf(a), 0.8f * k64_sinf(a * 2), -1.f + 2.4f * k64_sinf(a));
  k64_mat4_mul(model, &tr, &tmp);
}

// A strip of quads between two vertex rings in the two cache halves.
static void band(k64_kgfx_list_t *l, u32 prev, u32 cur, u32 n) {
  for (u32 i = 0; i < n; i++)
    k64_kgfx_tri2(l, prev + i, cur + i + 1, cur + i, prev + i, prev + i + 1, cur + i + 1);
}

static void draw(const k64_mat4_t *view, const k64_mat4_t *viewproj, float ang, u32 mode) {
  k64_kgfx_light_t lt[2] = {
    { { -0.5f, 0.8f, 0.5f }, 255, 240, 215 },
    { { 0.8f, -0.3f, -0.4f }, 50, 80, 160 },
  };
  bool lit = mode != MODE_ENV, env = mode != MODE_LIT;
  k64_kgfx_list_t l;
  k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
  k64_kgfx_viewport(&l, &vp);
  k64_kgfx_geommode(&l, K64_TRI_SHADE | K64_TRI_Z | (env ? K64_TRI_TEX : 0), 0,
                    K64_KGFX_CULL_BACK | (lit ? K64_KGFX_LIGHTING : 0) | (env ? K64_KGFX_TEXGEN : 0));
  for (u32 o = 0; o < NOBJ; o++) {
    k64_mat4_t model, mv, mvp, sc, m;
    object_model(&model, o, ang);
    k64_mat4_scale(&sc, 1.f / UNIT, 1.f / UNIT, 1.f / UNIT);
    k64_mat4_mul(&mvp, viewproj, &model);
    k64_mat4_mul(&m, &mvp, &sc);
    k64_mtx_to_fixed(&mtx[o], &m);
    k64_kgfx_mtx(&l, &mtx[o]);
    if (lit) {
      k64_kgfx_lights_set(&lights[o], lt, 2, 40, 40, 52, &model);
      k64_kgfx_lights(&l, &lights[o]);
    }
    if (env) {
      k64_mat4_mul(&mv, view, &model);
      k64_kgfx_texgen_set(&texgen[o], &mv, ENV, ENV);
      k64_kgfx_texgen(&l, &texgen[o]);
    }
    if (o == 0) {
      k64_kgfx_vtx(&l, torus_v[0], TMIN + 1, 0);
      for (int u = 1; u <= TMAJ; u++) {
        u32 cur = (u32)(u & 1) * 16;
        k64_kgfx_vtx(&l, torus_v[u], TMIN + 1, cur);
        band(&l, 16 - cur, cur, TMIN);
      }
    } else {
      k64_kgfx_vtx(&l, sphere_v[0], SSEG + 1, 0);
      for (int r = 1; r < SRING; r++) {
        u32 cur = (u32)(r & 1) * 16;
        k64_kgfx_vtx(&l, sphere_v[r], SSEG + 1, cur);
        band(&l, 16 - cur, cur, SSEG);
      }
    }
  }
  k64_kgfx_end(&l);
  k64_dcache_writeback(mtx, sizeof mtx);
  k64_dcache_writeback(lights, sizeof lights);
  k64_dcache_writeback(texgen, sizeof texgen);
  k64_kgfx_run(&l);
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  u16 *zbuf = k64_mem_alloc(K64_ZONE_Z, 320 * 240 * 2, 64);
  k64_viewport_set(&vp, 320, 240);
  build_torus();
  build_sphere();
  build_env();
  k64_dcache_writeback(&vp, sizeof vp);
  k64_dcache_writeback(torus_v, sizeof torus_v);
  k64_dcache_writeback(sphere_v, sizeof sphere_v);
  k64_dcache_writeback(env_px, sizeof env_px);
  k64_tex_t env = { .width = ENV, .height = ENV, .fmt = K64_FMT_RGBA, .siz = K64_SIZ_16,
                    .line_bytes = ENV * 2, .tmem_line = ENV * 2 / 8, .pixels = env_px };

  k64_mat4_t proj, view, viewproj;
  k64_mat4_perspective(&proj, 60.f * K64_PI / 180.f, 320.f / 240.f, 1.f, 100.f);
  k64_mat4_lookat(&view, (k64_vec3_t){ 0, 2.f, 7.f }, (k64_vec3_t){ 0, 0, -1.f }, (k64_vec3_t){ 0, 1, 0 });
  k64_mat4_mul(&viewproj, &proj, &view);

  u32 mode = MODE_ENV_LIT;
  u32 cpu_us = 0, rsp_us = 0, rdp_clk = 0, tris = 0;
  for (u32 frame = 0;; frame++) {
    k64_pad_poll_start();
    const k64_surface_t *fb = k64_display_acquire();
    u32 t0 = k64_count(), c0 = k64_rdp_clock();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, zbuf);
    k64_rdp_clear(k64_rgba16(16, 18, 30, 255) * 0x10001u, true);
    k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF | K64_OM_PERSP_TEX | K64_OM_SAMPLE_BILINEAR);
    if (mode == MODE_LIT) {
      k64_rdp_set_combine(K64_CC_SHADE);
    } else {
      k64_rdp_set_combine(mode == MODE_ENV ? K64_CC_TEX : K64_CC_TEX_SHADE);
      k64_rdp_load_tex(0, 0, &env, K64_TEX_CLAMP, K64_TEX_CLAMP);
    }

    draw(&view, &viewproj, frame * 0.02f, mode);
    u32 t1 = k64_count();
    u32 st = k64_kgfx_wait();
    rsp_us = K64_US(k64_count() - t1);
    cpu_us = K64_US(t1 - t0);
    tris = k64_kgfx_tri_count();
    if (st) k64_printf("[07] kgfx status %x\n", st);

    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  07 %s", mode_name[mode]);
    k64_text_printf(8, 208, 0xFFE040FF, "tris %u  cpu %5u us  rsp %5u us", tris, cpu_us, rsp_us);
    k64_text_printf(8, 220, 0xFFE040FF, "rdp %6u clk   A: mode", rdp_clk);
    k64_pad_poll_end();
    k64_rdp_wait_idle();
    rdp_clk = (k64_rdp_clock() - c0) & 0xFFFFFF;
    k64_display_show(fb);
    if ((frame & 63) == 63)
      k64_printf("[07] frame %u mode %s tris %u cpu %u us rsp %u us rdp %u clk\n", frame,
                 mode_name[mode], tris, cpu_us, rsp_us, rdp_clk);
    if ((k64_pads[0].pressed & K64_PAD_A) || (frame & 255) == 255) mode = (mode + 1) % MODE_COUNT;
  }
}
