// kestrel64-sdk — boot logo: the Kestrel64 bird, flat as on the label, turns into a lit 3D
// model, flies a loop around the screen and lands back in the logo pose under the title.
//
// Every ROM built with k64_add_rom links this file (opt out: NO_LOGO, or -DK64_LOGO=OFF for
// the whole build) and crt0 runs k64_logo_play() between k64_runtime_init() and main().
// START, A or B skips to the fade-out. Everything the logo allocates is given back:
// main() starts with the memory zones as k64_runtime_init() left them.
//
// The model, colors, camera and the 60 Hz animation track come from tools/k64logo.py
// (logo_data.h is its output), which renders the same track for the video intro/outro.
#include <k64/k64.h>
#include <k64/logo.h>
#include "logo_data.h"

#define BG_SEG   12                       // background: glow center + 3 rings of BG_SEG
#define BG_NVTX  (1 + 3 * BG_SEG)
#define SKIP_FADE 0.4f                    // seconds

static k64_vtx_t bg_vtx[BG_NVTX] K64_ALIGN(16);
static k64_mtxfx_t bg_mtx K64_ALIGN(16);
static k64_kgfx_mtxlights_t part_ml[4];
static k64_viewport_t vp K64_ALIGN(16);
static u64 list_buf[640] K64_ALIGN(16);

// Model matrix, column vectors: m[column][row].
static void mat_from(k64_mat4_t *m, const float r[3][3], const float t[3]) {
  for (int c = 0; c < 3; c++)
    for (int k = 0; k < 3; k++) m->m[c][k] = r[k][c];
  for (int k = 0; k < 3; k++) { m->m[k][3] = 0; m->m[3][k] = t[k]; }
  m->m[3][3] = 1;
}

static void mul3(float o[3][3], const float a[3][3], const float b[3][3]) {
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) o[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}

static void rot_axis(float r[3][3], const float u[3], float a) {
  float c = k64_cosf(a), s = k64_sinf(a), t = 1 - c;
  r[0][0] = c + u[0] * u[0] * t;        r[0][1] = u[0] * u[1] * t - u[2] * s; r[0][2] = u[0] * u[2] * t + u[1] * s;
  r[1][0] = u[1] * u[0] * t + u[2] * s; r[1][1] = c + u[1] * u[1] * t;        r[1][2] = u[1] * u[2] * t - u[0] * s;
  r[2][0] = u[2] * u[0] * t - u[1] * s; r[2][1] = u[2] * u[1] * t + u[0] * s; r[2][2] = c + u[2] * u[2] * t;
}

static void quat_rot(float r[3][3], const s16 *q) {
  float w = q[0] / 16384.f, x = q[1] / 16384.f, y = q[2] / 16384.f, z = q[3] / 16384.f;
  r[0][0] = 1 - 2 * (y * y + z * z); r[0][1] = 2 * (x * y - w * z);     r[0][2] = 2 * (x * z + w * y);
  r[1][0] = 2 * (x * y + w * z);     r[1][1] = 1 - 2 * (x * x + z * z); r[1][2] = 2 * (y * z - w * x);
  r[2][0] = 2 * (x * z - w * y);     r[2][1] = 2 * (y * z + w * x);     r[2][2] = 1 - 2 * (x * x + y * y);
}

static u8 mix8(float a, float b, float t) {
  float v = a + (b - a) * t;
  return (u8)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
}

// Screen background as one mesh at the far plane: a vertical gradient (linear in y, so
// Gouraud reproduces it exactly) plus a radial glow centered on the bird's resting place.
static void build_bg(const k64_mat4_t *viewproj, float aspect, float fade) {
  float c[4] = { 0, 0, 0, 1 };
  const float rest[3] = { 0, LOGO_REST_Y, 0 };
  for (int k = 0; k < 4; k++)
    c[k] = viewproj->m[0][k] * rest[0] + viewproj->m[1][k] * rest[1] + viewproj->m[2][k] * rest[2] + viewproj->m[3][k];
  float gx = c[0] / c[3], gy = c[1] / c[3];
  float ry = 2 * 0.62f, rx = ry / aspect;          // glow radius: 0.62 screen heights
  float far = 0;                                   // reach of the outer ring: past every corner
  for (int k = 0; k < 4; k++) {
    float dx = ((k & 1) ? 1 : -1) - gx, dy = ((k & 2) ? 1 : -1) - gy;
    float d = k64_sqrtf(dx * dx / (rx * rx) + dy * dy / (ry * ry));
    if (d > far) far = d;
  }
  far = far * 1.08f / k64_cosf(K64_PI / BG_SEG);
  if (far < 1.1f) far = 1.1f;
  const float ring[3] = { 0.45f, 1.f, far }, glow[3] = { 0.42f, 0, 0 };
  for (int i = 0; i < BG_NVTX; i++) {
    float x = gx, y = gy, g = 1;
    if (i > 0) {
      int r = (i - 1) / BG_SEG, s = (i - 1) % BG_SEG;
      float a = 2 * K64_PI * s / BG_SEG;
      x = gx + rx * ring[r] * k64_cosf(a);
      y = gy + ry * ring[r] * k64_sinf(a);
      g = glow[r];
    }
    float t = (1 - y) * 0.5f;                      // 0 at the top, 1 at the bottom
    k64_vtx_t *v = &bg_vtx[i];
    v->x = (s16)(x * 1024); v->y = (s16)(y * 1024); v->z = 0; v->norm = 0;
    v->r = mix8(0, LOGO_BG[0][0] + (LOGO_BG[1][0] - LOGO_BG[0][0]) * t + LOGO_BG[2][0] * g, fade);
    v->g = mix8(0, LOGO_BG[0][1] + (LOGO_BG[1][1] - LOGO_BG[0][1]) * t + LOGO_BG[2][1] * g, fade);
    v->b = mix8(0, LOGO_BG[0][2] + (LOGO_BG[1][2] - LOGO_BG[0][2]) * t + LOGO_BG[2][2] * g, fade);
    v->a = 255; v->s = v->t = 0;
  }
  k64_dcache_writeback(bg_vtx, sizeof bg_vtx);
}

static void draw_bg(k64_kgfx_list_t *l) {
  k64_kgfx_geommode(l, K64_TRI_SHADE | K64_TRI_Z, 0, 0);
  k64_kgfx_mtx(l, &bg_mtx);
  k64_kgfx_vtx(l, bg_vtx, 1 + BG_SEG, 0);          // center + ring 0
  for (u32 s = 0; s < BG_SEG; s++)
    k64_kgfx_tri(l, 0, 1 + s, 1 + (s + 1) % BG_SEG);
  for (u32 r = 0; r < 2; r++) {                    // ring r (0..11) and ring r + 1 (12..23)
    k64_kgfx_vtx(l, &bg_vtx[1 + r * BG_SEG], 2 * BG_SEG, 0);
    for (u32 s = 0; s < BG_SEG; s++) {
      u32 n = (s + 1) % BG_SEG;
      k64_kgfx_tri2(l, s, BG_SEG + s, BG_SEG + n, s, BG_SEG + n, n);
    }
  }
}

static void set_colors(float mix) {
  for (u32 i = 0; i < LOGO_NVTX; i++) {
    logo_vtx[i].r = mix8(LOGO_SIL[0], logo_rgb[i][0], mix);
    logo_vtx[i].g = mix8(LOGO_SIL[1], logo_rgb[i][1], mix);
    logo_vtx[i].b = mix8(LOGO_SIL[2], logo_rgb[i][2], mix);
  }
  k64_dcache_writeback(logo_vtx, sizeof logo_vtx);
}

static void draw_bird(k64_kgfx_list_t *l, const k64_mat4_t *viewproj, const s16 *tr, float fade) {
  float R[3][3], S[3][3] = { { 0 } }, RS[3][3];
  quat_rot(R, tr);
  float sc = LOGO_SCALE * tr[7] / 16384.f;
  S[0][0] = S[1][1] = sc; S[2][2] = sc * tr[8] / 16384.f;
  mul3(RS, R, S);
  const float pos[3] = { tr[4] / 4096.f, tr[5] / 4096.f, tr[6] / 4096.f };
  float lm = tr[12] / 255.f, theta = tr[9] / 4096.f, tuck = tr[10] / 4096.f;
  k64_kgfx_light_t lt[3];
  for (int i = 0; i < 3; i++)
    lt[i] = (k64_kgfx_light_t){ { LOGO_LDIR[i][0], LOGO_LDIR[i][1], LOGO_LDIR[i][2] },
                                mix8(0, LOGO_LCOL[i][0] * lm, fade), mix8(0, LOGO_LCOL[i][1] * lm, fade),
                                mix8(0, LOGO_LCOL[i][2] * lm, fade) };
  u8 amb[3];
  for (int k = 0; k < 3; k++) amb[k] = mix8(0, 255 + (LOGO_AMB[k] - 255) * lm, fade);
  static const float zaxis[3] = { 0, 0, 1 };

  k64_kgfx_geommode(l, K64_TRI_SHADE | K64_TRI_Z, 0, K64_KGFX_CULL_BACK | K64_KGFX_LIGHTING);
  u32 c = 0;
  for (u32 part = 0; part < 4; part++) {
    // local hinge: wings about the shoulder axis (+theta near, -theta far), legs about the hip
    float Rl[3][3], pv[3] = { 0, 0, 0 };
    rot_axis(Rl, zaxis, 0);
    if (part == 1 || part == 2) {
      rot_axis(Rl, LOGO_AXIS, part == 1 ? theta : -theta);
      for (int k = 0; k < 3; k++) pv[k] = LOGO_HINGE[k];
    } else if (part == 3) {
      rot_axis(Rl, zaxis, tuck);
      for (int k = 0; k < 3; k++) pv[k] = LOGO_HIP[k];
    }
    // v' = RS (Rl (v - pv) + pv) + pos
    float M[3][3], RR[3][3], t[3], q[3];
    mul3(M, RS, Rl);
    mul3(RR, R, Rl);
    for (int k = 0; k < 3; k++) q[k] = pv[k] - (Rl[k][0] * pv[0] + Rl[k][1] * pv[1] + Rl[k][2] * pv[2]);
    for (int k = 0; k < 3; k++) t[k] = RS[k][0] * q[0] + RS[k][1] * q[1] + RS[k][2] * q[2] + pos[k];
    k64_mat4_t model, rot, mvp;
    mat_from(&model, M, t);
    mat_from(&rot, RR, (const float[3]){ 0, 0, 0 });
    k64_mat4_mul(&mvp, viewproj, &model);
    k64_mtx_to_fixed(&part_ml[part].mtx, &mvp);
    k64_kgfx_lights_set(&part_ml[part].lights, lt, 3, amb[0], amb[1], amb[2], &rot);
    k64_kgfx_mtx_lights(l, &part_ml[part]);
    for (; c < LOGO_NCHUNK && logo_chunk[c][0] == part; c++) {
      k64_kgfx_vtx(l, &logo_vtx[logo_chunk[c][1]], logo_chunk[c][2], 0);
      u32 t0 = logo_chunk[c][3], t1 = logo_chunk[c + 1][3];
      for (u32 i = t0; i < t1; i += 2) {
        const u8 *a = logo_tri[i];
        if (i + 1 < t1) {
          const u8 *b = logo_tri[i + 1];
          k64_kgfx_tri2(l, a[0], a[1], a[2], b[0], b[1], b[2]);
        } else {
          k64_kgfx_tri(l, a[0], a[1], a[2]);
        }
      }
    }
  }
  k64_dcache_writeback(part_ml, sizeof part_ml);
}

void k64_logo_play(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2,
                                         .dither_filter = true });
  k64_rdp_init();
  u16 *zbuf = k64_mem_alloc(K64_ZONE_Z, 320 * 240 * 2, 64);
  k64_viewport_set(&vp, 320, 240);
  k64_dcache_writeback(&vp, sizeof vp);
  k64_font_t font;
  bool have_font = k64_font_from_blob(&font, logo_font);

  const float aspect = 320.f / 240.f;
  k64_mat4_t proj, view, viewproj, bgm;
  k64_mat4_perspective(&proj, LOGO_FOVY, aspect, LOGO_NEAR, LOGO_FAR);
  k64_mat4_lookat(&view, (k64_vec3_t){ LOGO_EYE[0], LOGO_EYE[1], LOGO_EYE[2] },
                  (k64_vec3_t){ LOGO_AT[0], LOGO_AT[1], LOGO_AT[2] }, (k64_vec3_t){ 0, 1, 0 });
  k64_mat4_mul(&viewproj, &proj, &view);
  k64_mat4_identity(&bgm);                         // NDC * 1024 in, at the far end of Z
  bgm.m[0][0] = bgm.m[1][1] = 1.f / 1024; bgm.m[2][2] = 0; bgm.m[3][2] = 0.998f;
  k64_mtx_to_fixed(&bg_mtx, &bgm);
  k64_dcache_writeback(&bg_mtx, sizeof bg_mtx);

  int width = have_font ? k64_font_width(&font, LOGO_TITLE) : 0;
  u32 start = k64_count();
  float skip_at = -1, last_fade = -1, last_mix = -1;
  for (;;) {
    float t = (float)(u32)(k64_count() - start) / K64_COUNT_HZ;
    u32 f = (u32)(t * 60);
    if (f >= LOGO_NFRAMES) break;
    const s16 *tr = logo_track[f];
    float fade = tr[13] / 255.f;
    if (skip_at >= 0) {
      float k = 1 - (t - skip_at) / SKIP_FADE;
      if (k <= 0) break;
      if (k < fade) fade = k;
    }
    k64_pad_poll_start();
    const k64_surface_t *fb = k64_display_acquire();
    if (fade != last_fade) build_bg(&viewproj, aspect, fade);
    float mix = tr[11] / 255.f;
    if (mix != last_mix) set_colors(mix);
    last_fade = fade; last_mix = mix;

    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, zbuf);
    k64_rdp_clear(0, true);
    // magic-square dither + the VI dedither filter: the dark gradients band at 16 bpp without
    k64_rdp_set_other_modes((K64_OM_1C_OPAQUE & ~K64_OM_RGB_DITHER(3)) | K64_OM_RGB_DITHER(0) | K64_OM_ZBUF);
    k64_rdp_set_combine(K64_CC_SHADE);
    k64_kgfx_list_t l;
    k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
    k64_kgfx_viewport(&l, &vp);
    draw_bg(&l);
    if (fade > 0) draw_bird(&l, &viewproj, tr, fade);
    k64_kgfx_end(&l);
    k64_kgfx_run(&l);
    k64_kgfx_wait();

    float ta = tr[14] / 255.f * fade;
    if (have_font && ta > 0) {
      u32 a = (u32)(ta * 255 + 0.5f);
      int x = (320 - width) / 2, y = (int)(240 * LOGO_TITLE_Y + (1 - tr[14] / 255.f) * 240 * 0.03f);
      k64_font_set_palette(1, (LOGO_TITLE2_RGBA & ~0xFFu) | a);
      for (int d = 0; d < 2; d++)                  // twice, 1 px apart: a heavier weight
        k64_font_draw(&font, x + d, y, (LOGO_TITLE_RGBA & ~0xFFu) | a, LOGO_TITLE);
    }
    k64_pad_poll_end();
    k64_rdp_wait_idle();
    k64_display_show(fb);
    if (skip_at < 0 && (k64_pads[0].pressed & (K64_PAD_START | K64_PAD_A | K64_PAD_B))) skip_at = t;
  }
  k64_rdp_wait_idle();
  k64_tex_tmem_forget();
  k64_mem_init();
}
