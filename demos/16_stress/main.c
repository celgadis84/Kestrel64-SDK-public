// Demo 16 — geometry stress test: N copies of a parametric object, lit, z-buffered, each
// with its own matrix, to see how many triangles the machine pushes per frame.
//
// The object is recorded once as a kgfx sub-list (VTX + TRI, ending with RET); each copy
// costs the main list two commands (matrix+lights, CALL). Rows of 16 vertices alternate
// between the two halves of the vertex cache, so every vertex is transformed once.
//
// Controls (controller 1):
//   Up / Down        copies x2 / /2         C-Up / C-Down   copies +1 / -1
//   Right / Left     detail (rows) +1 / -1  A               next shape
//   B                lighting on/off        Z               z-buffer on/off
//   START            restart the automatic sweep
// With no input the demo sweeps by itself: it doubles the copies every 2 seconds and logs
// the frame cost of each step over ISViewer, until a frame takes more than 100 ms.
#include <k64/k64.h>

#define UNIT       256
#define SEGS       15                      // quads around: a row is SEGS + 1 vertices
#define MIN_ROWS   4
#define MAX_ROWS   32
#define MAX_COPIES 1024
#define SWEEP_FRAMES 120

enum { SHAPE_SPHERE, SHAPE_TORUS, SHAPE_BLOB, SHAPE_CONE, SHAPE_COUNT };
static const char *shape_name[] = { "sphere", "torus", "blob", "cone" };

static k64_vtx_t verts[MAX_ROWS][SEGS + 1] K64_ALIGN(16);
static u64 geom_list[MAX_ROWS * 9 + 4] K64_ALIGN(16);
static k64_kgfx_mtxlights_t inst[MAX_COPIES] K64_ALIGN(16);
static u64 main_list[MAX_COPIES * 2 + 16] K64_ALIGN(16);
static k64_viewport_t vp K64_ALIGN(16);

static void vertex(k64_vtx_t *v, float px, float py, float pz, float nx, float ny, float nz, u8 r, u8 g, u8 b) {
  float l = k64_sqrtf(nx * nx + ny * ny + nz * nz);
  if (l > 0) { nx /= l; ny /= l; nz /= l; }
  *v = (k64_vtx_t){ (s16)(px * UNIT), (s16)(py * UNIT), (s16)(pz * UNIT),
                    k64_kgfx_pack_normal(nx, ny, nz), r, g, b, 255, 0, 0 };
}

// Fills rows 0..rows-1 of the vertex grid for `shape`, records the sub-list, returns the
// triangles per copy.
static u32 build_shape(int shape, u32 rows) {
  for (u32 r = 0; r < rows; r++) {
    float v = (float)r / (rows - 1);
    for (u32 i = 0; i <= SEGS; i++) {
      float ph = 2 * K64_PI * i / SEGS, cp = k64_cosf(ph), sp = k64_sinf(ph);
      u8 shade = (u8)(160 + 70 * ((r + i) & 1));
      k64_vtx_t *o = &verts[r][i];
      switch (shape) {
        case SHAPE_SPHERE: {
          float th = K64_PI * v, s = k64_sinf(th), c = k64_cosf(th);
          vertex(o, s * cp, c, s * sp, s * cp, c, s * sp, 230, shade, 190);
          break;
        }
        case SHAPE_TORUS: {                  // rows go around the ring, segments around the tube
          float th = 2 * K64_PI * v, ct = k64_cosf(th), st = k64_sinf(th);
          float d = 0.65f + 0.32f * cp;
          vertex(o, d * ct, 0.32f * sp, d * st, cp * ct, sp, cp * st, shade, 200, 240);
          break;
        }
        case SHAPE_BLOB: {
          float th = K64_PI * v, s = k64_sinf(th), c = k64_cosf(th);
          float k = 1.f + 0.18f * k64_sinf(5 * th) * k64_cosf(4 * ph);
          vertex(o, k * s * cp, k * c, k * s * sp, s * cp, c, s * sp, 240, 200, shade);
          break;
        }
        default: {                           // cone: apex .. base edge, then the base disc
          u32 side = rows - 2;               // rows 0..side-1 are the side
          if (r < side) {
            float t = (float)r / (side - 1);
            vertex(o, t * cp, 1.f - 2.f * t, t * sp, 2 * cp, 1, 2 * sp, 200, shade, 120);
          } else {                           // base edge again (normal down), then the centre
            float rad = r == side ? 1.f : 0.f;
            vertex(o, rad * cp, -1.f, rad * sp, 0, -1, 0, 200, shade, 120);
          }
          break;
        }
      }
    }
  }
  k64_dcache_writeback(verts, sizeof verts);

  k64_kgfx_list_t l;
  k64_kgfx_begin(&l, geom_list, K64_ARRAY_LEN(geom_list));
  k64_kgfx_vtx(&l, verts[0], SEGS + 1, 0);
  u32 tris = 0;
  for (u32 r = 1; r < rows; r++) {
    u32 cur = (r & 1) * 16, prev = 16 - cur;
    k64_kgfx_vtx(&l, verts[r], SEGS + 1, cur);
    if (shape == SHAPE_CONE && r == rows - 2) continue;    // side edge -> base edge: no band
    for (u32 i = 0; i < SEGS; i++)
      k64_kgfx_tri2(&l, prev + i, cur + i + 1, cur + i, prev + i, prev + i + 1, cur + i + 1);
    tris += 2 * SEGS;
  }
  k64_kgfx_ret(&l);
  k64_dcache_writeback(geom_list, (u32)(l.cur - l.base) * 8);
  return tris;
}

typedef struct { u32 frames, frame_us, cpu_us, rsp_us, rdp_us, drawn; } acc_t;

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 3 });
  k64_rdp_init();
  k64_text_init();
  u16 *zbuf = k64_mem_alloc(K64_ZONE_Z, 320 * 240 * 2, 64);
  k64_viewport_set(&vp, 320, 240);
  k64_dcache_writeback(&vp, sizeof vp);

  k64_mat4_t proj, view, viewproj, unit;
  k64_mat4_perspective(&proj, 60.f * K64_PI / 180.f, 320.f / 240.f, 1.f, 100.f);
  k64_mat4_lookat(&view, (k64_vec3_t){ 0, 0, 7.f }, (k64_vec3_t){ 0, 0, 0 }, (k64_vec3_t){ 0, 1, 0 });
  k64_mat4_mul(&viewproj, &proj, &view);
  k64_mat4_scale(&unit, 1.f / UNIT, 1.f / UNIT, 1.f / UNIT);
  const k64_kgfx_light_t lt[2] = {
    { { -0.5f, 0.7f, 0.6f }, 255, 230, 190 },
    { { 0.9f, -0.2f, -0.3f }, 40, 70, 170 },
  };

  int shape = SHAPE_SPHERE;
  u32 rows = 16, copies = 1, tris_obj = 0, sweep_t = 0;
  bool lit = true, zbuf_on = true, sweep = true, dirty = true;
  acc_t acc = { 0 }, shown = { 0 };
  u32 last_show = k64_count();

  for (u32 frame = 0;; frame++) {
    k64_pad_poll();
    u16 p = k64_pads[0].pressed;
    if (p) {
      if (p & K64_PAD_START) { sweep = true; copies = 1; sweep_t = 0; }
      else sweep = false;
      if ((p & K64_PAD_UP) && copies < MAX_COPIES) copies *= 2;
      if ((p & K64_PAD_DOWN) && copies > 1) copies /= 2;
      if ((p & K64_PAD_CUP) && copies < MAX_COPIES) copies++;
      if ((p & K64_PAD_CDOWN) && copies > 1) copies--;
      if ((p & K64_PAD_RIGHT) && rows < MAX_ROWS) { rows++; dirty = true; }
      if ((p & K64_PAD_LEFT) && rows > MIN_ROWS) { rows--; dirty = true; }
      if (p & K64_PAD_A) { shape = (shape + 1) % SHAPE_COUNT; dirty = true; }
      if (p & K64_PAD_B) lit = !lit;
      if (p & K64_PAD_Z) zbuf_on = !zbuf_on;
      acc = (acc_t){ 0 };
    }
    if (dirty) { tris_obj = build_shape(shape, rows); dirty = false; }

    const k64_surface_t *fb = k64_display_acquire();
    u32 t0 = k64_count(), pb0 = DPC_PIPEBUSY;
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, zbuf_on ? zbuf : NULL);
    k64_rdp_clear(k64_rgba16(10, 12, 24, 255) * 0x10001u, zbuf_on);
    k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | (zbuf_on ? K64_OM_ZBUF : 0));
    k64_rdp_set_combine(K64_CC_SHADE);

    // copies on a square grid in the z = 0 plane, each spinning
    u32 cols = 1;
    while (cols * cols < copies) cols++;
    float cell = 7.6f / cols, ang = frame * 0.03f, s = cell * 0.42f;
    k64_kgfx_list_t l;
    k64_kgfx_begin(&l, main_list, K64_ARRAY_LEN(main_list));
    k64_kgfx_viewport(&l, &vp);
    k64_kgfx_geommode(&l, K64_TRI_SHADE | (zbuf_on ? K64_TRI_Z : 0), 0,
                      K64_KGFX_CULL_BACK | (lit ? K64_KGFX_LIGHTING : 0));
    for (u32 n = 0; n < copies; n++) {
      u32 cx = n % cols, cy = n / cols;
      k64_mat4_t ry, rx, rot, sc, tr, model, tmp, m;
      k64_mat4_rotate_y(&ry, ang + n * 0.37f);
      k64_mat4_rotate_x(&rx, 0.5f + ang * 0.6f + n * 0.21f);
      k64_mat4_mul(&rot, &ry, &rx);
      k64_mat4_scale(&sc, s, s, s);
      k64_mat4_translate(&tr, (cx + 0.5f) * cell - 3.8f, 2.85f - (cy + 0.5f) * cell * 0.75f, 0);
      k64_mat4_mul(&tmp, &rot, &sc);
      k64_mat4_mul(&model, &tr, &tmp);
      k64_mat4_mul(&tmp, &viewproj, &model);
      k64_mat4_mul(&m, &tmp, &unit);
      k64_mtx_to_fixed(&inst[n].mtx, &m);
      if (lit) {
        k64_kgfx_lights_set(&inst[n].lights, lt, 2, 36, 36, 48, &model);
        k64_kgfx_mtx_lights(&l, &inst[n]);
      } else {
        k64_kgfx_mtx(&l, &inst[n].mtx);
      }
      k64_kgfx_call(&l, geom_list);
    }
    k64_kgfx_end(&l);
    k64_dcache_writeback(inst, copies * sizeof inst[0]);
    k64_kgfx_run(&l);
    u32 t1 = k64_count();
    u32 st = k64_kgfx_wait();
    u32 t2 = k64_count();
    if (st) k64_printf("[16] kgfx status %x\n", st);
    u32 drawn = k64_kgfx_tri_count();

    u32 sent = tris_obj * copies, fus = shown.frame_us ? shown.frame_us : 1;
    k64_text_printf(8, 8, 0xFFFFFFFF, "16 stress  %s  %u rows  %s%s", shape_name[shape], rows,
                    lit ? "lit" : "unlit", zbuf_on ? "  zbuf" : "");
    k64_text_printf(8, 20, 0xFFFFFFFF, "%u x %u = %u tris", copies, tris_obj, sent);
    k64_text_printf(8, 184, 0xFFE040FF, "drawn %u (front-facing)", shown.drawn);
    k64_text_printf(8, 196, 0xFFE040FF, "frame %6u us  %3u.%u fps", shown.frame_us, 1000000 / fus, 10000000 / fus % 10);
    k64_text_printf(8, 208, 0xFFE040FF, "cpu %5u  rsp %5u  rdp %5u us", shown.cpu_us, shown.rsp_us, shown.rdp_us);
    k64_text_printf(8, 220, 0xFFE040FF, "%u k tris/s  rsp %u ns/tri%s", (u32)((u64)shown.drawn * 1000 / fus),
                    sent ? shown.rsp_us * 1000 / sent : 0, sweep ? "  (sweep)" : "");
    k64_rdp_wait_idle();
    u32 rdp_clk = (DPC_PIPEBUSY - pb0) & 0xFFFFFF;        // RDP pipeline busy clocks (62.5 MHz)
    k64_display_show(fb);
    u32 now = k64_count();

    // cpu = building the list; rsp = until the microcode is done (from the frame start);
    // rdp = clocks the RDP pipeline was busy over the frame (DPC_PIPEBUSY)
    acc.frames++;
    acc.frame_us += K64_US(now - last_show);
    acc.cpu_us += K64_US(t1 - t0);
    acc.rsp_us += K64_US(t2 - t0);
    acc.rdp_us += rdp_clk * 2 / 125;
    acc.drawn += drawn;
    last_show = now;
    if (acc.frames == 16) {
      shown = (acc_t){ 16, acc.frame_us / 16, acc.cpu_us / 16, acc.rsp_us / 16, acc.rdp_us / 16, acc.drawn / 16 };
      acc = (acc_t){ 0 };
    }

    if (sweep && ++sweep_t == SWEEP_FRAMES) {
      sweep_t = 0;
      k64_printf("[16] %s rows %u copies %u tris %u drawn %u  frame %u us  cpu %u rsp %u rdp %u us  %u ktris/s  rsp %u ns/tri\n",
                 shape_name[shape], rows, copies, sent, shown.drawn, shown.frame_us, shown.cpu_us,
                 shown.rsp_us, shown.rdp_us, (u32)((u64)shown.drawn * 1000 / (shown.frame_us ? shown.frame_us : 1)),
                 shown.rsp_us * 1000 / sent);
      if (shown.frame_us > 100000 || copies == MAX_COPIES) {
        sweep = false;
        k64_printf("[16] sweep done\n");
      } else {
        copies *= 2;
        acc = (acc_t){ 0 };
      }
    }
  }
}
