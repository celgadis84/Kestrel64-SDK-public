// A side of the 16_stress comparison: the same scene written natively for tiny3d on
// libdragon (no kestrel64-sdk code involved), to measure tiny3d's own RSP pipeline.
//
// Same as demos/16_stress (sphere only): N copies of a lit, z-buffered, back-face culled
// sphere of `rows` rows x 16 vertices, each copy with its own matrix, on the same grid,
// camera and lights. The sphere is recorded once as an rspq block; each copy costs the
// frame one matrix set + one block call.
//
// Vertices are loaded the way tiny3d's 70-entry cache allows best: two rows (32 vertices)
// per load, rows cycling through four 16-entry slots, so every vertex is transformed
// once (the same once-per-vertex cost as the kgfx version).
//
// Controls (controller 1): Up / Down copies x2 / /2, C-Up / C-Down copies +1 / -1,
// Right / Left rows +1 / -1, START restarts the automatic sweep. With no input the demo
// doubles the copies every 120 frames and logs each step over ISViewer.
#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#define FB_COUNT   3
#define UNIT       256
#define SEGS       15
#define MIN_ROWS   4
#define MAX_ROWS   32
#define MAX_COPIES 1024
#define SWEEP_FRAMES 120

static T3DVertPacked verts[MAX_ROWS * 8] __attribute__((aligned(16)));

static void vertex(int k, float px, float py, float pz, float nx, float ny, float nz,
                   uint8_t r, uint8_t g, uint8_t b) {
  T3DVec3 n = {{ nx, ny, nz }};
  t3d_vec3_norm(&n);
  T3DVertPacked *p = &verts[k / 2];
  int16_t pos[3] = { (int16_t)(px * UNIT), (int16_t)(py * UNIT), (int16_t)(pz * UNIT) };
  uint32_t rgba = (uint32_t)r << 24 | (uint32_t)g << 16 | (uint32_t)b << 8 | 255;
  if (k & 1) {
    memcpy(p->posB, pos, sizeof pos); p->normB = t3d_vert_pack_normal(&n); p->rgbaB = rgba;
    p->stB[0] = p->stB[1] = 0;
  } else {
    memcpy(p->posA, pos, sizeof pos); p->normA = t3d_vert_pack_normal(&n); p->rgbaA = rgba;
    p->stA[0] = p->stA[1] = 0;
  }
}

static rspq_block_t *build_sphere(uint32_t rows, uint32_t *tris_out) {
  for (uint32_t r = 0; r < rows; r++) {
    float v = (float)r / (rows - 1), th = T3D_PI * v, s = sinf(th), c = cosf(th);
    for (uint32_t i = 0; i <= SEGS; i++) {
      float ph = 2 * T3D_PI * i / SEGS, cp = cosf(ph), sp = sinf(ph);
      uint8_t shade = (uint8_t)(160 + 70 * ((r + i) & 1));
      vertex(r * 16 + i, s * cp, c, s * sp, s * cp, c, s * sp, 230, shade, 190);
    }
  }
  data_cache_hit_writeback(verts, sizeof verts);

  uint32_t tris = 0;
  rspq_block_begin();
  for (uint32_t r = 0; r < rows; r++) {
    uint32_t slot = (r & 3) * 16;
    if ((r & 1) == 0) t3d_vert_load(&verts[r * 8], slot, r + 1 < rows ? 32 : 16);
    if (r == 0) continue;
    uint32_t cur = slot, prev = ((r - 1) & 3) * 16;
    for (uint32_t i = 0; i < SEGS; i++) {
      t3d_tri_draw(prev + i, cur + i + 1, cur + i);
      t3d_tri_draw(prev + i, prev + i + 1, cur + i + 1);
    }
    tris += 2 * SEGS;
  }
  t3d_tri_sync();
  *tris_out = tris;
  return rspq_block_end();
}

int main(void) {
  debug_init_isviewer();
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, FB_COUNT, GAMMA_NONE, FILTERS_RESAMPLE);
  rdpq_init();
  joypad_init();
  t3d_init((T3DInitParams){});
  rdpq_text_register_font(FONT_BUILTIN_DEBUG_MONO, rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO));

  T3DViewport viewport = t3d_viewport_create_buffered(FB_COUNT);
  T3DMat4FP *mats = malloc_uncached(sizeof(T3DMat4FP) * FB_COUNT * MAX_COPIES);

  uint8_t ambient[4] = { 36, 36, 48, 0xFF };
  uint8_t col0[4] = { 255, 230, 190, 0xFF }, col1[4] = { 40, 70, 170, 0xFF };
  T3DVec3 dir0 = {{ -0.5f, 0.7f, 0.6f }}, dir1 = {{ 0.9f, -0.2f, -0.3f }};
  t3d_vec3_norm(&dir0);
  t3d_vec3_norm(&dir1);

  uint32_t rows = 16, copies = 1, tris_obj = 0, sweep_t = 0, fb_idx = 0;
  bool sweep = true;
  rspq_block_t *sphere = build_sphere(rows, &tris_obj);
  uint32_t acc_frames = 0, acc_us = 0, shown_us = 0;
  uint32_t last = get_ticks_us();

  for (uint32_t frame = 0;; frame++) {
    joypad_poll();
    joypad_buttons_t p = joypad_get_buttons_pressed(JOYPAD_PORT_1);
    if (p.raw) {
      if (p.start) { sweep = true; copies = 1; sweep_t = 0; }
      else sweep = false;
      if (p.d_up && copies < MAX_COPIES) copies *= 2;
      if (p.d_down && copies > 1) copies /= 2;
      if (p.c_up && copies < MAX_COPIES) copies++;
      if (p.c_down && copies > 1) copies--;
      uint32_t nr = rows;
      if (p.d_right && rows < MAX_ROWS) nr++;
      if (p.d_left && rows > MIN_ROWS) nr--;
      if (nr != rows) {
        rspq_wait();
        rspq_block_free(sphere);
        rows = nr;
        sphere = build_sphere(rows, &tris_obj);
      }
      acc_frames = acc_us = 0;
    }

    t3d_viewport_set_perspective(&viewport, T3D_DEG_TO_RAD(60.0f), 320.f / 240.f, 1.0f, 100.0f);
    t3d_viewport_look_at(&viewport, &(T3DVec3){{ 0, 0, 7.f }}, &(T3DVec3){{ 0, 0, 0 }}, &(T3DVec3){{ 0, 1, 0 }});

    uint32_t cols = 1;
    while (cols * cols < copies) cols++;
    float cell = 7.6f / cols, ang = frame * 0.03f, s = cell * 0.42f / UNIT;
    fb_idx = (fb_idx + 1) % FB_COUNT;
    T3DMat4FP *m = mats + fb_idx * MAX_COPIES;
    for (uint32_t n = 0; n < copies; n++) {
      uint32_t cx = n % cols, cy = n / cols;
      t3d_mat4fp_from_srt_euler(&m[n], (float[3]){ s, s, s },
                                (float[3]){ 0.5f + ang * 0.6f + n * 0.21f, ang + n * 0.37f, 0 },
                                (float[3]){ (cx + 0.5f) * cell - 3.8f, 2.85f - (cy + 0.5f) * cell * 0.75f, 0 });
    }

    rdpq_attach(display_get(), display_get_zbuf());
    t3d_frame_start();
    rdpq_mode_antialias(AA_NONE);               // opaque 1-cycle like the kgfx side
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    t3d_viewport_attach(&viewport);
    t3d_screen_clear_color(RGBA32(80, 96, 192, 0xFF));
    t3d_screen_clear_depth();
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_CULL_BACK);
    t3d_light_set_ambient(ambient);
    t3d_light_set_directional(0, col0, &dir0);
    t3d_light_set_directional(1, col1, &dir1);
    t3d_light_set_count(2);

    t3d_matrix_push_pos(1);
    for (uint32_t n = 0; n < copies; n++) {
      t3d_matrix_set(&m[n], true);
      rspq_block_run(sphere);
    }
    t3d_matrix_pop(1);

    uint32_t sent = tris_obj * copies, fus = shown_us ? shown_us : 1;
    rdpq_text_printf(NULL, FONT_BUILTIN_DEBUG_MONO, 8, 16, "t3d stress  sphere  %lu rows", rows);
    rdpq_text_printf(NULL, FONT_BUILTIN_DEBUG_MONO, 8, 28, "%lu x %lu = %lu tris", copies, tris_obj, sent);
    rdpq_text_printf(NULL, FONT_BUILTIN_DEBUG_MONO, 8, 220, "frame %6lu us  %3lu fps%s", shown_us, 1000000 / fus,
                     sweep ? "  (sweep)" : "");
    rdpq_detach_show();

    uint32_t now = get_ticks_us();
    acc_us += now - last;
    last = now;
    if (++acc_frames == 16) { shown_us = acc_us / 16; acc_frames = acc_us = 0; }

    if (sweep && ++sweep_t == SWEEP_FRAMES) {
      sweep_t = 0;
      debugf("[t3d] sphere rows %lu copies %lu tris %lu  frame %lu us\n", rows, copies, sent, shown_us);
      if (shown_us > 100000 || copies == MAX_COPIES) {
        sweep = false;
        debugf("[t3d] sweep done\n");
      } else {
        copies *= 2;
        acc_frames = acc_us = 0;
      }
    }
  }
}
